// Test for data-race-free access to timer_period_ms_ in RealtimeThreadExecutor.
// Run under TSAN (-fsanitize=thread) to verify no data race is reported.
#include <gtest/gtest.h>
#include <chrono>
#include <thread>

#include "kairo/realtime_thread_executor.hpp"

using namespace kairo;
using namespace std::chrono_literals;

namespace {

RealtimeThreadConfig make_config() {
    RealtimeThreadConfig cfg;
    cfg.thread_name   = "tsan_test_rt";
    cfg.cycle_period_ns = 5'000'000;  // 5ms
    cfg.thread_priority = 0;
    cfg.cycle_callback  = []() {};
    return cfg;
}

}  // namespace

// Repeatedly start/stop the executor from the main thread while the worker
// thread may be writing timer_period_ms_ (Windows path).  Under TSAN this
// would immediately report a data race if the field is a plain unsigned int.
TEST(RealtimeTimerPeriodRace, NoDataRace) {
    constexpr int kIterations = 1000;

    for (int i = 0; i < kIterations; ++i) {
        RealtimeThreadExecutor exec("race_test", make_config());
        exec.start();
        // Give the worker thread a brief window to run its initialisation
        // code (including the Windows timer_period_ms_ write on short cycles).
        std::this_thread::sleep_for(1ms);
        exec.stop();
    }
    // If TSAN detects a race it terminates the process with a non-zero exit
    // code before we reach this point.
    SUCCEED();
}
