// CR-004: LockFreeTaskExecutor::set_before_publish_hook stores
// user_before_publish_hook_ / user_before_publish_context_ with plain
// non-atomic writes (lockfree_task_executor.cpp:326-327) while the installed
// trampoline reads the same fields on PRODUCER threads
// (lockfree_task_executor.cpp:336-337, invoked from the queue's
// begin_write/begin_batch_write hook dispatch).
//
// TSAN reproduction: one worker; 3 producer threads hammer push_task; the
// main thread alternates set_before_publish_hook(dummy, nullptr) and
// set_before_publish_hook(nullptr, nullptr) `iterations` times.
//
// Build (TSAN):
//   g++ -std=c++20 -O1 -g -fsanitize=thread -fno-omit-frame-pointer
//     -DKAIRO_THREAD_POOL_TEST_HOOKS -I include -I src
//     review_verification/lf_cr004_hook_race.cpp build-tsan/src/libexecutor.a
//     -lpthread -ldl -o review_verification/lf_cr004_hook_race_tsan
// Run:
//   setarch $(uname -m) -R ./review_verification/lf_cr004_hook_race_tsan
//     [iterations]

#include "kairo/lockfree_task_executor.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using kairo::LockFreeTaskExecutor;

static void dummy_hook(void*) {}

int main(int argc, char** argv) {
    const int iterations = argc > 1 ? std::atoi(argv[1]) : 100000;

    LockFreeTaskExecutor ex(1024);
    if (!ex.start()) {
        std::printf("FATAL: executor start failed\n");
        return 2;
    }

    std::atomic<bool> stop{false};
    std::atomic<long> total_pushes{0};
    std::vector<std::thread> producers;
    for (int p = 0; p < 3; ++p) {
        producers.emplace_back([&] {
            long n = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                (void)ex.push_task([] {});
                ++n;
            }
            total_pushes.fetch_add(n, std::memory_order_relaxed);
        });
    }

    for (int i = 0; i < iterations; ++i) {
        ex.set_before_publish_hook(&dummy_hook, nullptr);
        ex.set_before_publish_hook(nullptr, nullptr);
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto& t : producers) {
        t.join();
    }
    ex.stop_and_join();
    std::printf("done: %d hook alternations, %ld producer pushes\n",
                iterations, total_pushes.load());
    return 0;
}
