// CR-012: submit_on* 捕获 SerialExecutionContext& 裸引用悬垂
//（executor.hpp:1217 complete_terminal 捕获 &context；1432 publish_task 捕获
//  &context；1456 TicketGuard 持有 SerialExecutionContext* context）
//
// 结构事实（include/executor/serial_execution_context.hpp:21）：
//   ~SerialExecutionContext() → shutdown() → join 自己的串行 worker
//   ⇒ "任务体在串行线程 sleep、随后析构 context"会被 join 掩盖（variant B）。
//   真正不等待的悬垂窗口在池侧：publish_task 在默认池 worker 上运行，持有
//   SerialExecutionContext& / TicketGuard::context 裸指针（variant A）。
//   先占满全部池 worker → submit_on → 立刻析构 context → 占位任务结束后
//   publish_task 才运行 → 触达已析构对象。
//
// 运行提示：默认 ASAN 会用 0x55 填充已释放内存，pthread_mutex_lock（libc，
// 未插桩）读到垃圾 mutex 状态会 futex 永久等待而非触发插桩检查；
// 用 ASAN_OPTIONS=max_free_fill_size=0 保留原 mutex 字节，让锁正常通过、
// 后续对已释放对象的插桩读（stopping_/mutex_ 所在 chunk）触发
// heap-use-after-free 报告。
#include <kairo/executor.hpp>
#include <kairo/serial_execution_context.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

using namespace kairo;
using namespace std::chrono_literals;

static void log(const char* msg) {
    std::printf("%s\n", msg);
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "A";

    Executor ex;
    if (!ex.initialize(ExecutorConfig{})) {
        log("initialize failed");
        return 1;
    }

    if (mode[0] == 'A') {
        // variant A：占满池 worker → submit_on → 立刻析构 context → 放行池
        std::vector<std::future<void>> blockers;
        // 20 > max_threads(=hw=14)：保证 submit_on 之后所有池 worker 都被
        // 占位，publish_task 必须等到占位任务结束才有 worker 可用，
        // ctx.reset()（立即执行）必赢竞争。
        for (int i = 0; i < 20; ++i) {
            blockers.push_back(ex.submit([] {
                std::this_thread::sleep_for(250ms);
            }));
        }
        auto ctx = std::make_unique<SerialExecutionContext>();
        std::atomic<bool> serial_ran{false};
        auto sub = ex.submit_on(*ctx, [&serial_ran] { serial_ran.store(true); });
        (void)sub;
        log("[A] submit_on done, destroying context now");
        ctx.reset();  // 析构：置 stopping_ + join 串行 worker（空闲，立即返回）
        log("[A] context destroyed; pool still busy with blockers");
        for (auto& f : blockers) {
            if (f.wait_for(10s) != std::future_status::ready) {
                log("[A] blocker timeout");
                return 2;
            }
            f.get();
        }
        // 此刻 publish_task 才被某个 worker 取出 → 触达已析构 *ctx
        log("[A] blockers drained; publish_task now runs against freed context");
        std::this_thread::sleep_for(500ms);
        auto fstat = sub.wait_for(std::chrono::seconds(0));
        std::printf("[A] serial_ran=%d future_ready=%d\n",
                    static_cast<int>(serial_ran.load()),
                    static_cast<int>(fstat == std::future_status::ready));
        std::fflush(stdout);
        log("[A] reached end without crash (unexpected for ASAN run)");
        (void)ex.shutdown(true);
        return 0;
    }

    if (mode[0] == 'B') {
        // variant B：先等 publish 完成、串行任务开跑，再析构 context。
        // 预期：~SerialExecutionContext join 串行 worker → 析构等待任务体
        // 完成（掩盖任务体访问），flag=1、dtor 耗时 ≈ 任务剩余 sleep。
        auto ctx = std::make_unique<SerialExecutionContext>();
        std::atomic<bool> flag{false};
        auto sub = ex.submit_on(*ctx, [&flag] {
            std::this_thread::sleep_for(300ms);
            flag.store(true);
        });
        std::this_thread::sleep_for(50ms);  // publish 已发生、串行任务在跑
        auto start = std::chrono::steady_clock::now();
        ctx.reset();
        auto dtor_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
        std::printf("[B] context dtor took %lldms, flag=%d (dtor joins serial "
                    "worker => dtor waits)\n",
                    static_cast<long long>(dtor_ms),
                    static_cast<int>(flag.load()));
        std::fflush(stdout);
        (void)sub.wait_for(std::chrono::seconds(5));
        (void)ex.shutdown(true);
        return 0;
    }

    log("unknown mode");
    return 2;
}
