#include "wavm/openmp/openmp.h"

#include <future>
#include <WAVM/Platform/Thread.h>
#include <WAVM/Runtime/Runtime.h>
#include <WAVM/Runtime/Intrinsics.h>

#include <faabric/state/StateKeyValue.h>
#include <faabric/scheduler/Scheduler.h>
#include <faabric/util/timing.h>
#include <wavm/openmp/Level.h>
#include <wavm/openmp/ThreadState.h>
#include <wavm/OMPThreadPool.h>
#include <wavm/WAVMWasmModule.h>

using namespace WAVM;

namespace wasm {
    using namespace openmp;

    /**
     * Performs actual static assignment
     */
    template<typename T>
    void for_static_init(I32 schedule, I32 *lastIter, T *lower, T *upper, T *stride, T incr, T chunk);

    /**
     * Function used to spawn OMP threads. Will be called from within a thread
     * (hence needs to set up its own TLS)
     */
    I64 ompThreadEntryFunc(void *threadArgsPtr) {
        auto args = *reinterpret_cast<LocalThreadArgs *>(threadArgsPtr);
        // Set up various TLS
        setTLS(args.tid, args.level);
        setExecutingModule(args.parentModule);
        setExecutingCall(args.parentCall);
        return getExecutingWAVMModule()->executeThreadLocally(args.spec);
    }

    /**
     * Performs actual static assignment
     */
    template<typename T>
    void for_static_init(I32 schedule, I32 *lastIter, T *lower, T *upper, T *stride, T incr, T chunk);


    /**
     * @return the thread number, within its team, of the thread executing the function.
     */
    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "omp_get_thread_num", I32, omp_get_thread_num) {
        faabric::util::getLogger()->debug("S - omp_get_thread_num");
        return thisThreadNumber;
    }

    /**
     * @return the number of threads currently in the team executing the parallel region from
     * which it is called.
     */
    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "omp_get_num_threads", I32, omp_get_num_threads) {
        faabric::util::getLogger()->debug("S - omp_get_num_threads");
        return thisLevel->numThreads;
    }

    /**
     * @return the maximum number of threads that can be used to form a new team if a parallel
     * region without a num_threads clause is encountered.
     */
    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "omp_get_max_threads", I32, omp_get_max_threads) {
        faabric::util::getLogger()->debug("S - omp_get_max_threads");
        return thisLevel->get_next_level_num_threads();
    }

    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "omp_get_level", I32, omp_get_level) {
        faabric::util::getLogger()->debug("S - omp_get_level");
        return thisLevel->depth;
    }

    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "omp_get_max_active_levels", I32, omp_get_max_active_levels) {
        faabric::util::getLogger()->debug("S - omp_get_max_active_levels");
        return thisLevel->maxActiveLevel;
    }

    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "omp_set_max_active_levels", void, omp_set_max_active_levels, I32 level) {
        const std::shared_ptr<spdlog::logger> &logger = faabric::util::getLogger();
        logger->debug("S - omp_set_max_active_levels {}", level);
        if (level < 0) {
            logger->warn("Trying to set active level with a negative number {}", level);
            return;
        }
        thisLevel->maxActiveLevel = level;
    }

    /**
     * Synchronization point at which threads in a parallel region will not execute beyond
     * the omp barrier until all other threads in the team complete all explicit tasks in the region.
     * Concepts used for reductions and split barriers.
     * @param loc
     * @param global_tid
     */
    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "__kmpc_barrier", void, __kmpc_barrier, I32 loc, I32 globalTid) {
        faabric::util::getLogger()->debug("S - __kmpc_barrier {} {}", loc, globalTid);

        if (!thisLevel->barrier || thisLevel->numThreads <= 1) {
            return;
        }

        thisLevel->barrier->wait();
    }

    /**
     * Enter code protected by a `critical` construct. This function blocks until the thread can enter the critical section.
     * @param loc  source location information.
     * @param global_tid  global thread number.
     * @param crit identity of the critical section. This could be a pointer to a lock
        associated with the critical section, or some other suitably unique value.
        The lock is not used because Faasm needs to control the locking mechanism for the team.
     */
    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "__kmpc_critical", void, __kmpc_critical, I32 loc, I32 globalTid, I32 crit) {
        faabric::util::getLogger()->debug("S - __kmpc_critical {} {} {}", loc, globalTid, crit);
        if (thisLevel->numThreads > 1) {
            thisLevel->criticalSection.lock();
        }
    }

    /**
     * Exits code protected by a `critical` construct, releasing the held lock. This function blocks until the thread can enter the critical section.
     * @param loc  source location information.
     * @param global_tid  global thread number.
     * @param crit compiler lock. See __kmpc_critical for more information
     */
    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "__kmpc_end_critical", void, __kmpc_end_critical, I32 loc, I32 globalTid,
                                   I32 crit) {
        faabric::util::getLogger()->debug("S - __kmpc_end_critical {} {} {}", loc, globalTid, crit);
        if (thisLevel->numThreads > 1) {
            thisLevel->criticalSection.unlock();
        }
    }

    /**
     * The omp flush directive identifies a point at which the compiler ensures that all threads in a parallel region
     * have the same view of specified objects in memory. Like clang here we use a fence, but this semantic might
     * not be suited for distributed work. People doing distributed DSM OMP synch the page there.
     * @param loc Source location info
     */
    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "__kmpc_flush", void, __kmpc_flush, I32 loc) {
        faabric::util::getLogger()->debug("S - __kmpc_flush{}", loc);

        // Full memory fence, a bit overkill maybe for Wasm
        __sync_synchronize();
    }

    /**
     * No implied BARRIER exists on either entry to or exit from the MASTER section.
     * @param loc  source location information.
     * @param global_tid  global thread number.
     * @return 1 if this thread should execute the <tt>master</tt> block, 0 otherwise.
     *
     * Faasm: at the moment we only ensure the MASTER section is ran only once but do
     * not handle properly assigning to the master section. Support for better gtid and
     * teams will come. This is called by all threads with same GTID, which is not
     * what the native code does.
     */
    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "__kmpc_master", I32, __kmpc_master, I32 loc, I32 globalTid) {
        faabric::util::getLogger()->debug("S - __kmpc_master {} {}", loc, globalTid);
        return thisThreadNumber == 0 ? 1 : 0;
    }

    /**
     * Only called by the thread executing the master region.
     * @param loc  source location information.
     * @param global_tid  global thread number .
     */
    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "__kmpc_end_master", void, __kmpc_end_master, I32 loc, I32 globalTid) {
        faabric::util::getLogger()->debug("S - __kmpc_end_master {} {}", loc, globalTid);
        WAVM_ASSERT(thisThreadNumber == 0)
    }

    /**
     * Test whether to execute a single construct. There are no implicit barriers in the two "single" calls,
     rather the compiler should introduce an explicit barrier if it is required.
     * @param loc
     * @param globalTid
     * @return 1 if this thread should execute the single construct, zero otherwise.
     */
    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "__kmpc_single", I32, __kmpc_single, I32 loc, I32 globalTid) {
        faabric::util::getLogger()->debug("S - __kmpc_single {} {}", loc, globalTid);
        return thisThreadNumber == 0 ? 1 : 0;
    }

    /**
     * Test whether to execute a single construct. There are no implicit barriers in the two "single" calls,
     rather the compiler should introduce an explicit barrier if it is required.
     * @param loc
     * @param globalTid
     * @return 1 if this thread should execute the single construct, zero otherwise.
     */
    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "__kmpc_end_single", void, __kmpc_end_single, I32 loc, I32 globalTid) {
        faabric::util::getLogger()->debug("S - __kmpc_end_single {} {}", loc, globalTid);
        WAVM_ASSERT(thisThreadNumber == 0)
    }

    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "__kmpc_push_num_threads", void, __kmpc_push_num_threads,
                                   I32 loc, I32 globalTid, I32 numThreads) {
        faabric::util::getLogger()->debug("S - __kmpc_push_num_threads {} {} {}", loc, globalTid, numThreads);
        if (numThreads > 0) {
            pushedNumThreads = numThreads;
        }
    }

    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "omp_set_num_threads", void, omp_set_num_threads, I32 numThreads) {
        faabric::util::getLogger()->debug("S - omp_set_num_threads {}", numThreads);
        if (numThreads > 0) {
            wantedNumThreads = numThreads;
        }
    }

    /**
     * If the runtime is called once, equivalent of calling get_thread_num() at the deepest
     * @param loc The usual...
     * @return
     */
    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "__kmpc_global_thread_num", I32, __kmpc_global_thread_num, I32 loc) {
        faabric::util::getLogger()->debug("S - __kmpc_global_thread_num {}", loc);
        return thisThreadNumber; // Might be wrong if called at depth 1 while another thread at depths 1 has forked
    }

    /**
     * The "real" version of this function is implemented in the openmp source at
     * openmp/runtime/src/kmp_csupport.cpp. This in turn calls __kmp_fork_call which
     * does the real heavy lifting (see openmp/runtime/src/kmp_runtime.cpp)
     *
     * @param locPtr pointer to the source location info (type ident_t)
     * @param argc number of arguments to pass to the microtask
     * @param microtaskPtr function pointer for the microtask itself (microtask_t)
     * @param argsPtr pointer to the arguments for the microtask (if applicable)
     *
     * The microtask function takes two or more arguments:
     * 1. The thread ID within its current team
     * 2. The number of non-global shared variables it has access to
     * 3+. Separate arguments, each of which is a pointer to one of the non-global shared variables
     */
    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "__kmpc_fork_call", void, __kmpc_fork_call, I32 locPtr, I32 argc,
                                   I32 microtaskPtr, I32 argsPtr) {
        const std::shared_ptr<spdlog::logger> &logger = faabric::util::getLogger();
        logger->debug("S - __kmpc_fork_call {} {} {} {}", locPtr, argc, microtaskPtr, argsPtr);

        WAVMWasmModule *parentModule = getExecutingWAVMModule();
        Runtime::Memory *memoryPtr = parentModule->defaultMemory;
        faabric::Message *parentCall = getExecutingCall();

        // Retrieve the microtask function from the table
        Runtime::Function *func = Runtime::asFunction(
                Runtime::getTableElement(getExecutingWAVMModule()->defaultTable, microtaskPtr));

#ifdef OPENMP_FORK_REDIS_TRACE
        const faabric::utilTimePoint iterationTp = faabric::utilstartTimer();
#endif

        // Set up number of threads for next level
        int nextNumThreads = thisLevel->get_next_level_num_threads();
        pushedNumThreads = -1; // Resets for next push

        if (0 > thisLevel->userDefaultDevice) {

            std::vector<int> chainedThreads;
            chainedThreads.reserve(nextNumThreads);

            std::string activeSnapshotKey;
            size_t threadSnapshotSize;

            // TODO - Implement different cases like calling parallel section in a loop
            // Or calling several parallel sections to cache snapshots as much as possible
            int32_t callId = std::rand() % 100'000;
            activeSnapshotKey = fmt::format("fork_{}", callId);
            threadSnapshotSize = parentModule->snapshotToState(activeSnapshotKey);

            faabric::scheduler::Scheduler &sch = faabric::scheduler::getScheduler();

            const faabric::Message *originalCall = getExecutingCall();
            const std::string origStr = faabric::util::funcToString(*originalCall, false);

            U32 *nativeArgs = Runtime::memoryArrayPtr<U32>(memoryPtr, argsPtr, argc);
            // Create the threads (messages) themselves
            for (int threadNum = 0; threadNum < nextNumThreads; threadNum++) {
                faabric::Message call = faabric::util::messageFactory(originalCall->user(), originalCall->function());
                call.set_isasync(true);
                for (int argIdx = argc - 1; argIdx >= 0; argIdx--) {
                    call.add_ompfunctionargs(nativeArgs[argIdx]);
                }

                // Snapshot details
                call.set_snapshotkey(activeSnapshotKey);
                call.set_snapshotsize(threadSnapshotSize);
                call.set_funcptr(microtaskPtr);
                call.set_ompthreadnum(threadNum);
                call.set_ompnumthreads(nextNumThreads);
                thisLevel->snapshot_parent(call);
                const std::string chainedStr = faabric::util::funcToString(call, false);
                sch.callFunction(call);

                logger->debug("Forked thread {} ({}) -> {} {}(*{}) ({})", origStr, faabric::util::getSystemConfig().endpointHost, chainedStr,
                              microtaskPtr, argsPtr, call.scheduledhost());
                chainedThreads[threadNum] = call.id();
            }

            I64 numErrors = 0;

            for (int threadNum = 0; threadNum < nextNumThreads; threadNum++) {
                int callTimeoutMs = faabric::util::getSystemConfig().chainedCallTimeout;
                logger->info("Waiting for thread #{} with call id {} with a timeout of {}", threadNum,
                             chainedThreads[threadNum], callTimeoutMs);

                int returnCode = 1;
                try {
                    const faabric::Message result = sch.getFunctionResult(chainedThreads[threadNum], callTimeoutMs);
                    returnCode = result.returnvalue();
                } catch (faabric::redis::RedisNoResponseException &ex) {
                    faabric::util::getLogger()->error("Timed out waiting for chained call: {}", chainedThreads[threadNum]);
                } catch (std::exception &ex) {
                    faabric::util::getLogger()->error("Non-timeout exception waiting for chained call: {}", ex.what());
                }

                if (returnCode) {
                    numErrors++;
                }
            }

            if (numErrors) {
                throw std::runtime_error(fmt::format("{} OMP threads have exited with errors", numErrors));
            }

            logger->debug("Distributed Fork finished successfully");
        } else { // Single host

            // Set up new level
            auto nextLevel = std::make_shared<SingleHostLevel>(thisLevel, nextNumThreads);

            // Safety - must ensure thread arguments lifetime is longer than the threads
            // And that this container is not moved (reserve).
            std::vector<std::vector<IR::UntaggedValue>> microtaskArgs;
            microtaskArgs.reserve(nextNumThreads);

            std::vector<std::future<I64>> threadsFutures;
            threadsFutures.reserve(nextNumThreads);

            // Build up arguments
            for (int threadNum = 0; threadNum < nextNumThreads; threadNum++) {
                // Note - these arguments are the thread number followed by the number of
                // shared variables, then the pointers to those shared variables
                microtaskArgs.push_back({threadNum, argc});
                if (argc > 0) {
                    // Get pointer to start of arguments in host memory
                    U32 *pointers = Runtime::memoryArrayPtr<U32>(memoryPtr, argsPtr, argc);
                    for (int argIdx = 0; argIdx < argc; argIdx++) {
                        microtaskArgs[threadNum].emplace_back(pointers[argIdx]);
                    }
                }

                // Arguments for spawning the thread
                // NOTE - CLion auto-format insists on this layout... and clangd really hates C99 extensions
                LocalThreadArgs threadArgs = {
                        .tid = threadNum,
                        .level = nextLevel,
                        .parentModule = parentModule,
                        .parentCall = parentCall,
                        .spec = {
                                .contextRuntimeData = contextRuntimeData,
                                .func = func,
                                .funcArgs = microtaskArgs[threadNum].data(),
                        }
                };

                threadsFutures.emplace_back(parentModule->getOMPPool()->runThread(std::move(threadArgs)));
            }

            // Await all threads
            I64 numErrors = 0;
            for (auto &f : threadsFutures) {
                numErrors += f.get();
            }

            if (numErrors) {
                throw std::runtime_error(fmt::format("{} OMP threads have exited with errors", numErrors));
            }
        }

#ifdef OPENMP_FORK_REDIS_TRACE
        const long distributedIterationTime = faabric::utilgetTimeDiffNanos(iterationTp);
        logger->warn("{}, Wasm local,{}", nextNumThreads, distributedIterationTime);
#endif
    }

    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "faasmp_incrby", I64, __faasmp_incrby, I32 keyPtr, I64 value) {
        const std::shared_ptr<spdlog::logger> &logger = faabric::util::getLogger();
        logger->debug("S - __faasmp_incryby {} {}", keyPtr, value);

        Runtime::Memory *memoryPtr = getExecutingWAVMModule()->defaultMemory;
        std::string key{&Runtime::memoryRef<char>(memoryPtr, (Uptr) keyPtr)};
        faabric::redis::Redis &redis = faabric::redis::Redis::getState();
        return redis.incrByLong(key, value);
    }

    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "__faasmp_getLong", I64, __faasmp_getLong, I32 keyPtr) {
        const std::shared_ptr<spdlog::logger> &logger = faabric::util::getLogger();
        logger->debug("S - __faasmp_getLong {}", keyPtr);

        Runtime::Memory *memoryPtr = getExecutingWAVMModule()->defaultMemory;
        std::string key{&Runtime::memoryRef<char>(memoryPtr, (Uptr) keyPtr)};
        faabric::redis::Redis &redis = faabric::redis::Redis::getState();
        return redis.getLong(key);
    }

    /**
     * This function is just around to debug issues with threaded access to stacks.
     */
    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "__faasmp_debug_copy", void, __faasmp_debug_copy, I32 src, I32 dest) {
        const std::shared_ptr<spdlog::logger> &logger = faabric::util::getLogger();
        logger->debug("S - __faasmp_debug_copy {} {}", src, dest);

        // Get pointers on host to both src and dest
        Runtime::Memory *memoryPtr = getExecutingWAVMModule()->defaultMemory;
        int *hostSrc = &Runtime::memoryRef<int>(memoryPtr, src);
        int *hostDest = &Runtime::memoryRef<int>(memoryPtr, dest);

        logger->debug("{}: copy {} -> {}", thisThreadNumber, *hostSrc, *hostDest);

        *hostDest = *hostSrc;
    }
    /**
     * @param    loc       Source code location
     * @param    gtid      Global thread id of this thread
     * @param    schedule  Scheduling type for the parallel loop
     * @param    lastIterPtr Pointer to the "last iteration" flag (boolean)
     * @param    lowerPtr    Pointer to the lower bound
     * @param    upperPtr    Pointer to the upper bound of loop chunk
     * @param    stridePtr   Pointer to the stride for parallel loop
     * @param    incr      Loop increment
     * @param    chunk     The chunk size for the parallel loop
     *
     * The functions compute the upper and lower bounds and strides to be used for the
     * set of iterations to be executed by the current thread.
     *
     * The guts of the implementation in openmp can be found in __kmp_for_static_init in
     * runtime/src/kmp_sched.cpp
     *
     * See sched_type for supported scheduling.
     */
    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "__kmpc_for_static_init_4", void, __kmpc_for_static_init_4,
                                   I32 loc, I32 gtid, I32 schedule, I32 lastIterPtr, I32 lowerPtr,
                                   I32 upperPtr, I32 stridePtr, I32 incr, I32 chunk) {
        const std::shared_ptr<spdlog::logger> &logger = faabric::util::getLogger();
        logger->debug("S - __kmpc_for_static_init_4 {} {} {} {} {} {} {} {} {}",
                      loc, gtid, schedule, lastIterPtr, lowerPtr, upperPtr, stridePtr, incr, chunk);

        // Get host pointers for the things we need to write
        Runtime::Memory *memoryPtr = getExecutingWAVMModule()->defaultMemory;
        I32 *lastIter = &Runtime::memoryRef<I32>(memoryPtr, lastIterPtr);
        I32 *lower = &Runtime::memoryRef<I32>(memoryPtr, lowerPtr);
        I32 *upper = &Runtime::memoryRef<I32>(memoryPtr, upperPtr);
        I32 *stride = &Runtime::memoryRef<I32>(memoryPtr, stridePtr);

        for_static_init<I32>(schedule, lastIter, lower, upper, stride, incr, chunk);
    }

    /*
     * See __kmpc_for_static_init_4
     */
    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "__kmpc_for_static_init_8", void, __kmpc_for_static_init_8,
                                   I32 loc, I32 gtid, I32 schedule, I32 lastIterPtr,
                                   I32 lowerPtr, I32 upperPtr, I32 stridePtr, // Pointers to I64
                                   I64 incr, I64 chunk) {

        const std::shared_ptr<spdlog::logger> &logger = faabric::util::getLogger();
        logger->debug("S - __kmpc_for_static_init_4 {} {} {} {} {} {} {} {} {}",
                      loc, gtid, schedule, lastIterPtr, lowerPtr, upperPtr, stridePtr, incr, chunk);

        // Get host pointers for the things we need to write
        Runtime::Memory *memoryPtr = getExecutingWAVMModule()->defaultMemory;
        I32 *lastIter = &Runtime::memoryRef<I32>(memoryPtr, lastIterPtr);
        I64 *lower = &Runtime::memoryRef<I64>(memoryPtr, lowerPtr);
        I64 *upper = &Runtime::memoryRef<I64>(memoryPtr, upperPtr);
        I64 *stride = &Runtime::memoryRef<I64>(memoryPtr, stridePtr);

        for_static_init<I64>(schedule, lastIter, lower, upper, stride, incr, chunk);
    }


    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "__kmpc_for_static_fini", void, __kmpc_for_static_fini,
                                   I32 loc, I32 gtid) {
        faabric::util::getLogger()->debug("S - __kmpc_for_static_fini {} {}", loc, gtid);
    }

    /**
     *  When reaching the end of the reduction loop, the threads need to synchronise to operate the
     *  reduction function. In the multi-machine case, this
     */
    int startReduction(int reduce_data) {
        int retVal = 0;
        const std::shared_ptr<spdlog::logger> &logger = faabric::util::getLogger();

        switch (thisLevel->reductionMethod()) {
            case ReduceTypes::criticalBlock:
                logger->debug("Thread {} reduction locking", thisThreadNumber);
                thisLevel->reduceMutex.lock();
                retVal = 1;
                break;
            case ReduceTypes::emptyBlock:
                retVal = 1;
                break;
            case ReduceTypes::atomicBlock:
                retVal = 2;
                break;
            case ReduceTypes::notDefined:
                throw std::runtime_error("Unsupported reduce operation");
                break;
            case ReduceTypes::multiHostSum:
                retVal = 1;
                break;
        }
        return retVal;
    }

    /**
     *  Called immediately after running the reduction section before exiting the `reduce` construct.
     */
    void endReduction() {
        if (0 <= thisLevel->userDefaultDevice) {
            // Unlocking not owned mutex is UB
            if (thisLevel->numThreads > 1) {
                faabric::util::getLogger()->debug("Thread {} unlocking reduction", thisThreadNumber);
                thisLevel->reduceMutex.unlock();
            }
        }
    }

    /**
     * A blocking reduce that includes an implicit barrier.
     * @param loc source location information
     * @param gtid global thread id
     * @param num_vars number of items (variables) to be reduced
     * @param reduce_size size of data in bytes to be reduced
     * @param reduce_data pointer to data to be reduced
     * @param reduce_func callback function providing reduction operation on two operands and returning result of reduction in lhs_data. Of type void(∗)(void ∗lhs data, void ∗rhs data)
     * @param lck pointer to the unique lock data structure
     * @return 1 for the master thread, 0 for all other team threads, 2 for all team threads if atomic reduction needed
     */
    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "__kmpc_reduce", I32, __kmpc_reduce, I32 loc, I32 gtid, I32 num_vars,
                                   I32 reduce_size, I32 reduce_data, I32 reduce_func, I32 lck) {
        faabric::util::getLogger()->debug("S - __kmpc_reduce {} {} {} {} {} {} {}", loc, gtid, num_vars, reduce_size,
                                 reduce_data, reduce_func, lck);

        return startReduction(reduce_data);
    }

    /**
     * The nowait version is used for a reduce clause with the nowait argument. Or direct exit of parallel section.
     * @param loc source location information
     * @param gtid global thread id
     * @param num_vars number of items (variables) to be reduced
     * @param reduce_size size of data in bytes to be reduced
     * @param reduce_data pointer to data to be reduced
     * @param reduce_func callback function providing reduction operation on two operands and returning result of reduction in lhs_data. Of type void(∗)(void ∗lhs data, void ∗rhs data)
     * @param lck pointer to the unique lock data structure
     * @return 1 for the master thread, 0 for all other team threads, 2 for all team threads if atomic reduction needed
     */
    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "__kmpc_reduce_nowait", I32, __kmpc_reduce_nowait, I32 loc, I32 gtid,
                                   I32 num_vars, I32 reduce_size, I32 reduce_data, I32 reduce_func, I32 lck) {
        faabric::util::getLogger()->debug("S - __kmpc_reduce_nowait {} {} {} {} {} {} {}", loc, gtid, num_vars, reduce_size,
                                 reduce_data, reduce_func, lck);

        return startReduction(reduce_data);
    }

    /**
     * Finish the execution of a blocking reduce. The lck pointer must be the same as that used in the corresponding start function.
     * @param loc location info
     * @param gtid global thread id
     * @param lck kmp_critical_name* to the critical section.
     */
    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "__kmpc_end_reduce", void, __kmpc_end_reduce, I32 loc, I32 gtid, I32 lck) {
        faabric::util::getLogger()->debug("S - __kmpc_end_reduce {} {} {}", loc, gtid, lck);
        endReduction();
    }

    /**
     * Arguments similar to __kmpc_end_reduce. Finish the execution of a reduce_nowait.
     * @param loc
     * @param gtid
     * @param lck
     */
    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "__kmpc_end_reduce_nowait", void, __kmpc_end_reduce_nowait, I32 loc, I32 gtid,
                                   I32 lck) {
        faabric::util::getLogger()->debug("S - __kmpc_end_reduce_nowait {} {} {}", loc, gtid, lck);
        endReduction();
    }

    /**
     * Get the number of devices (different CPU sockets or machines) available to that user
     */
    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "omp_get_num_devices", int, omp_get_num_devices) {
        faabric::util::getLogger()->debug("S - omp_get_num_devices");
        return thisLevel->userDefaultDevice;
    }

    /**
     * Switches between local and remote threads.
     */
    WAVM_DEFINE_INTRINSIC_FUNCTION(env, "omp_set_default_device", void, omp_set_default_device,
                                   int defaultDeviceNumber) {
        auto logger = faabric::util::getLogger();
        logger->debug("S - omp_set_default_device {}", defaultDeviceNumber);
        if (abs(defaultDeviceNumber) > 1) {
            faabric::util::getLogger()->warn(
                    "Given default device index ({}) is bigger than num of available devices (1), ignoring",
                    defaultDeviceNumber);
            return;
        }
        // TODO - flag negative with the specialisation of Level instead
        thisLevel->userDefaultDevice = defaultDeviceNumber;
    }

    template<typename T>
    void for_static_init(I32 schedule, I32 *lastIter, T *lower, T *upper, T *stride, T incr, T chunk) {
        // Unsigned version of the given template parameter
        typedef typename std::make_unsigned<T>::type UT;

        const std::shared_ptr<spdlog::logger> &logger = faabric::util::getLogger();

        if (thisLevel->numThreads == 1) {
            *lastIter = true;
            *stride = (incr > 0) ? (*upper - *lower + 1) : (-(*lower - *upper + 1));
            return;
        }

        UT tripCount;
        if (incr == 1) {
            tripCount = *upper - *lower + 1;
        } else if (incr == -1) {
            tripCount = *lower - *upper + 1;
        } else if (incr > 0) {
            // upper-lower can exceed the limit of signed type
            tripCount = (int) (*upper - *lower) / incr + 1;
        } else {
            tripCount = (int) (*lower - *upper) / (-incr) + 1;
        }

        switch (schedule) {
            case kmp::sch_static_chunked: {
                int span;
                if (chunk < 1) {
                    chunk = 1;
                }
                span = chunk * incr;
                *stride = span * thisLevel->numThreads;
                *lower = *lower + (span * thisThreadNumber);
                *upper = *lower + span - incr;
                *lastIter = (thisThreadNumber == ((tripCount - 1) / (unsigned int) chunk) % thisLevel->numThreads);
                break;
            }
            case kmp::sch_static: { // (chunk not given)
                // If we have fewer trip_counts than threads
                if (tripCount < thisLevel->numThreads) {
                    logger->warn("Small for loop trip count {} {}", tripCount,
                                 thisLevel->numThreads); // Warns for future use, not tested at scale
                    if (thisThreadNumber < tripCount) {
                        *upper = *lower = *lower + thisThreadNumber * incr;
                    } else {
                        *lower = *upper + incr;
                    }
                    *lastIter = (thisThreadNumber == tripCount - 1);
                } else {
                    // TODO: We only implement below kmp_sch_static_balanced, not kmp_sch_static_greedy
                    // Those are set through KMP_SCHEDULE so we would need to look out for real code setting this
                    logger->debug("Ignores KMP_SCHEDULE variable, defaults to static balanced schedule");
                    U32 small_chunk = tripCount / thisLevel->numThreads;
                    U32 extras = tripCount % thisLevel->numThreads;
                    *lower += incr * (thisThreadNumber * small_chunk +
                                      (thisThreadNumber < extras ? thisThreadNumber : extras));
                    *upper = *lower + small_chunk * incr - (thisThreadNumber < extras ? 0 : incr);
                    *lastIter = (thisThreadNumber == thisLevel->numThreads - 1);
                }

                *stride = tripCount;
                break;
            }
            default: {
                throw std::runtime_error(fmt::format("Unimplemented scheduler {}", schedule));
            }
        }
    }

    void ompLink() {

    }
}
