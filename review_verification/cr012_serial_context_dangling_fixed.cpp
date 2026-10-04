// CR-012 FIXED-direction adaptation of cr012_serial_context_dangling.cpp.
//
// Original test reproduced the dangling SerialExecutionContext& capture
// (worker-side publish_task touching a destroyed context => permanent futex
// hang, ASAN run exited 124). After the pimpl/shared-state fix, expected:
//
//   variant A (context dies before publish):
//     - no hang, no ASAN report;
//     - publish_task observes post_reserved()==false (Shared detached)
//       and settles the business future with ExecutorStopping;
//     - the submitted callback never runs;
//   control (context alive):
//     - submit_on callbacks run FIFO in submission order, futures complete.
//
// Build (ASAN):
//   g++ -std=c++20 -O1 -g -fsanitize=address -fno-omit-frame-pointer
//     -DKAIRO_THREAD_POOL_TEST_HOOKS -I include -I src
//     review_verification/cr012_serial_context_dangling_fixed.cpp
//     build-asan/src/libexecutor.a -lpthread -dl
//     -o review_verification/cr012_serial_context_dangling_fixed
#include <kairo/executor.hpp>
#include <kairo/serial_execution_context.hpp>
#include <kairo/types.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

using namespace kairo;
using namespace std::chrono_literals;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

}  // namespace

int main() {
    Executor ex;
    if (!ex.initialize(ExecutorConfig{})) {
        std::printf("initialize failed\n");
        return 2;
    }

    // ---------------- variant A: context destroyed before publish ----------
    {
        std::vector<std::future<void>> blockers;
        for (int i = 0; i < 20; ++i) {
            blockers.push_back(ex.submit([] {
                std::this_thread::sleep_for(250ms);
            }));
        }
        auto ctx = std::make_unique<SerialExecutionContext>();
        std::atomic<bool> serial_ran{false};
        auto sub = ex.submit_on(*ctx, [&serial_ran] { serial_ran.store(true); });
        std::printf("[A] submit_on done, destroying context now\n");
        std::fflush(stdout);
        ctx.reset();  // Shared detached; facade closures keep shared_ptr

        for (auto& f : blockers) {
            if (f.wait_for(10s) != std::future_status::ready) {
                check(false, "A: blocker timed out (pool stuck)");
                return 2;
            }
            f.get();
        }
        std::printf("[A] blockers drained; publish_task now runs against "
                    "detached (not freed) Shared state\n");
        std::fflush(stdout);

        const auto st = sub.wait_for(5s);
        const bool ready = (st == std::future_status::ready);
        check(ready, "A: business future ready within 5s (was: permanent "
                     "hang, exit 124 pre-fix)");
        check(!serial_ran.load(), "A: submitted callback did NOT run");

        bool stopping_exc = false;
        std::string what;
        if (ready) {
            try {
                sub.get();
                what = "<no exception>";
            } catch (const ExecutorStopping& e) {
                stopping_exc = true;
                what = e.what();
            } catch (const std::exception& e) {
                what = std::string(typeid(e).name()) + ": " + e.what();
            }
        }
        check(stopping_exc,
              "A: future carries ExecutorStopping (got: " + what + ")");
    }

    // ---------------- control: alive context, FIFO order -------------------
    {
        SerialExecutionContext ctx;  // stays alive for the whole block
        std::mutex m;
        std::vector<int> order;
        std::vector<std::future<int>> futures;
        for (int i = 0; i < 3; ++i) {
            futures.push_back(ex.submit_on(ctx, [i, &m, &order] {
                std::this_thread::sleep_for(20ms);
                std::lock_guard<std::mutex> lk(m);
                order.push_back(i);
                return i * 10;
            }));
        }
        bool all_ok = true;
        for (int i = 0; i < 3; ++i) {
            if (futures[i].wait_for(5s) != std::future_status::ready ||
                futures[i].get() != i * 10) {
                all_ok = false;
            }
        }
        check(all_ok, "control: 3 submit_on futures completed with correct values");
        bool fifo = (order.size() == 3) && (order[0] == 0 && order[1] == 1 &&
                                            order[2] == 2);
        check(fifo, "control: callbacks executed in FIFO submission order");
    }

    (void)ex.shutdown(true);
    std::printf(g_failures == 0
                    ? "CR-012-FIXED VERDICT: PASS (no hang, ExecutorStopping "
                      "settlement, control path healthy)\n"
                    : "CR-012-FIXED VERDICT: FAIL (%d checks failed)\n",
                g_failures);
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
