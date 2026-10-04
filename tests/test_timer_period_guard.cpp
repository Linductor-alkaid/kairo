#include <gtest/gtest.h>

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

MMRESULT WINAPI fake_time_begin_period_failure(UINT /*period_ms*/) {
    ++g_begin_calls;
    return TIMERR_NOCANDO;
}

void reset_counters() {
    g_timer_reference_count = 0;
    g_begin_calls = 0;
    g_end_calls = 0;
}

}  // namespace
#endif

TEST(TimerPeriodGuard, BalancesSuccessfulBeginAndEnd) {
#ifdef _WIN32
    reset_counters();
    kairo::util::set_timer_period_functions_for_test(&fake_time_begin_period,
                                                        &fake_time_end_period);

    {
        kairo::util::TimerPeriodGuard guard(1);
        EXPECT_EQ(g_begin_calls, 1);
        EXPECT_EQ(g_end_calls, 0);
        EXPECT_EQ(g_timer_reference_count, 1);
    }

    EXPECT_EQ(g_begin_calls, 1);
    EXPECT_EQ(g_end_calls, 1);
    EXPECT_EQ(g_timer_reference_count, 0);
    kairo::util::reset_timer_period_functions_for_test();
#else
    kairo::util::TimerPeriodGuard guard(1);
    SUCCEED();
#endif
}

TEST(TimerPeriodGuard, DoesNotEndWhenBeginFails) {
#ifdef _WIN32
    reset_counters();
    kairo::util::set_timer_period_functions_for_test(&fake_time_begin_period_failure,
                                                        &fake_time_end_period);

    {
        kairo::util::TimerPeriodGuard guard(1);
        EXPECT_EQ(g_begin_calls, 1);
        EXPECT_EQ(g_end_calls, 0);
        EXPECT_EQ(g_timer_reference_count, 0);
    }

    EXPECT_EQ(g_begin_calls, 1);
    EXPECT_EQ(g_end_calls, 0);
    EXPECT_EQ(g_timer_reference_count, 0);
    kairo::util::reset_timer_period_functions_for_test();
#else
    kairo::util::TimerPeriodGuard guard(1);
    SUCCEED();
#endif
}
