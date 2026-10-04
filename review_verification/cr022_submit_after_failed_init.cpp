// CR-022: submissions accepted (and never executed) after initialization
// failure rollback / before any initialize().
//
// Claim: rollback_initialization_failure() (thread_pool.cpp:156-157) resets
// stop_=false and initialized_=false. try_submit() only checks stop_, never
// initialized_, so after a failed initialize() (or with no initialize() at
// all) tasks are accepted into the scheduler while zero workers exist and are
// therefore never executed; try_wait_for_completion times out.
//
// Expected evidence if CONFIRMED:
//  - try_submit returns true / submit() returns a valid future
//  - future.wait_for(1s) == timeout, task body never ran
//  - try_wait_for_completion(1s) == false, total_threads == 0
//
// NOTE: the ThreadPool destructor calls shutdown(true) whose
// wait_for_completion blocks up to kDefaultWaitForCompletionTimeout (300s)
// because the stranded task keeps total != completed. To keep this test
// bounded we intentionally leak the pools and terminate via std::_Exit().
#include "kairo/thread_pool/thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <string>
#include <system_error>
#include <utility>

#if !defined(KAIRO_THREAD_POOL_TEST_HOOKS)
#error "compile with -DKAIRO_THREAD_POOL_TEST_HOOKS (prebuilt lib has hooks)"
#endif

using namespace kairo;
using namespace std::chrono_literals;

namespace {

const char* status_name(std::future_status st) {
    switch (st) {
        case std::future_status::deferred: return "deferred";
        case std::future_status::ready: return "ready";
        case std::future_status::timeout: return "timeout";
    }
    return "unknown";
}

void report_case(const char* label,
                 bool submit_accepted,
                 std::future_status st,
                 bool task_ran,
                 bool completion_wait_ok,
                 const ThreadPoolStatus& status,
                 bool stopped) {
    printf("[%s] try_submit accepted = %s\n",
           label, submit_accepted ? "true" : "false");
    printf("[%s] future.wait_for(1s) = %s\n", label, status_name(st));
    printf("[%s] task body executed = %s\n", label, task_ran ? "yes" : "no");
    printf("[%s] try_wait_for_completion(1s) = %s\n",
           label, completion_wait_ok ? "true (completed)" : "false (TIMEOUT)");
    printf("[%s] status: total_threads=%zu queue_size=%zu total_tasks=%zu "
           "completed=%zu, is_stopped=%s\n",
           label, status.total_threads, status.queue_size, status.total_tasks,
           status.completed_tasks, stopped ? "true" : "false");
}

bool case_confirmed(bool submit_accepted,
                    std::future_status st,
                    bool task_ran,
                    bool completion_wait_ok) {
    return submit_accepted && st == std::future_status::timeout && !task_ran &&
           !completion_wait_ok;
}

}  // namespace

int main() {
    setbuf(stdout, nullptr);
    bool any_confirmed = false;

    // ---- Part A: pool constructed but initialize() never called ----
    {
        // Intentionally leaked: destructor would block in shutdown()'s 300s
        // wait_for_completion because the stranded task can never complete.
        auto* pool = new ThreadPool();

        std::atomic<bool> ran{false};
        bool accepted = pool->try_submit([&ran]() { ran.store(true); });
        std::future<void> f = pool->submit([&ran]() { ran.store(true); });
        auto st = f.wait_for(1s);
        bool completion_wait_ok =
            pool->try_wait_for_completion(std::chrono::milliseconds(1000));
        ThreadPoolStatus status = pool->get_status();

        report_case("A: never initialized", accepted, st, ran.load(),
                    completion_wait_ok, status, pool->is_stopped());
        any_confirmed |= case_confirmed(accepted, st, ran.load(),
                                        completion_wait_ok);
    }

    // ---- Part B: initialize() fails, rollback runs, then submit ----
    {
        auto* pool = new ThreadPool();

        ThreadPoolConfig config;
        config.min_threads = 4;
        config.max_threads = 4;
        config.queue_capacity = 100;

        std::atomic<int> create_attempts{0};
        pool->set_worker_thread_start_hook_for_test([&](size_t) {
            int attempt = create_attempts.fetch_add(1) + 1;
            if (attempt == 3) {
                throw std::system_error(
                    std::make_error_code(std::errc::resource_unavailable_try_again),
                    "CR-022 injected worker thread creation failure");
            }
        });

        bool initialized = pool->initialize(config);
        printf("[B: failed init rollback] initialize() returned %s "
               "(attempt #3 injected to throw)\n",
               initialized ? "true" : "false");
        printf("[B: failed init rollback] is_stopped after rollback = %s "
               "(rollback resets stop_=false at thread_pool.cpp:157)\n",
               pool->is_stopped() ? "true" : "false");

        pool->set_worker_thread_start_hook_for_test(nullptr);

        std::atomic<bool> ran{false};
        bool accepted = pool->try_submit([&ran]() { ran.store(true); });
        std::future<void> f = pool->submit([&ran]() { ran.store(true); });
        auto st = f.wait_for(1s);
        bool completion_wait_ok =
            pool->try_wait_for_completion(std::chrono::milliseconds(1000));
        ThreadPoolStatus status = pool->get_status();

        report_case("B: after rollback", accepted, st, ran.load(),
                    completion_wait_ok, status, pool->is_stopped());
        bool b_confirmed = (!initialized) &&
                           case_confirmed(accepted, st, ran.load(),
                                          completion_wait_ok);
        any_confirmed |= b_confirmed;
    }

    printf("CR-022 verdict: %s\n",
           any_confirmed
               ? "CONFIRMED (submission accepted without initialized pool, "
                 "task never executes, completion wait times out)"
               : "NOT REPRODUCED (submission path rejected or executed tasks)");

    // Skip ThreadPool destructors on purpose (see file header comment).
    std::_Exit(any_confirmed ? 0 : 1);
}
