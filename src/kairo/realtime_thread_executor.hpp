#pragma once

#include "kairo/interfaces.hpp"
#include "kairo/config.hpp"
#include "kairo/types.hpp"
#include "util/lockfree_queue.hpp"
#include "util/exception_handler.hpp"
#include "util/thread_utils.hpp"
#include "util/object_pool.hpp"
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <utility>

namespace kairo {

static_assert(std::atomic<int64_t>::is_always_lock_free,
              "RealtimeThreadExecutor statistics require lock-free int64_t atomics");

/**
 * @brief 实时线程执行器
 * 
 * 实现 IRealtimeExecutor 接口，提供专用实时线程执行功能。
 * 用于处理高实时性任务（如实时通信、传感器采集），支持：
 * - 高优先级线程
 * - CPU 亲和性设置
 * - 精确周期控制（使用 std::this_thread::sleep_until）
 * - 无锁队列任务传递
 * - 周期统计和监控
 * - 异常处理
 */
class RealtimeThreadExecutor : public IRealtimeExecutor {
public:
    /**
     * @brief 构造函数
     *
     * @param name 执行器名称
     * @param config 实时线程配置
     * @param enable_stats 是否启用 LockFreeQueue 内部统计 (failed_pushes / peak_queue_size).
     *                     默认 false (零开销路径); P-001 (260615) 测试与生产监控建议 true.
     * @param queue_capacity RT 无锁队列容量 (默认 1024, 与既有行为兼容).
     */
    explicit RealtimeThreadExecutor(const std::string& name,
                                    const RealtimeThreadConfig& config,
                                    bool enable_stats = false,
                                    size_t queue_capacity = 1024);

    /**
     * @brief 析构函数
     *
     * 自动停止执行器并等待线程结束
     */
    ~RealtimeThreadExecutor();

    // 禁止拷贝和移动
    RealtimeThreadExecutor(const RealtimeThreadExecutor&) = delete;
    RealtimeThreadExecutor& operator=(const RealtimeThreadExecutor&) = delete;
    RealtimeThreadExecutor(RealtimeThreadExecutor&&) = delete;
    RealtimeThreadExecutor& operator=(RealtimeThreadExecutor&&) = delete;

    /**
     * @brief 启动实时线程
     * 
     * 创建实时线程，设置优先级和 CPU 亲和性，开始周期循环。
     * 
     * @return 如果启动成功返回 true，否则返回 false
     */
    bool start() override;

    /**
     * @brief 停止实时线程
     * 
     * 停止周期循环并等待线程结束。
     */
    void stop() override;

    /**
     * @brief 请求停止并在外部线程中等待实时线程结束
     * @return 外部调用返回 true；实时线程内调用返回 false
     */
    bool stop_and_join();

    /**
     * @brief 推送任务到无锁队列（在周期回调中处理）
     *
     * 任务通过无锁队列传递，在实时线程的下一个周期回调中执行。
     * 未运行、空任务、队列满或对象池耗尽时拒绝并计入 dropped_task_count_
     * (始终累计, 不依赖 enable_stats)。返回值直接反映该次 push 的成败
     * (无 toctou)；调用方也可通过 get_status() 观察 dropped_task_count
     * 与 failed_pushes。
     *
     * @param task 任务函数
     * @return ExecutorResult 任务被接受时 ok；否则携带失败类别
     */
    ExecutorResult push_task(std::function<void()> task) override;

    // Test-only hook invoked immediately after a producer wins the admission
    // CAS in enter_push() and before it observes running_. Blocking inside it
    // simulates a producer inside the submission path; stop_and_join() must
    // not return until every such producer leaves. Not invoked when admission
    // is rejected; do not change it while producers are submitting.
    using AdmissionRegisteredHook = void (*)(void*);
    void set_admission_registered_hook_for_test(AdmissionRegisteredHook hook,
                                                void* context);

    /**
     * @brief 获取执行器名称
     * @return 执行器名称
     */
    std::string get_name() const override;

    /**
     * @brief 获取执行器状态
     * @return 实时执行器状态
     */
    RealtimeExecutorStatus get_status() const override;

private:
    /**
     * @brief 简单周期循环（内置默认实现）
     * 
     * 使用 std::this_thread::sleep_until 实现精确周期控制。
     * 在每个周期中：
     * 1. 执行周期回调函数
     * 2. 处理无锁队列中的任务
     * 3. 更新统计信息
     * 4. 等待下一个周期
     */
    void simple_cycle_loop();

    /**
     * @brief 周期循环（使用外部周期管理器）
     * 
     * 当使用 ICycleManager 时，此方法被周期管理器在每个周期调用。
     * 执行周期回调、处理任务、更新统计，但不执行 sleep（由周期管理器负责）。
     */
    void cycle_loop();

    /**
     * @brief 处理无锁队列中的任务
     * 
     * 从无锁队列中弹出所有任务并执行，捕获异常。
     */
    void process_tasks();

    /**
     * @brief Drain queued tasks after the realtime thread has stopped.
     *
     * Caller must hold drain_mutex_ and must only call this after the realtime
     * thread is not running; LockFreeQueue has a single-consumer pop path.
     */
    void drain_stopped_queue();

    /**
     * @brief P-002: atomically register with the stop admission gate.
     *
     * Returns false once stop_and_join() has closed the gate; the caller must
     * not touch any other member in that case. Pair every success with
     * leave_push().
     */
    bool enter_push();
    void leave_push();

    /**
     * @brief 更新周期统计信息
     * 
     * @param cycle_time_ns 当前周期执行时间（纳秒）
     */
    void update_statistics(int64_t cycle_time_ns);

    struct TaskWrapper {
        std::function<void()> func;
    };

    using ThreadFactory = std::function<std::thread(std::function<void()>)>;

    std::string name_;                              // 执行器名称
    RealtimeThreadConfig config_;                   // 实时线程配置
    std::thread thread_;                            // 实时线程
    ThreadFactory thread_factory_{
        [](std::function<void()> entry) {
            return std::thread(std::move(entry));
        }
    };
    std::atomic<bool> running_{false};              // 运行状态标志
    std::thread::id worker_id_;
    // Lock order: stop_mutex_ is the sole lifecycle lock. Never invoke an
    // ICycleManager method while holding it because managers may synchronously
    // invoke a callback that calls stop_and_join().
    std::mutex stop_mutex_;
    std::condition_variable stop_completion_cv_;
    std::atomic<bool> stopping_{false};
    bool stop_finalization_in_progress_{false};
    std::atomic<bool> cycle_manager_active_{false};
    std::atomic<bool> self_stop_requested_{false};
    // P-002: single-atom admission gate for push_task(). Bit 31 marks the
    // gate closed by stop_and_join(); the low bits count producers that won
    // admission and are still inside push_task(). Registration and the
    // closed check are one RMW, so a producer either counts itself before the
    // close — and stop waits for it before the final drain — or rejects
    // without touching the object pool, queue, or any other member. The
    // previous unconditional fetch_add let stop observe a zero count while a
    // producer sat before the increment, letting stop drain, return, and the
    // destructor free the object under it.
    static constexpr uint32_t kPushGateClosedBit = uint32_t{1} << 31;
    static constexpr uint32_t kPushGateActiveMask = kPushGateClosedBit - 1;
    std::atomic<uint32_t> push_gate_{0};
    AdmissionRegisteredHook admission_registered_hook_{nullptr};
    void* admission_registered_context_{nullptr};

    // 无锁队列（直接传递任务指针）
    util::LockFreeQueue<TaskWrapper*> lockfree_queue_;

    // 对象池（预分配任务对象）
    util::ObjectPool<TaskWrapper> task_pool_;
    
    // 异常处理器
    util::ExceptionHandler exception_handler_;
    
    // 统计信息（使用原子变量支持并发访问）
    std::atomic<int64_t> cycle_count_{0};          // 周期计数
    std::atomic<int64_t> cycle_timeout_count_{0};   // 周期超时计数
    std::atomic<int64_t> avg_cycle_time_ns_{0};     // 平均周期执行时间（纳秒）
    std::atomic<int64_t> max_cycle_time_ns_{0};     // 最大周期执行时间（纳秒）
    std::atomic<bool> priority_applied_{false};
    std::atomic<bool> cpu_affinity_applied_{false};
    std::atomic<bool> process_memory_lock_applied_{false};
    std::atomic<int> process_memory_lock_errno_{0};
    std::atomic<bool> timer_slack_applied_{false};

    // P-001 (260615): 背压可见性 — 始终累计, 与 enable_stats 无关.
    // 未运行 / 空任务 / 队列满 / 对象池耗尽 任一路径触发 drop 时 +1.
    std::atomic<uint64_t> dropped_task_count_{0};
    std::atomic<uint64_t> rejected_not_running_count_{0};
    std::atomic<uint64_t> rejected_empty_task_count_{0};
    std::atomic<uint64_t> pool_exhausted_count_{0};
    std::atomic<uint64_t> queue_full_count_{0};
    std::atomic<uint64_t> cycle_manager_error_count_{0};
    // P-001 (260615): 构造时指定的统计开关, push_task() 路径不依赖此开关.
    const bool enable_stats_;
    
};

} // namespace kairo
