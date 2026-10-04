#pragma once

#include "load_balancer.hpp"
#include "priority_scheduler.hpp"
#include "../task/task.hpp"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>
#include <shared_mutex>

namespace kairo {

// 前向声明
class PriorityScheduler;

namespace detail {

// 将 src Task 复制到 dst Task，保持两套队列实现共享同一份拷贝语义。
// 之前位于 task_dispatcher.cpp 的匿名命名空间，模板化后需要可见。
// PA-4 后热路径已改用 task.hpp 的 move_task_fields；本助手保留给
// dispatch_task(const Task&) 等仍需拷贝语义的路径。
inline void copy_task_fields(Task& dst, const Task& src) {
    dst.task_id = src.task_id;
    dst.priority = src.priority;
    dst.function = src.function;
    dst.on_timeout = src.on_timeout;
    dst.submit_time_ns = src.submit_time_ns;
    dst.timeout_ms = src.timeout_ms;
    dst.dependencies = src.dependencies;
    dst.cancelled.store(src.cancelled.load(std::memory_order_acquire),
                        std::memory_order_release);
}

}  // namespace detail

/**
 * @brief 任务分发器（模板化以支持多种工作线程本地队列实现）
 *
 * QueueT 必须提供与 WorkerLocalQueue 兼容的接口：
 *   - bool push(const Task&)
 *   - bool push(Task&&)
 *   - size_t push_batch(const Task*, size_t)
 *   - size_t push_batch_move(std::unique_ptr<Task>*, size_t)
 *   - size_t size() const
 *
 * 既支持默认的 WorkerLocalQueue，也支持 USE_LOCKFREE_WORKER_QUEUE 模式下的
 * LockFreeWorkerQueue。
 */
template<typename QueueT>
class TaskDispatcher {
public:
    /**
     * @brief 构造函数
     *
     * @param balancer 负载均衡器引用
     * @param scheduler 优先级调度器引用
     * @param local_queues_slot 工作线程本地队列槽位（指向 ThreadPool 的
     *        unique_ptr 成员的指针；访问必须在 local_queues_mutex_ 保护下
     *        取快照，P-260617-002: dispatch 路径持 shared_lock，与 resize
     *        路径的 unique_lock 配对，防 vector element 重建期间悬空访问。
     *        PA-9: 槽位经裸指针快照读取，替代原先的
     *        std::atomic_load(shared_ptr*) —— libstdc++ 自由函数走全局
     *        自旋锁表，所有 shared_ptr 共享一池，热路径上每次 worker
     *        迭代/派发都串行化）
     * @param local_queues_mutex 保护 local_queues_ 的 shared_mutex 指针
     * @param wake_seq 工作线程驻停代次计数指针（PA-2）。任何让任务变得
     *        可执行的路径（入队/搬运成功）都必须递增并 notify，保证
     *        驻停 worker 的"扫描 -> 驻停"窗口无丢失唤醒。
     * @param worker_scan_covers_all_queues worker 空闲扫描是否能覆盖其他
     *        worker 的本地队列（即工作窃取启用）。true 时本地队列写入后
     *        notify_one 即可（任一被唤醒 worker 都能偷到），false 时必须
     *        notify_all（否则任务可能滞留在未被唤醒的目标 worker 队列）。
     */
    TaskDispatcher(LoadBalancer& balancer,
                   PriorityScheduler& scheduler,
                   const std::unique_ptr<std::vector<QueueT>>* local_queues_slot,
                   std::shared_mutex* local_queues_mutex = nullptr,
                   std::atomic<uint32_t>* wake_seq = nullptr,
                   bool worker_scan_covers_all_queues = true)
        : balancer_(balancer)
        , scheduler_(scheduler)
        , local_queues_slot_(local_queues_slot)
        , local_queues_mutex_(local_queues_mutex)
        , wake_seq_(wake_seq)
        , worker_scan_covers_all_queues_(worker_scan_covers_all_queues) {}

    /**
     * @brief 析构函数
     */
    ~TaskDispatcher() = default;

    // 禁止拷贝和移动
    TaskDispatcher(const TaskDispatcher&) = delete;
    TaskDispatcher& operator=(const TaskDispatcher&) = delete;
    TaskDispatcher(TaskDispatcher&&) = delete;
    TaskDispatcher& operator=(TaskDispatcher&&) = delete;

    /**
     * @brief 分发任务（从调度器获取并分发）
     *
     * 从 PriorityScheduler 获取一个任务，使用 LoadBalancer
     * 选择目标工作线程，将任务分发到该线程的本地队列。
     *
     * @return 成功分发返回 true，调度器为空返回 false
     */
    bool dispatch() {
        // P-260617-002: 持 shared_lock 保护 local_queues_ 访问，与 resize 路径的
        // unique_lock 配对。注意：RAII wrapper 保证所有 return 路径都释放锁。
        // CR-112: optional<shared_lock> 替代 unique_ptr 包装——原实现每次
        // dispatch 一次堆分配，optional 为纯栈上对象。
        std::optional<std::shared_lock<std::shared_mutex>> lq_lock;
        if (local_queues_mutex_) {
            lq_lock.emplace(*local_queues_mutex_);
        }
        std::vector<QueueT>* local_queues = queues_snapshot_locked();

        Task task;

        // 从调度器获取任务（PA-4: 字段级移动出队）
        if (!scheduler_.dequeue(task)) {
            return false;
        }

        // 使用负载均衡器选择目标工作线程
        size_t worker_id = balancer_.select_worker();

        // 检查 worker_id 是否有效
        if (!local_queues || worker_id >= local_queues->size()) {
            // 260610P009: resize 期间 LoadBalancer 可能返回已被移除的 worker_id。
            // 修复前: 直接丢弃,任务从 scheduler 出队后永久丢失(高并发场景下可能频繁发生)。
            // 修复后: 将任务重新 enqueue 回 scheduler,等待下一轮 dispatch。
            // 这样保证 total == completed + failed(无任务丢失)。
            scheduler_.enqueue(std::move(task));
            signal_work(false);
            return false;
        }

        // 分发任务到选定线程的本地队列（PA-4: 移动）
        bool success = (*local_queues)[worker_id].push(std::move(task));

        if (success) {
            // 更新负载信息
            size_t queue_size = (*local_queues)[worker_id].size();
            balancer_.update_load(worker_id, queue_size, 0);
            // PA-1: 单任务分发只唤醒一个 worker（窃取启用时任一被唤醒
            // worker 的穷尽扫描都能拿到该任务；禁用时唤醒全部）
            signal_work(worker_scan_covers_all_queues_);
        } else {
            // P-260623-001: 推送失败时(本地队列满)重新入队 scheduler,
            // 避免任务从 scheduler 出队后既不在本地队列也不在 scheduler 而被永久丢弃。
            // 镜像 dispatch_batch 中的回 enqueue 模式 (dispatch_batch 会回 enqueue 未推送的任务)。
            scheduler_.enqueue(std::move(task));
            signal_work(false);
        }

        return success;
    }

    /**
     * @brief 分发指定任务
     *
     * @param task 要分发的任务
     * @return 成功分发返回 true
     */
    bool dispatch_task(const Task& task) {
        // P-260617-002: 持 shared_lock 保护 local_queues_ 访问
        // CR-112: optional<shared_lock> 替代 unique_ptr 包装——原实现每次
        // dispatch 一次堆分配，optional 为纯栈上对象。
        std::optional<std::shared_lock<std::shared_mutex>> lq_lock;
        if (local_queues_mutex_) {
            lq_lock.emplace(*local_queues_mutex_);
        }
        std::vector<QueueT>* local_queues = queues_snapshot_locked();

        auto enqueue_fallback = [this, &task]() {
            try {
                scheduler_.enqueue(task);
            } catch (...) {
                return false;
            }
            signal_work(false);
            return false;
        };

        // 使用负载均衡器选择目标工作线程
        size_t worker_id = balancer_.select_worker();

        // 检查 worker_id 是否有效
        if (!local_queues || worker_id >= local_queues->size()) {
            // 260610P009 / 260625-007: resize 期间 LoadBalancer 可能返回已越界的
            // worker_id。dispatch_task 不从 scheduler 出队,但调用方已提交 task；
            // fallback 回 scheduler,避免静默丢任务。
            return enqueue_fallback();
        }

        // 分发任务到选定线程的本地队列
        bool success = (*local_queues)[worker_id].push(task);

        if (success) {
            // 更新负载信息
            size_t queue_size = (*local_queues)[worker_id].size();
            balancer_.update_load(worker_id, queue_size, 0);
            signal_work(worker_scan_covers_all_queues_);
        } else {
            // P-260623-001 / 260625-007: 本地队列满时回 enqueue 到 scheduler,
            // 与 dispatch()/dispatch_batch() 的无任务丢失契约保持一致。
            return enqueue_fallback();
        }

        return success;
    }

    /**
     * @brief 批量分发任务
     *
     * 从调度器批量获取任务并分发，减少锁竞争。
     *
     * PA-3: 消除了旧实现每次派发的 4-6 次堆分配——批缓冲、按 worker 分段
     * 缓冲、per-task 指派表与负载更新表全部提升为成员便签（调用方
     * ThreadPool::dispatch_pending_tasks 持 dispatcher_mutex_ 串行化所有
     * 访问，成员复用安全）；worker 分组由 vector<vector> + sort 改为
     * 计数排序（两遍 O(n) 扫描）。分块上限 kDispatchChunk 约束便签
     * 常驻内存，超大批次自动分多块处理。
     *
     * @param max_tasks 最多分发的任务数
     * @return 实际分发的任务数
     */
    size_t dispatch_batch(size_t max_tasks = 10) {
        if (max_tasks == 0) return 0;

        // P-260617-002: 持 shared_lock 保护 local_queues_ 访问整个函数体
        // （含空队列早退路径——snapshot 读取必须在锁内，与 dispatch() 一致）。
        // CR-112: optional<shared_lock> 替代 unique_ptr 包装——原实现每次
        // dispatch 一次堆分配，optional 为纯栈上对象。
        std::optional<std::shared_lock<std::shared_mutex>> lq_lock;
        if (local_queues_mutex_) {
            lq_lock.emplace(*local_queues_mutex_);
        }
        std::vector<QueueT>* local_queues = queues_snapshot_locked();
        if (!local_queues || local_queues->empty()) {
            // PA-2: 早退也必须是"任务已可执行"的唤醒终态（此刻任务在
            // scheduler 中），防止极端窗口（resize 交换队列集合）下
            // 驻停 worker 观察不到新任务。
            signal_work(false);
            return 0;
        }

        const size_t num_workers = local_queues->size();
        size_t total_dispatched = 0;
        bool requeued_any = false;

        while (total_dispatched < max_tasks) {
            const size_t dispatched_before = total_dispatched;
            const size_t chunk =
                std::min<size_t>(max_tasks - total_dispatched, kDispatchChunk);

            if (batch_.size() < chunk) {
                batch_.resize(chunk);
            }
            const size_t n = scheduler_.dequeue_batch(batch_.data(), chunk);
            if (n == 0) break;

            // 第一遍：选定每个任务的目标 worker，统计每 worker 任务数。
            // select_worker() 非确定（round-robin 推进/负载并发更新），
            // 必须单遍记录结果，不能在第二遍重调。
            assign_.resize(n);
            counts_.assign(num_workers, 0);
            for (size_t i = 0; i < n; ++i) {
                size_t worker_id = balancer_.select_worker();
                if (worker_id >= num_workers) worker_id = 0;
                assign_[i] = static_cast<uint32_t>(worker_id);
                ++counts_[worker_id];
            }

            // 第二遍：前缀和分段 + 原地重排成按 worker 连续的区段
            starts_.resize(num_workers);
            uint32_t offset = 0;
            for (size_t w = 0; w < num_workers; ++w) {
                starts_[w] = offset;
                offset += counts_[w];
            }
            cursors_.assign(starts_.begin(), starts_.end());
            if (arranged_.size() < n) {
                arranged_.resize(n);
            }
            for (size_t i = 0; i < n; ++i) {
                arranged_[cursors_[assign_[i]]++] = std::move(batch_[i]);
            }

            load_updates_.clear();
            for (size_t w = 0; w < num_workers; ++w) {
                if (counts_[w] == 0) continue;

                std::unique_ptr<Task>* segment = arranged_.data() + starts_[w];
                const size_t count = counts_[w];
                size_t pushed = (*local_queues)[w].push_batch_move(segment, count);
                total_dispatched += pushed;

                // 将未能推送的任务放回调度器（移动，不复制）
                for (size_t j = pushed; j < count; ++j) {
                    scheduler_.enqueue(std::move(*segment[j]));
                    requeued_any = true;
                }

                load_updates_.emplace_back(w, (*local_queues)[w].size(), 0);
            }

            if (!load_updates_.empty())
                balancer_.update_load_batch(load_updates_);

            if (n < chunk) break;  // 调度器已排空
            if (total_dispatched == dispatched_before) {
                // 本块零推进：出队的任务全部因本地队列满而回灌。继续
                // 循环只会重复 dequeue -> push 失败 -> 回灌 的空转（还
                // 持着 dispatcher_mutex_，满载/大批次下表现为持锁忙循环）。
                // 任务留在 scheduler 中，由消费腾出空间后的下一次 dispatch
                // 或 worker 的直接出队扫描处理。
                break;
            }
        }

        // PA-1/PA-2: 任务位置发生变化的每个终态都要唤醒驻停 worker。
        // 多任务搬运 -> notify_all（批次语义）；单任务 -> 按窃取覆盖
        // 决定（任一被唤醒 worker 的穷尽扫描可偷到即可 one）；只发生
        // 回灌 -> notify_one（任一被唤醒 worker 从 scheduler 出队即可）。
        if (total_dispatched > 1) {
            signal_work(true);
        } else if (total_dispatched == 1) {
            signal_work(worker_scan_covers_all_queues_);
        } else if (requeued_any) {
            signal_work(false);
        }
        return total_dispatched;
    }

    /**
     * @brief 获取待分发任务数量（调度器中的任务数）
     */
    size_t pending_tasks() const {
        return scheduler_.size();
    }

private:
    // PA-3: 单块出队上限，同时约束成员便签的常驻容量
    static constexpr size_t kDispatchChunk = 128;

    // 快照指针的 const 只针对 vector 结构（元素个数/槽位），元素本身的
    // push/pop 由 QueueT 自己的内部同步保护；返回非 const 指针以允许
    // 调用 push/push_batch_move。
    std::vector<QueueT>* queues_snapshot_locked() const {
        return const_cast<std::vector<QueueT>*>(local_queues_slot_->get());
    }

    // PA-2: 让任务变得可执行后唤醒驻停 worker。fetch_add(release) 与
    // worker 侧 wait 的 acquire 读配对：worker 观察到新代次即可看到
    // 此前完成的入队/搬运写入。atomic wait 在阻塞前原子复核值是否
    // 仍等于旧代次，因此 notify 早于 wait 到达也不会丢失。
    void signal_work(bool wake_all) {
        if (!wake_seq_) return;
        wake_seq_->fetch_add(1, std::memory_order_release);
        if (wake_all) {
            wake_seq_->notify_all();
        } else {
            wake_seq_->notify_one();
        }
    }

    LoadBalancer& balancer_;                      // 负载均衡器
    PriorityScheduler& scheduler_;                // 优先级调度器
    const std::unique_ptr<std::vector<QueueT>>* local_queues_slot_;
    std::shared_mutex* local_queues_mutex_;       // P-260617-002: 保护 local_queues_ 的 shared_mutex
    std::atomic<uint32_t>* wake_seq_;             // PA-2: worker 驻停代次计数
    bool worker_scan_covers_all_queues_;          // PA-1: 窃取启用时可 notify_one

    // ---- 以下便签仅在 ThreadPool::dispatch_pending_tasks 持
    // dispatcher_mutex_ 的串行化下访问（PA-3）----
    std::vector<std::unique_ptr<Task>> batch_;       // 出队缓冲
    std::vector<std::unique_ptr<Task>> arranged_;    // 按 worker 分段的待推送缓冲
    std::vector<uint32_t> assign_;                   // 任务 -> worker 指派
    std::vector<uint32_t> counts_;                   // 每 worker 任务数
    std::vector<uint32_t> starts_;                   // 每 worker 段起始偏移
    std::vector<uint32_t> cursors_;                  // 放置游标
    std::vector<std::tuple<size_t, size_t, size_t>> load_updates_;
};

} // namespace kairo
