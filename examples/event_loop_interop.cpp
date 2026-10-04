// event_loop_interop.cpp
// 《外部事件循环互操作》指南的可编译伴随示例（docs/external_event_loop_interop.md）。
//
// 本示例不依赖 asio：用一个 MiniEventLoop（互斥量 + 双端队列 + 条件变量实现
// 的串行派发循环）等价复现 asio strand/io_context 的互操作模式——
//
//   模式 1（现行合规）：把事件循环托管为 Blocking I/O worker，
//             run(StopToken) 驱动循环，wakeup() 唤醒，start_worker 收尾；
//   模式 2（盲区纪律）：线程池任务通过 post() 把延续派发回串行循环。
//             这些 post 级派发不进入 executor 的 admission/统计/失败事件，
//             因此状态必须由 shared_ptr 拥有、延续只做对象内移交；
//   模式 3（收尾同步）：串行循环每完成一批工作推进 PhaseGate 相位，
//             线程池侧用 wait_for() 等待批次完成（无锁、可超时）。
//
// 对应到 asio：MiniEventLoop ≈ io_context + strand；post() ≈ asio::post(strand,
// ...)；run(StopToken) ≈ io_context::run() + stop 信号；wakeup() ≈
// post 一个空任务或 stop()。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <kairo/comm.hpp>
#include <kairo/executor.hpp>

using namespace kairo;
using namespace std::chrono_literals;

namespace {

// ---------------------------------------------------------------------------
// MiniEventLoop：asio strand 的最小等价物（所有 post 的任务串行执行）。
// 托管为 IBlockingIoWorker：生命周期、线程命名与 stop 语义全部交给 executor。
// ---------------------------------------------------------------------------
class MiniEventLoopWorker final : public IBlockingIoWorker {
public:
    // 相当于 asio::post(strand, task)：任意线程调用；任务严格串行执行。
    void post(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(task));
        }
        cv_.notify_all();
    }

    // 相当于 io_context::run()：托管线程上串行执行所有已 post 的任务，
    // 直到 stop_token 置位并排空。
    void run(StopToken stop_token) override {
        std::unique_lock<std::mutex> lock(mutex_);
        running_.store(true, std::memory_order_release);
        cv_.notify_all();
        for (;;) {
            cv_.wait(lock, [this, &stop_token] {
                return !queue_.empty() || stop_token.stop_requested();
            });
            while (!queue_.empty()) {
                auto task = std::move(queue_.front());
                queue_.pop_front();
                lock.unlock();
                task();  // 串行执行：此后回调内访问的对象无需额外加锁
                lock.lock();
            }
            if (stop_token.stop_requested()) {
                break;
            }
        }
        running_.store(false, std::memory_order_release);
    }

    // executor 停止路径调用：让 run() 的等待立刻醒来检查 stop_token。
    void wakeup() noexcept override {
        cv_.notify_all();
    }

    bool wait_until_running(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] {
            return running_.load(std::memory_order_acquire);
        });
    }

private:
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
};

}  // namespace

int main() {
    Executor executor;
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    if (!executor.initialize(config)) {
        std::cerr << "failed to initialize executor\n";
        return 1;
    }

    // -----------------------------------------------------------------------
    // 模式 1：把事件循环托管为 Blocking I/O worker（现行合规路线）。
    // -----------------------------------------------------------------------
    auto loop = std::make_unique<MiniEventLoopWorker>();
    MiniEventLoopWorker* loop_view = loop.get();

    BlockingIoConfig worker_config;
    worker_config.thread_name = "event_loop";
    WorkerHandle worker = executor.start_worker(
        BlockingWorkerSpec{"event_loop", worker_config, std::move(loop)});
    if (!worker.status().is_running ||
        !loop_view->wait_until_running(1000ms)) {
        std::cerr << "failed to start hosted event loop\n";
        return 1;
    }
    std::cout << "[1] event loop hosted as blocking I/O worker\n";

    // -----------------------------------------------------------------------
    // 模式 2：pool 任务 -> strand 延续（post 级派发的盲区纪律）。
    //
    // 盲区事实：loop_view->post(...) 的任务不经过 executor 提交路径，
    // 不进入 admission、TaskStatistics 与 failure 事件。纪律：
    //   a) 跨界状态用 shared_ptr 拥有，移交后原线程不再访问；
    //   b) 延续内部异常必须自捕获（盲区里没人替你记录失败）。
    // -----------------------------------------------------------------------
    auto value = std::make_shared<std::atomic<int>>(0);
    auto stage = std::make_shared<std::atomic<int>>(0);

    auto producer = executor.submit([value, stage, loop_view]() noexcept {
        value->store(41, std::memory_order_release);
        stage->store(1, std::memory_order_release);

        // 延续派发回串行循环：此后 value/stage 只在 strand 上访问。
        loop_view->post([value, stage]() noexcept {
            value->fetch_add(1, std::memory_order_acq_rel);
            stage->store(2, std::memory_order_release);
        });
    });
    producer.wait();

    while (stage->load(std::memory_order_acquire) != 2) {
        std::this_thread::yield();
    }
    std::cout << "[2] pool -> strand continuation, value=" << value->load()
              << " (post dispatches are outside executor statistics)\n";

    // -----------------------------------------------------------------------
    // 模式 3：PhaseGate 收尾——串行批次推进相位，pool 侧等待。
    // -----------------------------------------------------------------------
    comm::PhaseGate gate("event_loop_batch");
    constexpr uint64_t kBatchSize = 4;

    auto batch_future = executor.submit([&gate, kBatchSize]() {
        // pool 线程等待串行侧完成整批工作；wait_for 支持超时，不会无限等。
        const comm::CommResult result = gate.wait_for(kBatchSize, 5s);
        return result.ok;
    });

    for (uint64_t i = 0; i < kBatchSize; ++i) {
        loop_view->post([i, &gate]() noexcept {
            // 每个串行任务完成一批次中的一步，然后推进相位。
            gate.advance_to(i + 1);
        });
    }

    const bool batch_ok = batch_future.get();
    std::cout << "[3] phase gate batch finalized, ok=" << batch_ok << "\n";

    // -----------------------------------------------------------------------
    // 收尾：先停止托管循环（run 排空返回），再 shutdown 线程池。
    // -----------------------------------------------------------------------
    worker.request_stop();
    loop_view->wakeup();
    worker.stop();
    std::cout << "[4] hosted loop stopped: "
              << (worker.status().is_running ? "still running" : "stopped")
              << "\n";

    executor.shutdown();
    std::cout << "event loop interop example completed\n";
    return batch_ok ? 0 : 1;
}
