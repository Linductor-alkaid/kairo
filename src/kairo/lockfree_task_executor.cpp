#include "kairo/lockfree_task_executor.hpp"
#include "util/admission_gate.hpp"
#include "util/cpu_pause.hpp"
#include "util/lockfree_queue.hpp"
#include "util/object_pool.hpp"
#include <chrono>
#include <thread>
#include <vector>

namespace kairo {
namespace {
// CR-031: 归还对象池前清空 callable。TaskWrapper 复用模型下 acquire 不
// 构造、release 不析构——旧任务的 func（可能持有文件句柄、shared_ptr、
// 大缓冲）此前滞留池节点，直到随机生产者线程复用节点赋值时才析构，长期
// 不复用则滞留到池析构。worker 归还前显式置空，把析构成本与副作用移回
// 执行线程，资源释放时机确定。
template <typename Wrapper>
inline void clear_batch_callables(Wrapper** batch, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (batch[i] != nullptr) {
            batch[i]->func = nullptr;
        }
    }
}
}  // namespace


LockFreeTaskExecutor::LockFreeTaskExecutor(size_t queue_capacity, size_t backoff_multiplier, bool enable_stats)
    : queue_(std::make_unique<util::LockFreeQueue<TaskWrapper*>>(queue_capacity, backoff_multiplier, enable_stats))
    // 队列容量会向上取整到 2 的幂；对象池必须按取整后的环容量分配，否则
    // 非 2 的幂请求（如 5 → 环 8、可用 7）会在池处提前背压，实际提交上限
    // 与 get_queue_stats().queue_capacity 报告的容量脱节。
    , task_pool_(std::make_unique<util::ObjectPool<TaskWrapper>>(queue_->capacity()))
    , task_pool_capacity_(queue_->capacity()) {
}

LockFreeTaskExecutor::~LockFreeTaskExecutor() {
    stop();
}

bool LockFreeTaskExecutor::start() {
    std::lock_guard<std::mutex> lock(stop_mutex_);
    if (util::admission_is_closed(push_gate_)) {
        return false;
    }

    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return false;
    }

    try {
        worker_ = create_worker_thread();
    } catch (...) {
        running_.store(false, std::memory_order_release);
        worker_id_ = std::thread::id{};
        return false;
    }
    return true;
}

std::thread LockFreeTaskExecutor::create_worker_thread() {
    return std::thread(&LockFreeTaskExecutor::worker_thread, this);
}

void LockFreeTaskExecutor::stop() {
    (void)stop_and_join();
}

bool LockFreeTaskExecutor::stop_and_join() {
    std::thread joiner;
    {
        std::lock_guard<std::mutex> lock(stop_mutex_);
        // P-001: close the admission gate atomically first. A producer either
        // registered before this RMW — and is waited for below — or observes
        // the closed bit and rejects without touching the pool, queue, or
        // any other member.
        util::admission_close(push_gate_);
        if (std::this_thread::get_id() == worker_id_) {
            self_stop_requested_.store(true, std::memory_order_release);
            running_.store(false, std::memory_order_release);
            return false;
        }

        while (util::admission_has_active(push_gate_)) {
            std::this_thread::yield();
        }
        running_.store(false, std::memory_order_release);
        // PA-8: 唤醒可能驻停在 futex 上的 worker，否则 join 会一直等待。
        // fetch_add(2) 保留 bit0 驻停位语义。
        wake_seq_.fetch_add(2, std::memory_order_release);
        wake_seq_.notify_all();
        if (worker_.joinable()) {
            joiner = std::move(worker_);
        }
    }

    if (joiner.joinable()) {
        joiner.join();
    }
    return true;
}

bool LockFreeTaskExecutor::is_running() const {
    return running_.load(std::memory_order_acquire);
}

bool LockFreeTaskExecutor::push_task(std::function<void()> task) {
    if (!task) {
        rejected_empty_count_.fetch_add(1, std::memory_order_relaxed);
        submission_rejection_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (!enter_push()) {
        submission_rejection_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    auto* wrapper = task_pool_->acquire();
    if (!wrapper) {
        submission_rejection_.fetch_add(1, std::memory_order_relaxed);
        leave_push();
        return false;
    }

    wrapper->func = std::move(task);

    if (!queue_->push(wrapper)) {
        task_pool_->release(wrapper);
        leave_push();
        // PA-8: 失败的提交同样意味着队列可能需要消费者维护——被取消的
        // 槽位（预留取消/批量回滚）要等消费者物理推进后才重新可用，
        // 而驻停的 worker 不会自发发现这一点。
        wake_worker_if_parking();
        return false;
    }

    leave_push();
    // PA-8: worker 空闲驻停时的定向唤醒。忙碌路径只付出一次 acquire load。
    wake_worker_if_parking();
    return true;
}

bool LockFreeTaskExecutor::push_tasks_batch(const std::function<void()>* tasks, size_t count, size_t& pushed) {
    pushed = 0;
    if (!tasks) {
        rejected_empty_count_.fetch_add(1, std::memory_order_relaxed);
        submission_rejection_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (count == 0) {
        return true;
    }
    // The bounded ring always reserves one slot, so no batch this large can
    // succeed. Reject it before scanning caller memory or allocating the
    // temporary pointer array.
    if (count > task_pool_capacity_ || count >= queue_->capacity()) {
        submission_rejection_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!tasks[i]) {
            rejected_empty_count_.fetch_add(1, std::memory_order_relaxed);
            submission_rejection_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    if (!enter_push()) {
        submission_rejection_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    struct PushGuard {
        LockFreeTaskExecutor& executor;

        ~PushGuard() {
            executor.leave_push();
        }
    } push_guard{*this};

    // P-260623-004: keep batch monitoring meaningful by bulk-acquiring
    // wrappers, populating them, then dispatching the whole array in one exact
    // batch call so the queue records a single batch_pushes++ and a single CAS
    // reservation, instead of N independent push() calls.
    //
    // CR-122: thread_local 便签替代每次调用的临时 vector。push gate
    //（enter_push）是多生产者 CAS 门而非互斥，成员缓冲会被并发生产者踩踏；
    // thread_local 保证线程内零堆分配、线程间无共享。容量上界即单批上限
    //（count ≤ task_pool_capacity_，见上方守卫）。
    thread_local std::vector<TaskWrapper*> batch_scratch;
    std::vector<TaskWrapper*>& ptrs = batch_scratch;
    size_t acquired = 0;

    try {
        if (before_batch_allocation_hook_) {
            before_batch_allocation_hook_(before_batch_allocation_context_);
        }
        ptrs.assign(count, nullptr);

        // 1) Bulk-acquire wrappers. If the pool cannot hand out `count` in one
        //    pass we must report a hard failure (matches the previous behaviour
        //    where the first acquire() returning null aborted the whole batch).
        for (size_t i = 0; i < count; ++i) {
            auto* wrapper = task_pool_->acquire();
            if (!wrapper) {
                submission_rejection_.fetch_add(1, std::memory_order_relaxed);
                for (size_t j = 0; j < acquired; ++j) {
                    task_pool_->release(ptrs[j]);
                }
                return false;
            }
            ptrs[i] = wrapper;
            ++acquired;
        }

        // 2) Populate wrappers. An exception while copying a std::function must
        //    release every acquired wrapper so the pool does not leak. We do not
        //    have to undo any queue mutation because exact enqueue happens after.
        for (size_t i = 0; i < count; ++i) {
            ptrs[i]->func = tasks[i];
        }

        // 3) Single exact batched enqueue. LockFreeTaskExecutor exposes atomic
        //    batch semantics: either all wrappers are handed to the queue, or none
        //    are. The queue-level exact helper refuses to reserve a smaller prefix.
        bool ok = queue_->push_batch_exact(ptrs.data(), count);
        if (ok) {
            pushed = count;
            wake_worker_if_parking();
        } else {
            // An unsuccessful exact batch is completely non-observable, so
            // every acquired wrapper remains ours to return to the pool.
            for (size_t i = 0; i < count; ++i) {
                task_pool_->release(ptrs[i]);
            }
            // PA-8: 失败路径已把预留槽位置为 Cancelled；驻停的 worker
            // 需要被唤醒才会推进前沿、释放这些槽位。
            wake_worker_if_parking();
        }

        return ok;
    } catch (...) {
        for (size_t i = 0; i < acquired; ++i) {
            task_pool_->release(ptrs[i]);
        }
        submission_rejection_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}

size_t LockFreeTaskExecutor::pending_count() const {
    return queue_->size();
}

uint64_t LockFreeTaskExecutor::processed_count() const {
    return processed_count_.load(std::memory_order_relaxed);
}

uint64_t LockFreeTaskExecutor::exception_count() const {
    return exception_count_.load(std::memory_order_relaxed);
}

uint64_t LockFreeTaskExecutor::rejected_empty_count() const {
    return rejected_empty_count_.load(std::memory_order_relaxed);
}

void LockFreeTaskExecutor::set_exception_handler(std::function<void(std::exception_ptr)> handler) {
    std::lock_guard<std::mutex> lock(exception_handler_mutex_);
    exception_handler_ = std::move(handler);
}

LockFreeTaskExecutor::QueueStats LockFreeTaskExecutor::get_queue_stats() const {
    return make_queue_stats(queue_->get_stats());
}

LockFreeTaskExecutor::QueueStats LockFreeTaskExecutor::expensive_diagnostic_snapshot() const {
    return make_queue_stats(queue_->expensive_diagnostic_snapshot());
}

LockFreeTaskExecutor::QueueStats LockFreeTaskExecutor::make_queue_stats(
    const util::LockFreeQueueStats& raw) const {
    QueueStats result;
    result.total_pushes = raw.total_pushes;
    result.failed_pushes = raw.failed_pushes;
    result.queue_full_rejections = raw.queue_full_rejections;
    result.total_pops = raw.total_pops;
    result.empty_pops = raw.empty_pops;
    result.batch_pushes = raw.batch_pushes;
    result.batch_pops = raw.batch_pops;
    result.current_size = raw.current_size;
    result.peak_size = raw.peak_size;
    result.queue_capacity = queue_->capacity();
    result.reserved_count = raw.reserved_count;
    result.reservation_count = raw.reservation_count;
    result.ready_count = raw.ready_count;
    result.contention_rejection = raw.contention_rejection;
    result.reservation_cancelled_rejections = raw.reservation_cancelled_rejections;
    result.cancelled_reservation_count = raw.cancelled_reservation_count;
    result.submission_rejection = submission_rejection_.load(std::memory_order_relaxed);
    result.reservation_wait_yields = raw.reservation_wait_yields;
    switch (raw.fail_reason) {
    case util::LockFreeQueueFailReason::QueueFull:
        result.fail_reason = QueueFailReason::QueueFull;
        break;
    case util::LockFreeQueueFailReason::Contention:
        result.fail_reason = QueueFailReason::Contention;
        break;
    case util::LockFreeQueueFailReason::ReservationCancelled:
        result.fail_reason = QueueFailReason::ReservationCancelled;
        break;
    case util::LockFreeQueueFailReason::None:
    default:
        result.fail_reason = QueueFailReason::None;
        break;
    }
    // P-260618-006: expose the exception count alongside the existing queue
    // stats so monitoring code can correlate exceptions with queue state.
    result.exception_count = exception_count_.load(std::memory_order_relaxed);
    result.rejected_empty_count = rejected_empty_count_.load(std::memory_order_relaxed);
    const double total_attempts =
        static_cast<double>(raw.total_pushes) + static_cast<double>(raw.failed_pushes);
    result.success_rate = total_attempts > 0.0
        ? static_cast<double>(raw.total_pushes) / total_attempts
        : 0.0;
    return result;
}

LockFreeTaskExecutor::QueueStats LockFreeTaskExecutor::get_status_snapshot() const {
    return get_queue_stats();
}

void LockFreeTaskExecutor::set_before_publish_hook(BeforePublishHook hook, void* context) {
    // PA-8: hook 是"生产者可能在提交窗口内停滞"的信号（诊断/恢复契约）。
    // worker 驻停后不再轮询，pop 侧的 scan-ahead 取消与 Reserved 前沿
    // 恢复需要消费者活动触发——这里装一层 trampoline，进入 hook 前
    // 定向唤醒驻停的 worker，保证停滞窗口内消费者至少完成一轮 pop
    // 维护。无 hook 时零开销（trampoline 不安装）。
    // CR-004：用户 hook/context 此前是两个普通字段，trampoline 在生产者
    // 线程二次读取，与并发的 set 构成数据竞争（TSAN 下已复现
    // call-through-null 崩溃）。改为单一原子指针发布不可变快照：写者只
    // 替换指针，读者（生产者线程）一次加载后经本地副本调用，看到的一定
    // 是完整且不再变化的 (hook, context) 对。快照节点按配置去重且不释放
    // （无法确认旧指针不再被生产者持有），所有权挂在 user_before_publish_states_。
    if (hook == nullptr) {
        user_before_publish_state_.store(nullptr, std::memory_order_release);
        queue_->set_before_publish_hook(nullptr, nullptr);
        return;
    }
    BeforePublishHookState* node = nullptr;
    {
        std::lock_guard<std::mutex> lock(user_before_publish_states_mutex_);
        for (auto& state : user_before_publish_states_) {
            if (state->hook == hook && state->context == context) {
                node = state.get();
                break;
            }
        }
        if (node == nullptr) {
            auto created = std::make_unique<BeforePublishHookState>();
            created->hook = hook;
            created->context = context;
            node = created.get();
            user_before_publish_states_.push_back(std::move(created));
        }
    }
    user_before_publish_state_.store(node, std::memory_order_release);
    queue_->set_before_publish_hook(
        [](void* self) {
            auto* exec = static_cast<LockFreeTaskExecutor*>(self);
            exec->wake_worker_if_parking();
            // 一次原子加载快照；即使并发 set 换掉了当前指针，本线程取得的
            // 节点仍完整有效（节点不可释放），不存在检查与调用之间的撕裂。
            const auto* state =
                exec->user_before_publish_state_.load(std::memory_order_acquire);
            if (state != nullptr) {
                state->hook(state->context);
            }
        },
        this);
}

void LockFreeTaskExecutor::set_before_batch_allocation_hook_for_test(
    BeforeBatchAllocationHook hook, void* context) {
    before_batch_allocation_context_ = context;
    before_batch_allocation_hook_ = hook;
}

void LockFreeTaskExecutor::set_admission_registered_hook_for_test(
    AdmissionRegisteredHook hook, void* context) {
    admission_registered_hook_ = hook;
    admission_registered_context_ = context;
}

bool LockFreeTaskExecutor::enter_push() {
    // P-001: admission is a single RMW — the closed check and the active
    // count increment cannot be interleaved with stop_and_join()'s close, so
    // a producer that wins this CAS is guaranteed to be waited for.
    if (!util::admission_enter(push_gate_)) {
        return false;
    }
    if (admission_registered_hook_) {
        admission_registered_hook_(admission_registered_context_);
    }
    return true;
}

void LockFreeTaskExecutor::leave_push() {
    util::admission_leave(push_gate_);
}

void LockFreeTaskExecutor::worker_thread() {
    {
        std::lock_guard<std::mutex> lock(stop_mutex_);
        worker_id_ = std::this_thread::get_id();
    }

    constexpr size_t BATCH_SIZE = 32;
    std::vector<TaskWrapper*> batch(BATCH_SIZE);

    while (true) {
        size_t popped = queue_->pop_batch(batch.data(), BATCH_SIZE);

        if (popped == 0) {
            if (!running_.load(std::memory_order_acquire)) {
                break;
            }

            // PA-8: Hybrid backoff: PAUSE spin → yield → 10µs-sleep 轮询
            // 缓冲带 → futex 驻停。原实现为永久 1µs-sleep 轮询，空闲时
            // 约 10⁶ syscall/s/核；驻停后空闲 CPU 占用近零。
            //
            // 缓冲带（10µs × 50 ≈ 500µs）不是可省的过渡：它把「间隔
            // 数百微秒的零星任务」负载钉在轮询捡起路径上——超载机器上
            // 刚被 futex 唤醒的线程要排在持续 runnable 的轮询线程后面，
            // 直接驻停会把这类负载的提交→执行尾延迟从 ~60µs 推到
            // ~150µs+（100µs 级承诺见 benchmark_lockfree_task_executor）。
            // 只有真正长期空闲（≥~0.6ms 无任务）才进入零 CPU 驻停。
            static constexpr uint32_t kPauseSpins  = 32;
            static constexpr uint32_t kYieldThresh = 64;
            static constexpr uint32_t kSleepThresh = kYieldThresh + 50;
            idle_count_++;
            if (idle_count_ <= kPauseSpins) {
                KAIRO_CPU_PAUSE();
            } else if (idle_count_ <= kYieldThresh) {
                std::this_thread::yield();
            } else if (idle_count_ <= kSleepThresh) {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            } else {
                // park_worker 的置位后终扫可能直接带回任务（跳过一次
                // 无谓的驻停+唤醒往返），带回时立即落入下方处理路径。
                popped = park_worker(batch.data(), BATCH_SIZE);
                if (popped > 0) {
                    idle_count_ = 0;
                }
            }
            if (popped == 0) {
                continue;
            }
        }

        if (popped > 0) {
            bool self_stop_interrupted_batch = false;
            size_t executed = 0;
            for (size_t i = 0; i < popped; ++i) {
                try {
                    batch[i]->func();
                } catch (...) {
                    // P-260618-006: surface task exceptions via the
                    // exception_count counter and (optionally) a registered
                    // handler. Default behavior is "count only" — no rethrow,
                    // no crash — preserving back-compat.
                    exception_count_.fetch_add(1, std::memory_order_relaxed);
                    std::function<void(std::exception_ptr)> handler;
                    {
                        std::lock_guard<std::mutex> lock(exception_handler_mutex_);
                        handler = exception_handler_;
                    }
                    if (handler) {
                        try {
                            handler(std::current_exception());
                        } catch (...) {
                            // Swallow exceptions from the handler itself;
                            // the worker must keep draining the queue.
                        }
                    }
                }
                ++executed;
                if (!running_.load(std::memory_order_acquire) &&
                    self_stop_requested_.load(std::memory_order_acquire)) {
                    // 批内自停：已执行的批量回收，剩余的也立即归还。
                    clear_batch_callables(batch.data(), executed);
                    clear_batch_callables(batch.data() + executed,
                                          popped - executed);
                    task_pool_->release_bulk(batch.data(), executed);
                    task_pool_->release_bulk(batch.data() + executed,
                                             popped - executed);
                    processed_count_.fetch_add(i + 1, std::memory_order_relaxed);
                    self_stop_interrupted_batch = true;
                    break;
                }
            }
            if (!self_stop_interrupted_batch) {
                // PA-5: 整批一次 splice 回池（一次 head CAS），消费者侧对
                // 生产者热字 head_ 的往返从每任务一次摊薄为每批一次。
                clear_batch_callables(batch.data(), popped);
                task_pool_->release_bulk(batch.data(), popped);
                processed_count_.fetch_add(popped, std::memory_order_relaxed);
            }
        }
        idle_count_ = 0;
    }

    if (self_stop_requested_.load(std::memory_order_acquire)) {
        return;
    }

    // 处理剩余任务
    size_t popped = 0;
    while ((popped = queue_->pop_batch(batch.data(), BATCH_SIZE)) > 0) {
        for (size_t i = 0; i < popped; ++i) {
            try {
                batch[i]->func();
            } catch (...) {
                // P-260618-006: same handling as in the running loop.
                exception_count_.fetch_add(1, std::memory_order_relaxed);
                std::function<void(std::exception_ptr)> handler;
                {
                    std::lock_guard<std::mutex> lock(exception_handler_mutex_);
                    handler = exception_handler_;
                }
                if (handler) {
                    try {
                        handler(std::current_exception());
                    } catch (...) {
                    }
                }
            }
        }
        clear_batch_callables(batch.data(), popped);
        task_pool_->release_bulk(batch.data(), popped);
        processed_count_.fetch_add(popped, std::memory_order_relaxed);
    }
}

size_t LockFreeTaskExecutor::park_worker(TaskWrapper** batch, size_t batch_size) {
    // PA-8 丢失唤醒封闭推导（CR-005 修订：置位必须用 RMW）。
    // 此前第 (2) 步是"load 旧值 + 普通 store"——生产者的 fetch_add(2) 若
    // 恰好落在 load 与 store 之间，其增量会被 store 覆盖回旧值，且终扫对
    // 刚发布任务没有 happens-before 保证（ARM 弱序下可带任务驻停）。改为
    // fetch_or 后推导重新封闭：
    //  - 生产者 bump（fetch_add）与本线程置位（fetch_or）是同一原子变量
    //    修改序上的 RMW，互相不可覆盖、必分先后；
    //  - 置位先完成：生产者 fetch_add 读到带驻停位的值，返回值触发
    //    notify_one，wait 被定向唤醒；
    //  - bump 先完成：fetch_or 经 RMW 全序读到含增量的值（并建立
    //    happens-before），入队 sequenced-before bump，因此终扫透过队列
    //    原子必然看到该任务，不会驻停。
    //  wait 允许虚假唤醒，醒来重扫。
    //
    // (1) 原子置位（bit0 置位，计数保留）。fetch_or 返回前置值，驻停
    // 比较值必须取"置位后"的值（previous | bit），否则 wait 的比较值
    // 永不含驻停位、与当前值恒不相等，worker 退化为忙轮询。
    const uint32_t parked_value =
        wake_seq_.fetch_or(kParkedBit, std::memory_order_acq_rel) | kParkedBit;
    // (2) 置位后终扫：看到任务则直接带回，不驻停。
    size_t popped = queue_->pop_batch(batch, batch_size);
    if (popped > 0) {
        wake_seq_.fetch_and(~kParkedBit, std::memory_order_release);
        return popped;
    }
    // (3) futex 驻停（32 位 wake_seq_ 走 libstdc++ futex 直达路径）。
    // 停滞生产者的恢复（scan-ahead 取消未决预留）不经过 push 成功路径，
    // 由 set_before_publish_hook 的 trampoline 在进入停滞窗口前定向唤醒。
    wake_seq_.wait(parked_value, std::memory_order_acquire);
    wake_seq_.fetch_and(~kParkedBit, std::memory_order_release);
    return 0;
}

void LockFreeTaskExecutor::wake_worker_if_parking() {
    // 无条件 RMW（全序、排空 store buffer——条件 load 检查会被 StoreLoad
    // 重排打穿）。仅当返回值带驻停位才发 futex notify：忙碌路径零 syscall。
    const uint32_t previous = wake_seq_.fetch_add(2, std::memory_order_release);
    if (previous & kParkedBit) {
        wake_seq_.notify_one();
    }
}

} // namespace kairo
