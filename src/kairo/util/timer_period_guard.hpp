#pragma once

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#else
using UINT = unsigned int;
#endif

namespace kairo {
namespace util {

class TimerPeriodGuard {
public:
    explicit TimerPeriodGuard(UINT period_ms);
    ~TimerPeriodGuard();

    TimerPeriodGuard(const TimerPeriodGuard&) = delete;
    TimerPeriodGuard& operator=(const TimerPeriodGuard&) = delete;

private:
    UINT period_ms_ = 0;
    bool active_ = false;
};

#ifdef _WIN32
using TimerPeriodBeginFn = MMRESULT(WINAPI*)(UINT);
using TimerPeriodEndFn = MMRESULT(WINAPI*)(UINT);

void set_timer_period_functions_for_test(TimerPeriodBeginFn begin_fn,
                                         TimerPeriodEndFn end_fn);
void reset_timer_period_functions_for_test();
#endif

}  // namespace util
}  // namespace kairo
