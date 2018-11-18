#include <catch/catch.hpp>

#include <worker/worker.h>

using namespace worker;

namespace tests {
    static infra::Redis redis;

    void setUp() {
        redis.flushAll();

        // Network ns requires root
        util::setEnvVar("NETNS_MODE", "off");
    }

    void tearDown() {
        util::unsetEnvVar("NETNS_MODE");
    }

    void execFunction(message::FunctionCall &call) {
        redis.callFunction(call);

        Worker w(1);
        w.run();
    }

    TEST_CASE("Test full execution of WASM module", "[worker]") {
        setUp();

        message::FunctionCall call;
        call.set_user("demo");
        call.set_function("echo");
        call.set_inputdata("this is input");
        call.set_resultkey("test_echo");

        // Run the execution
        execFunction(call);
        message::FunctionCall result = redis.getFunctionResult(call);

        // Check output
        REQUIRE(result.outputdata() == "this is input");
        REQUIRE(result.success());

        tearDown();
    }

    TEST_CASE("Test executing non-existent function", "[worker]") {
        setUp();

        message::FunctionCall call;
        call.set_user("foobar");
        call.set_function("baz");
        call.set_resultkey("test_invalid");

        execFunction(call);
        message::FunctionCall result = redis.getFunctionResult(call);

        REQUIRE(!result.success());
        REQUIRE(result.outputdata() == "foobar - baz is not a valid function");

        tearDown();
    }

    TEST_CASE("Test function chaining", "[worker]") {
        setUp();

        message::FunctionCall call;
        call.set_user("demo");
        call.set_function("chain");
        call.set_resultkey("test_chain");

        // Make sure enough available workers on unassigned
        redis.addToUnassignedSet("worker 1");
        redis.addToUnassignedSet("worker 2");
        redis.addToUnassignedSet("worker 3");
        redis.addToUnassignedSet("worker 4");

        // Run the execution
        execFunction(call);

        // Check the call executed successfully
        message::FunctionCall result = redis.getFunctionResult(call);
        REQUIRE(result.success());

        // Check the chained calls have been set up
        message::FunctionCall chainA = redis.nextFunctionCall("worker 2");
        message::FunctionCall chainB = redis.nextFunctionCall("worker 3");
        message::FunctionCall chainC = redis.nextFunctionCall("worker 4");

        // Check all are set with the right user
        REQUIRE(chainA.user() == "demo");
        REQUIRE(chainB.user() == "demo");
        REQUIRE(chainC.user() == "demo");

        // Check function names
        std::vector<message::FunctionCall> calls(3);
        calls.push_back(chainA);
        calls.push_back(chainB);
        calls.push_back(chainC);

        bool aFound = false;
        bool bFound = false;
        bool cFound = false;

        for(const auto c : calls) {
            if(c.function() == "echo") {
                std::vector<uint8_t> expected = {0, 1, 2};
                REQUIRE(util::stringToBytes(c.inputdata()) == expected);
                aFound = true;
            }
            if(c.function() == "x2") {
                std::vector<uint8_t> expected = {1, 2, 3};
                REQUIRE(util::stringToBytes(c.inputdata()) == expected);
                bFound = true;
            }
            if(c.function() == "dummy") {
                std::vector<uint8_t> expected = {2, 3, 4};
                REQUIRE(util::stringToBytes(c.inputdata()) == expected);
                cFound = true;
            }
        }

        bool allFound = aFound && bFound && cFound;
        REQUIRE(allFound);

        tearDown();
    }

    TEST_CASE("Test state", "[worker]") {
        setUp();

        // Initially function's state should be an empty array
        // Note, we need to prepend the user to the actual key used in the code
        const char* stateKey = "demo_state_example";
        std::vector<uint8_t> initialState = redis.get(stateKey);
        REQUIRE(initialState.empty());

        // Set up the function call
        message::FunctionCall call;
        call.set_user("demo");
        call.set_function("state");
        call.set_resultkey("test_state");

        // Execute and check
        execFunction(call);
        message::FunctionCall resultA = redis.getFunctionResult(call);
        REQUIRE(resultA.success());

        // Load the state again, it should have a new element
        std::vector<uint8_t> stateA = redis.get(stateKey);
        std::vector<uint8_t> expectedA = {0};
        REQUIRE(stateA == expectedA);

        // Call the function a second time, the state should have another element added
        execFunction(call);
        message::FunctionCall resultB = redis.getFunctionResult(call);
        REQUIRE(resultB.success());

        std::vector<uint8_t> stateB = redis.get(stateKey);
        std::vector<uint8_t> expectedB = {0, 1};
        REQUIRE(stateB == expectedB);
    }

    TEST_CASE("Test state increment", "[worker]") {
        setUp();

        // Set up the function call
        message::FunctionCall call;
        call.set_user("demo");
        call.set_function("increment");
        call.set_resultkey("test_state_incr");

        // Execute and check
        execFunction(call);
        message::FunctionCall resultA = redis.getFunctionResult(call);
        REQUIRE(resultA.success());
        REQUIRE(resultA.outputdata() == "Counter: 001");

        // Call the function a second time, the state should have been incremented
        execFunction(call);
        message::FunctionCall resultB = redis.getFunctionResult(call);
        REQUIRE(resultB.success());
        REQUIRE(resultB.outputdata() == "Counter: 002");
    }
}