// CR-022 FIXED-direction adaptation of cr022_submit_after_failed_init.cpp.
//
// Original test reproduced: submissions accepted (try_submit==true, future
// stranded) when the pool was never initialized or after a failed
// initialize() rollback. After the fix (submit paths check initialized_ under
// the lock; timeout_ms read inside the lock), expected:
//
//   Part A (never initialized):   try_submit == false; submit() future ready
//                                 within 1s carrying "ThreadPool is stopped";
//                                 body never ran; total_threads == 0.
//   Part B (after failed init + rollback): initialize() == false and the same
//                                 rejection behavior as A.
//   Part C (control, initialize() OK): submission executes normally.
//
// Build (normal):
//   g++ -std=c++20 -O1 -g -DKAIRO_THREAD_POOL_TEST_HOOKS -I include -I src
//     review_verification/cr022_submit_after_failed_init_fixed.cpp
//     build/src/libexecutor.a -lpthread -ldl
//     -o review_verification/cr022_submit_after_failed_init_fixed
#include "kairo/thread_pool/thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <string>
#include <system_error>

#if !defined(KAIRO_THREAD_POOL_TEST_HOOKS)
#error "compile with -DKAIRO_THREAD_POOL_TEST_HOOKS (prebuilt lib has hooks)"
#endif

using namespace kairo;
using namespace std::chrono_literals;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

const char* status_name(std::future_status st) {
    switch (st) {
        case std::future_status::deferred: return "deferred";
        case std::future_status::ready: return "ready";
        case std::future_status::timeout: return "timeout";
    }
    return "unknown";
}

// Rejection contract shared by parts A and B.
void expect_rejected(ThreadPool& pool, const char* label) {
    std::atomic<bool> ran{false};
    const bool accepted =
        pool.try_submit([&ran] { ran.store(true); });
    check(!accepted, std::string(label) + ": try_submit returns false (rejected)");

    std::future<void> f = pool.submit([&ran] { ran.store(true); });
    const auto st = f.wait_for(1s);
    check(st == std::future_status::ready,
          std::string(label) + ": submit() future ready within 1s (got " +
              status_name(st) + ")");
    bool stopped_err = false;
    if (st == std::future_status::ready) {
        try {
            f.get();
        } catch (const std::runtime_error& e) {
            stopped_err = std::string(e.what()).find("stopped") !=
                          std::string::npos;
        } catch (...) {
        }
    }
    check(stopped_err, std::string(label) + ": future carries 'ThreadPool is "
                       "stopped' runtime_error");
    check(!ran.load(), std::string(label) + ": task body never ran");
    const bool drained = pool.try_wait_for_completion(1s);
    check(drained, std::string(label) + ": try_wait_for_completion(1s) true "
                   "(no stranded task)");
    const ThreadPoolStatus status = pool.get_status();
    check(status.total_threads == 0,
          std::string(label) + ": total_threads == 0 (got " +
              std::to_string(status.total_threads) + ")");
}

}  // namespace

int main() {
    setbuf(stdout, nullptr);

    // ---- Part A: pool constructed but initialize() never called ----------
    {
        auto* pool = new ThreadPool();  // leaked on purpose (mirrors original)
        expect_rejected(*pool, "A: never initialized");
    }

    // ---- Part B: initialize() fails, rollback runs, then submit ----------
    {
        auto* pool = new ThreadPool();  // leaked on purpose (mirrors original)
        ThreadPoolConfig config;
        config.min_threads = 4;
        config.max_threads = 4;
        config.queue_capacity = 100;

        std::atomic<int> create_attempts{0};
        pool->set_worker_thread_start_hook_for_test([&](size_t) {
            const int attempt = create_attempts.fetch_add(1) + 1;
            if (attempt == 3) {
                throw std::system_error(
                    std::make_error_code(std::errc::resource_unavailable_try_again),
                    "CR-022 injected worker thread creation failure");
            }
        });
        const bool initialized = pool->initialize(config);
        check(!initialized, "B: initialize() returned false (injected failure)");
        pool->set_worker_thread_start_hook_for_test(nullptr);
        expect_rejected(*pool, "B: after rollback");
    }

    // ---- Part C (control): successful initialize, normal execution --------
    {
        ThreadPool pool;
        ThreadPoolConfig config;
        config.min_threads = 2;
        config.max_threads = 2;
        config.queue_capacity = 100;
        check(pool.initialize(config), "C: initialize() succeeded");
        std::atomic<bool> ran{false};
        std::future<int> f = pool.submit([&ran] {
            ran.store(true);
            return 42;
        });
        const auto st = f.wait_for(5s);
        bool value_ok = false;
        if (st == std::future_status::ready) {
            value_ok = (f.get() == 42);
        }
        check(value_ok && ran.load(),
              "C: post-initialize submission executes and returns 42 "
              "(no behavior regression)");
        pool.shutdown();
    }

    std::printf("CR-022-FIXED VERDICT: %s (%d checks failed)\n",
                g_failures == 0 ? "PASS" : "FAIL", g_failures);
    // Parts A/B pools are intentionally leaked; skip their destructors.
    std::_Exit(g_failures == 0 ? 0 : 1);
}
