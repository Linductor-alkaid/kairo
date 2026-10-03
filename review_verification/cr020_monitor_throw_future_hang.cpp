// CR-020: monitor record_task_start throwing -> submit() future never ready.
//
// Claim: thread_pool.cpp execute_task() calls monitor->record_task_start()
// BEFORE task.function(). If the callback throws, the outer catch-all only
// fires update_statistics(); the submit()-level promise is never satisfied,
// so the future returned to the caller hangs forever.
//
// Expected evidence if CONFIRMED:
//  - throwing monitor: future.wait_for(2s) == timeout, task body never ran
//  - pool internally considers the task "completed" (try_wait_for_completion
//    returns true) while the future is stranded
//  - worker survived: after switching to a well-behaved monitor, a new
//    submitted task completes normally (control)
#include "kairo/thread_pool/thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <string>

using namespace kairo;
using namespace std::chrono_literals;

namespace {

class ThrowingStartMonitor : public monitor::TaskMonitor {
public:
    void record_task_start(const std::string& /*task_id*/,
                           const std::string& /*task_type*/) override {
        throw std::runtime_error("CR-020 injected failure in record_task_start");
    }
};

const char* status_name(std::future_status st) {
    switch (st) {
        case std::future_status::deferred: return "deferred";
        case std::future_status::ready: return "ready";
        case std::future_status::timeout: return "timeout";
    }
    return "unknown";
}

}  // namespace

int main() {
    setbuf(stdout, nullptr);

    ThreadPool pool;
    ThreadPoolConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    config.queue_capacity = 1000;
    if (!pool.initialize(config)) {
        printf("FATAL: pool initialize failed\n");
        return 2;
    }

    // ---- Case 1: throwing record_task_start ----
    ThrowingStartMonitor throwing_monitor;
    pool.set_task_monitor(&throwing_monitor);

    std::atomic<bool> task_ran{false};
    std::future<void> f = pool.submit([&task_ran]() { task_ran.store(true); });

    auto st = f.wait_for(2s);
    printf("[throwing monitor] future.wait_for(2s) = %s\n", status_name(st));
    printf("[throwing monitor] task body executed = %s\n",
           task_ran.load() ? "yes" : "no");

    bool internally_done =
        pool.try_wait_for_completion(std::chrono::milliseconds(2000));
    printf("[throwing monitor] pool try_wait_for_completion(2s) = %s "
           "(pool-side completion vs stranded future)\n",
           internally_done ? "true" : "false");

    ThreadPoolStatus s1 = pool.get_status();
    printf("[throwing monitor] status: total_tasks=%zu completed=%zu failed=%zu "
           "active=%zu queue=%zu\n",
           s1.total_tasks, s1.completed_tasks, s1.failed_tasks,
           s1.active_threads, s1.queue_size);

    bool case1_confirmed =
        (st == std::future_status::timeout) && !task_ran.load();

    // ---- Case 2 (control): well-behaved monitor, worker must have survived ----
    monitor::TaskMonitor normal_monitor;
    pool.set_task_monitor(&normal_monitor);

    std::atomic<bool> task2_ran{false};
    std::future<int> g = pool.submit([&task2_ran]() {
        task2_ran.store(true);
        return 7;
    });
    auto st2 = g.wait_for(2s);
    printf("[control monitor] future.wait_for(2s) = %s\n", status_name(st2));
    if (st2 == std::future_status::ready) {
        printf("[control monitor] future value = %d, task body executed = %s\n",
               g.get(), task2_ran.load() ? "yes" : "no");
    } else {
        printf("[control monitor] task body executed = %s\n",
               task2_ran.load() ? "yes" : "no");
    }
    bool control_ok = (st2 == std::future_status::ready) && task2_ran.load();

    printf("CR-020 verdict: %s\n",
           (case1_confirmed && control_ok)
               ? "CONFIRMED (future never ready when record_task_start throws; "
                 "pool healthy with normal monitor)"
               : (case1_confirmed ? "CONFIRMED (control failed separately)"
                                  : "NOT REPRODUCED"));

    pool.shutdown();
    return (case1_confirmed && control_ok) ? 0 : 1;
}
