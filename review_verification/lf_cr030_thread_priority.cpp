// CR-030: Linux set_thread_priority applies the nice part of the SCHED_OTHER
// path to the CALLING thread (setpriority(PRIO_PROCESS, 0, priority), who=0 ->
// caller TID), ignoring the target `handle` (thread_utils.cpp:234-242).
//
// Priority->policy mapping in the implementation:
//   priority in [1,99]   -> SCHED_FIFO(sched_priority=priority)  [needs root]
//   otherwise            -> SCHED_OTHER(sched_priority=0); if priority != 0,
//                           setpriority(PRIO_PROCESS, 0, clamp(priority,-20,19))
// so the SCHED_OTHER/nice sub-path is exercised with priority=100 (clamped to
// nice 19, lowering priority, allowed unprivileged).
//
// CONFIRMED if: set_thread_priority(worker_handle, 100) returns true, main's
// nice becomes 19, worker's nice stays 0. A contrast probe then shows that
// setpriority(PRIO_PROCESS, worker_tid, ...) does reach the worker, proving
// per-thread targeting is possible and the handle was simply ignored.

#include "kairo/util/thread_utils.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

using kairo::util::get_current_thread_priority;
using kairo::util::set_thread_priority;

namespace {

pid_t gettid_sys() { return static_cast<pid_t>(syscall(SYS_gettid)); }

int self_nice() {
    errno = 0;
    const int n = getpriority(PRIO_PROCESS, 0);
    return errno == 0 ? n : -1000;
}

}  // namespace

int main() {
    std::atomic<pid_t> worker_tid{0};
    std::atomic<int> worker_nice_sample{-1000};
    std::atomic<bool> shutdown{false};

    std::thread worker([&] {
        worker_tid = gettid_sys();
        while (!shutdown.load(std::memory_order_relaxed)) {
            worker_nice_sample = self_nice();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    while (worker_tid.load() == 0) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const pid_t main_tid = gettid_sys();
    const int main_nice_before = self_nice();
    const int worker_nice_before = worker_nice_sample.load();
    const pthread_t handle = worker.native_handle();

    std::printf("uid=%d main tid=%d nice_before=%d prio_api=%d\n", getuid(),
                main_tid, main_nice_before, get_current_thread_priority());
    std::printf("worker tid=%d nice_before=%d\n", worker_tid.load(),
                worker_nice_before);

    // ---- Sub-path 1 (CR-030's exact branch): priority=100 -> SCHED_OTHER +
    // setpriority(PRIO_PROCESS, 0, clamp(100->19)) ----
    const bool r_other = set_thread_priority(handle, 100);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const int main_nice_after = self_nice();
    const int worker_nice_after = worker_nice_sample.load();
    std::printf("[SCHED_OTHER path] set_thread_priority(worker_handle, 100) -> %s\n",
                r_other ? "true" : "false");
    std::printf("[SCHED_OTHER path] main nice after=%d  worker nice after=%d\n",
                main_nice_after, worker_nice_after);

    const bool main_changed = main_nice_after != main_nice_before;
    const bool worker_unchanged = worker_nice_after == worker_nice_before;
    std::printf("[SCHED_OTHER path] main_changed=%d worker_unchanged=%d\n",
                main_changed, worker_unchanged);

    // ---- Contrast: targeting the worker TID directly does reach the worker.
    const int sp = setpriority(PRIO_PROCESS, worker_tid.load(), 19);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::printf("[contrast] setpriority(PRIO_PROCESS, worker_tid, 19) -> %d "
                "(errno=%s), worker nice now=%d, main nice=%d\n",
                sp, strerror(errno), worker_nice_sample.load(), self_nice());

    // ---- Sub-path 2: priority=50 -> SCHED_FIFO(50) via pthread_setschedparam.
    const bool r_fifo = set_thread_priority(handle, 50);
    std::printf("[SCHED_FIFO path] set_thread_priority(worker_handle, 50) -> %s "
                "(expected false/EPERM for non-root)\n",
                r_fifo ? "true" : "false");

    // ---- Sub-path 3: priority=0 -> SCHED_OTHER, setpriority skipped.
    const bool r_zero = set_thread_priority(handle, 0);
    std::printf("[priority=0 path] -> %s (no nice write by design; main=%d)\n",
                r_zero ? "true" : "false", self_nice());

    shutdown = true;
    worker.join();

    const char* verdict;
    if (r_other && main_changed && worker_unchanged) {
        verdict = "CR-030 VERDICT: CONFIRMED - nice applied to CALLING thread; "
                  "target handle ignored (contrast probe reaches worker via TID)";
    } else if (r_other && main_changed && !worker_unchanged) {
        verdict = "CR-030 VERDICT: PARTIAL - handle ignored, but nice reached "
                  "the whole process (worker changed too)";
    } else {
        verdict = "CR-030 VERDICT: NOT CONFIRMED - caller nice unchanged";
    }
    std::printf("%s\n", verdict);
    return (r_other && main_changed && worker_unchanged) ? 0 : 1;
}
