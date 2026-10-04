#include <gtest/gtest.h>

#include <stdexcept>

#include "kairo/util/timer_period_guard.hpp"

#ifdef _WIN32
namespace {

int g_timer_reference_count = 0;
int g_begin_calls = 0;
int g_end_calls = 0;

MMRESULT WINAPI fake_time_begin_period(UINT /*period_ms*/) {
    ++g_begin_calls;
    ++g_timer_reference_count;
    return TIMERR_NOERROR;
}

MMRESULT WINAPI fake_time_end_period(UINT /*period_ms*/) {
    ++g_end_calls;
    --g_timer_reference_count;
    return TIMERR_NOERROR;
}

}  // namespace
#endif

TEST(RealtimeInitExceptionSafety, TimerPeriodEndsDuringInitStackUnwind) {
#ifdef _WIN32
    g_timer_reference_count = 0;
    g_begin_calls = 0;
    g_end_calls = 0;
    kairo::util::set_timer_period_functions_for_test(&fake_time_begin_period,
                                                        &fake_time_end_period);

    EXPECT_THROW(
        [] {
            kairo::util::TimerPeriodGuard guard(1);
            throw std::runtime_error("simulated realtime init failure");
        }(),
        std::runtime_error);

    EXPECT_EQ(g_begin_calls, 1);
    EXPECT_EQ(g_end_calls, 1);
    EXPECT_EQ(g_timer_reference_count, 0);
    kairo::util::reset_timer_period_functions_for_test();
#else
    EXPECT_THROW(
        [] {
            kairo::util::TimerPeriodGuard guard(1);
            throw std::runtime_error("simulated realtime init failure");
        }(),
        std::runtime_error);
#endif
}
