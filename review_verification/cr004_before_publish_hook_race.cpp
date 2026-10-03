// CR-004: LockFreeTaskExecutor::set_before_publish_hook 对
// user_before_publish_hook_ / user_before_publish_context_ 的普通写
// （lockfree_task_executor.cpp:326-327）与 trampoline lambda 在生产者线程中的
// 普通读（lockfree_task_executor.cpp:336-337）构成数据竞争。
//
// 方法（TSAN 构建）：1 worker；3 个生产者线程持续 push_task；主线程交替
// set_before_publish_hook(hookA, nullptr) / (nullptr, nullptr) 共 20 万次。
// 若 TSAN 报出涉及这两个字段、写栈含 set_before_publish_hook、读栈含
// trampoline（begin_write → hook 调用 → push_task）的 data race → CONFIRMED。
//
// 编译（TSAN）：g++ -std=c++20 -O1 -g -fsanitize=thread
//   -fno-omit-frame-pointer -I include -I src cr004.cpp
//   build-tsan/src/libexecutor.a -lpthread -ldl -o cr004_tsan
// 运行：TSAN_OPTIONS="second_deadlock_stack=1 halt_on_error=0" ./cr004_tsan

#include "kairo/lockfree_task_executor.hpp"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_producers_stop{false};

void hookA(void*) {}  // 空函数 hook

}  // namespace

int main() {
    kairo::LockFreeTaskExecutor exec(1024);
    if (!exec.start()) {
        std::fprintf(stderr, "FATAL: executor start failed\n");
        return 2;
    }

    constexpr int kProducers = 3;
    std::vector<std::thread> producers;
    std::atomic<uint64_t> pushed{0};
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&] {
            uint64_t local = 0;
            while (!g_producers_stop.load(std::memory_order_relaxed)) {
                if (exec.push_task([] {})) {
                    ++local;
                } else {
                    std::this_thread::yield();
                }
            }
            pushed.fetch_add(local, std::memory_order_relaxed);
        });
    }

    // 主线程反复换装/摘除 hook：换装时 trampoline 生效（生产者在 begin_write
    // 中读取 user_before_publish_* 普通字段），摘除时主线程普通写这些字段。
    constexpr int kIterations = 200000;
    for (int i = 0; i < kIterations; ++i) {
        if (i % 2 == 0) {
            exec.set_before_publish_hook(&hookA, nullptr);
        } else {
            exec.set_before_publish_hook(nullptr, nullptr);
        }
    }

    g_producers_stop.store(true, std::memory_order_relaxed);
    for (auto& t : producers) {
        t.join();
    }
    exec.set_before_publish_hook(nullptr, nullptr);
    exec.stop();

    std::printf("done: iterations=%d pushed=%llu processed=%llu\n",
                kIterations, static_cast<unsigned long long>(pushed.load()),
                static_cast<unsigned long long>(exec.processed_count()));
    return 0;
}
