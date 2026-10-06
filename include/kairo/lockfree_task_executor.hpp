#pragma once

#include <functional>
#include <memory>
#include <thread>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace kairo {

// Forward declaration
namespace util {
template<typename T> class LockFreeQueue;
template<typename T> class ObjectPool;
struct LockFreeQueueStats;
}

/**
 * @brief 无锁任务执行器
 *
 * 封装无锁队列和消费者线程，提供高性能任务提交接口。
 * 支持多个线程并发调用 push_task()，单个消费者线程处理任务（MPSC模式）。
 *
 * 使用场景：
 * - 高频日志收集
 * - 异步事件处理
 * - 性能敏感路径的任务分发
 * - 多线程环境下的任务聚合
 */
class LockFreeTaskExecutor {
public:
    enum class QueueFailReason : uint8_t {
        None,
        QueueFull,
        Contention,
        ReservationCancelled,
    };
    /**
     * @brief 构造函数
     * @param queue_capacity 队列容量（非 2 的幂会向上取整到 2 的幂；对象池与
     *                       push_tasks_batch 上限按取整后的环容量统一分配，
     *                       实际可用槽位为取整后容量减一）
     * @param backoff_multiplier CAS 退避倍数（默认2，适合中等竞争场景）；必须大于0，
     *                           超过 LockFreeQueue::kMaxBackoffMultiplier 时会被钳制
     * @param enable_stats 是否启用性能统计（默认false）
     */
    explicit LockFreeTaskExecutor(size_t queue_capacity = 1024, size_t backoff_multiplier = 2, bool enable_stats = false);

    /**
     * @brief 析构函数（自动停止）
     */
    ~LockFreeTaskExecutor();

    // 禁止拷贝和移动
    LockFreeTaskExecutor(const LockFreeTaskExecutor&) = delete;
    LockFreeTaskExecutor& operator=(const LockFreeTaskExecutor&) = delete;

    /**
     * @brief 启动消费者线程
     * @return 成功返回true；已启动或停止已开始返回false。stop_and_join() 与
     * start() 并发时按同一生命周期锁线性化；一旦停止开始，之后不能重启。
     */
    bool start();

    /**
     * @brief 停止消费者线程并等待
     */
    void stop();

    /**
     * @brief 请求停止并在外部线程中等待消费者线程结束
     * @return 外部调用返回 true；消费者线程内调用返回 false。与 start() 并发时，
     * 先获得生命周期锁的操作决定结果。
     */
    bool stop_and_join();

    /**
     * @brief 检查是否正在运行
     */
    bool is_running() const;

    /**
     * @brief 提交任务到无锁队列（线程安全，支持多线程并发调用）
     * @param task 任务函数
     * @return 成功返回true，空任务、队列满或 stop() 后返回false
     */
    bool push_task(std::function<void()> task);

    /**
     * @brief 批量提交任务
     * @param tasks 任务数组
     * @param count 任务数量
     * @param pushed 实际提交的任务数量（输出参数）。返回 true 时等于 count；
     *               返回 false 时为 0。
     * @return 全部入队返回 true；空输入、stop() 后、队列空间不足、对象池耗尽或
     *         内部临时分配失败返回 false
     */
    bool push_tasks_batch(const std::function<void()>* tasks, size_t count, size_t& pushed);

    /**
     * @brief 获取队列中待处理任务数（近似值）
     */
    size_t pending_count() const;

    /**
     * @brief 获取已处理任务总数
     */
    uint64_t processed_count() const;

    /**
     * @brief 获取任务执行过程中累计捕获的异常次数
     *
     * P-260618-006: 之前 LockFreeTaskExecutor 对任务抛出的异常是完全
     * 静默吞噬 (catch (...) {}), 与 ThreadPool/RealtimeThreadExecutor 行为
     * 不一致, 属于可观测性盲区. 该计数器始终累计, 不受 enable_stats
     * 影响, 是核心可观测性指标.
     */
    uint64_t exception_count() const;

    /**
     * @brief 获取因空 std::function 输入被拒绝的累计次数
     *
     * 空任务是提交端输入错误，不会进入队列，也不会计入 exception_count()
     * 或 processed_count()。
     */
    uint64_t rejected_empty_count() const;

    /**
     * @brief 注册异常处理器(可选). 任务抛出异常时, worker 线程会调用
     *        此回调, 传入 std::exception_ptr.
     *
     * P-260618-006: 即使没有注册 handler, exception_count 也会递增;
     * handler 是可选的扩展点, 让用户可以拿到 exception_ptr 并自定义
     * 日志/上报/重试逻辑. 默认行为(无 handler)与修复前完全一致
     * (不抛、不重试), 仅多了一个计数器.
     */
    void set_exception_handler(std::function<void(std::exception_ptr)> handler);

    /**
     * @brief 获取队列性能统计
     */
    struct QueueStats {
        uint64_t total_pushes;
        uint64_t failed_pushes;
        uint64_t queue_full_rejections;
        uint64_t total_pops;
        uint64_t empty_pops;
        uint64_t batch_pushes;
        uint64_t batch_pops;
        uint64_t current_size;
        uint64_t peak_size;
        // The adjusted power-of-two capacity of the underlying queue.
        uint64_t queue_capacity;
        uint64_t reserved_count;
        uint64_t reservation_count;
        uint64_t ready_count;
        uint64_t contention_rejection;
        uint64_t reservation_cancelled_rejections;
        uint64_t cancelled_reservation_count;
        // Rejections before a task reaches the queue (empty input, stopped
        // executor, or object-pool exhaustion).
        uint64_t submission_rejection;
        uint64_t reservation_wait_yields;
        QueueFailReason fail_reason;
        // P-260618-006: 暴露异常计数, 与 processed_count() 一起是任务执行
        // 端到端可观测性的两个核心指标.
        uint64_t exception_count;
        // 因空任务输入被拒绝的次数；不受 enable_stats 影响。
        uint64_t rejected_empty_count;
        // 成功入队数占全部入队尝试数的比例:
        // total_pushes / (total_pushes + failed_pushes).
        double success_rate;
    };
    QueueStats get_queue_stats() const;

    /**
     * @brief Returns a non-synchronized, value-typed queue status snapshot.
     *
     * This is suitable for monitoring only: concurrent producers and the
     * consumer may advance between individual field loads.
     */
    QueueStats get_status_snapshot() const;

    /**
     * @brief Returns a full per-slot diagnostic queue snapshot.
     *
     * This method scans every queue slot, so it is O(queue_capacity) and is
     * intended only for infrequent diagnostics. Like get_status_snapshot(),
     * it is non-synchronized and may observe concurrent state transitions.
     * `reserved_count` includes slots in Reserved or Writing; `ready_count`
     * includes Published slots. These per-slot counts require
     * `enable_stats=true`.
     */
    QueueStats expensive_diagnostic_snapshot() const;

    // Test/debug hook: invoked after a producer reserves a slot and before it
    // enters the non-interruptible write window. This method may be called
    // concurrently with push_task(); each producer observes either the old
    // complete (hook, context) pair or the new complete pair. Keep context
    // valid until the hook is cleared or replaced and concurrent producers
    // have stopped.
    using BeforePublishHook = void (*)(void*);
    void set_before_publish_hook(BeforePublishHook hook, void* context);

    // Test-only hook invoked immediately before push_tasks_batch allocates its
    // temporary wrapper-pointer array. It may throw to simulate allocation
    // failure; do not change it while producers are submitting batches.
    using BeforeBatchAllocationHook = void (*)(void*);
    void set_before_batch_allocation_hook_for_test(BeforeBatchAllocationHook hook,
                                                   void* context);

    // Test-only hook invoked immediately after a producer wins the admission
    // CAS in enter_push() and before it touches the object pool or queue.
    // Blocking inside it simulates a producer inside the submission path;
    // stop_and_join() must not return until every such producer leaves.
    // Not invoked when admission is rejected; do not change it while
    // producers are submitting.
    using AdmissionRegisteredHook = void (*)(void*);
    void set_admission_registered_hook_for_test(AdmissionRegisteredHook hook,
                                                void* context);

protected:
    virtual std::thread create_worker_thread();

private:
    struct TaskWrapper {
        std::function<void()> func;
    };

    bool enter_push();
    void leave_push();
    void worker_thread();
    size_t park_worker(TaskWrapper** batch, size_t batch_size);
    void wake_worker_if_parking();
    QueueStats make_queue_stats(const util::LockFreeQueueStats& raw) const;

    std::unique_ptr<util::LockFreeQueue<TaskWrapper*>> queue_;
    std::unique_ptr<util::ObjectPool<TaskWrapper>> task_pool_;
    size_t task_pool_capacity_;

    std::thread worker_;
    std::thread::id worker_id_;
    std::mutex stop_mutex_;
    std::atomic<bool> self_stop_requested_{false};
    std::atomic<bool> running_{false};
    // P-001: single-atom admission gate. Bit 31 marks the gate closed by
    // stop_and_join(); the low bits count producers that won admission and
    // are still inside the submission path. Registration and the closed
    // check are one RMW, so a producer either counts itself before the close
    // — and stop waits for it before joining/draining — or rejects without
    // touching the pool, queue, or any other member. The previous
    // check-increment-recheck enter_push() let stop observe a zero count
    // while a producer sat between its stopped_ load and the fetch_add,
    // letting stop return and the destructor free the object under it.
    // The RMW protocol itself lives in src/kairo/util/admission_gate.hpp
    // (shared with RealtimeThreadExecutor); storage stays here because this
    // is a public header and cannot depend on internal src/ headers.
    std::atomic<uint32_t> push_gate_{0};
    std::atomic<uint64_t> processed_count_{0};
    // P-260618-006: 累计异常计数, 始终累计, worker 线程写, 读取方任意线程.
    std::atomic<uint64_t> exception_count_{0};
    // 空任务属于提交拒绝，而不是 worker 执行异常。
    std::atomic<uint64_t> rejected_empty_count_{0};
    std::atomic<uint64_t> submission_rejection_{0};
    BeforeBatchAllocationHook before_batch_allocation_hook_{nullptr};
    void* before_batch_allocation_context_{nullptr};
    AdmissionRegisteredHook admission_registered_hook_{nullptr};
    void* admission_registered_context_{nullptr};
    // P-260618-006: 可选异常回调. 在 worker 线程中调用, 需自行保证线程安全.
    std::mutex exception_handler_mutex_;
    std::function<void(std::exception_ptr)> exception_handler_;
    uint32_t idle_count_{0};  // only accessed from worker_thread

    // PA-8: 空闲驻停（替代原 1µs-sleep 永久轮询，约 10⁶ syscall/s/核）。
    // wake_seq_ 必须是 32 位：libstdc++ 仅对 4 字节标量走 futex 直达路径，
    // 64 位会落入内部 mutex+condvar waiter 池（与 ThreadPool P1 相同结论）。
    //
    // 编码：bit0 = 驻停标志（仅 worker 置位/清除），bit1..30 = 唤醒计数
    // （生产者每次 push 成功后 fetch_add(2) 无条件递增）。生产者的 RMW
    // 是全序操作（x86 lock 指令排空 store buffer）：push 的发布存储必然
    // 先于该 RMW 全局可见——worker 端要么置位后终扫看到任务，要么
    // wait 的原子复核看到计数变化，两者不可能同时错过（条件 load 检查
    // 会被 StoreLoad 重排打穿，故驻停标志必须并入被 RMW 的同一字）。
    // 通知条件式：仅当 RMW 返回值带驻停位才 notify_one，忙碌路径
    // 零 syscall、一次 lock xadd。
    static constexpr uint32_t kParkedBit = uint32_t{1} << 0;
    alignas(64) std::atomic<uint32_t> wake_seq_{0};
    // set_before_publish_hook 安装的用户回调与上下文；队列里装的是
    // 会先唤醒驻停 worker 的 trampoline（见 .cpp）。
    // CR-004：用户 hook 以不可变快照节点发布，trampoline（生产者线程）
    // 一次原子加载后经本地副本调用。节点一经发布不可释放（无 epoch/hazard
    // 回收机制，无法确认旧指针不再被生产者持有），按 (hook, context) 去重
    // 常驻进程生命周期——诊断路径，节点 16 字节，数量等于不同配置数。
    struct BeforePublishHookState {
        BeforePublishHook hook;
        void* context;
    };
    std::atomic<BeforePublishHookState*> user_before_publish_state_{nullptr};
    // 配置路径专用（trampoline 不触碰）：持有快照节点所有权并按配置去重。
    std::mutex user_before_publish_states_mutex_;
    std::vector<std::unique_ptr<BeforePublishHookState>> user_before_publish_states_;
};

} // namespace kairo
