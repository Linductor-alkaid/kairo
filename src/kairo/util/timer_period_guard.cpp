#include "timer_period_guard.hpp"

namespace kairo {
namespace util {

#ifdef _WIN32
namespace {

TimerPeriodBeginFn g_time_begin_period = &timeBeginPeriod;
TimerPeriodEndFn g_time_end_period = &timeEndPeriod;

}  // namespace

TimerPeriodGuard::TimerPeriodGuard(UINT period_ms)
    : period_ms_(period_ms)
{
    active_ = (g_time_begin_period(period_ms_) == TIMERR_NOERROR);
}

TimerPeriodGuard::~TimerPeriodGuard() {
    if (active_) {
        g_time_end_period(period_ms_);
    }
}

void set_timer_period_functions_for_test(TimerPeriodBeginFn begin_fn,
                                         TimerPeriodEndFn end_fn) {
    g_time_begin_period = begin_fn ? begin_fn : &timeBeginPeriod;
    g_time_end_period = end_fn ? end_fn : &timeEndPeriod;
}

void reset_timer_period_functions_for_test() {
    g_time_begin_period = &timeBeginPeriod;
    g_time_end_period = &timeEndPeriod;
}

#else

TimerPeriodGuard::TimerPeriodGuard(UINT /*period_ms*/) {}

TimerPeriodGuard::~TimerPeriodGuard() = default;

#endif

}  // namespace util
}  // namespace kairo
