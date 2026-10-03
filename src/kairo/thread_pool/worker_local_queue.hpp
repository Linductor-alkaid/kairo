#pragma once

#include "../task/task.hpp"
#include <vector>
#include <mutex>
#include <atomic>
#include <cstddef>
#include <memory>

namespace kairo {

/**
 * @brief 工作线程本地队列
 * 
 * 每个工作线程维护一个本地任务队列。
 * 支持本地 push/pop 和工作窃取（steal）。
 * 
 * 设计要点：
 * - 使用双端队列（deque），前端用于本地 pop，后端用于 steal
 * - 使用互斥锁保护队列操作
 * - 支持线程安全的 push、pop 和 steal
 */
class WorkerLocalQueue {
public:
    /// capacity == 0 时使用的回退槽位数（CR-117：阈值计算必须与实际容量一致，
    /// ThreadPoolResizer 等消费方一律经 effective_queue_capacity() 取值）。
    static constexpr size_t kDefaultCapacitySlots = 100;

    /**
     * @brief 构造函数
     *
     * @param capacity 队列容量（默认 0）。当 capacity == 0 时,内部会回退到一个
     *                 自适应最小值 (当前实现为 100 槽, 详见 worker_local_queue.cpp)。
     *                 当 capacity > 0 时,严格按该值分配固定大小环形缓冲区。
     *                 注: 0 并不表示"无限制",而是"使用默认最小容量"的 sentinel。
     *                 P-260623-001: 此前的注释曾暗示 0 = 无限制,与实际行为不符,现已明确化。
     */
    explicit WorkerLocalQueue(size_t capacity = 0);

    /**
     * @brief 析构函数
     */
    ~WorkerLocalQueue() = default;

    // 禁止拷贝和移动（因为包含不可移动的 mutex）
    WorkerLocalQueue(const WorkerLocalQueue&) = delete;
    WorkerLocalQueue& operator=(const WorkerLocalQueue&) = delete;
    WorkerLocalQueue(WorkerLocalQueue&&) = delete;
    WorkerLocalQueue& operator=(WorkerLocalQueue&&) = delete;

    /**
     * @brief 推入任务（由 TaskDispatcher 调用）
     * 
     * @param task 任务对象
     * @return 成功返回 true，队列满时返回 false
     */
    bool push(const Task& task);
    
    /**
     * @brief 推入任务（移动版本）
     * 
     * @param task 任务对象（会被移动）
     * @return 成功返回 true，队列满时返回 false
     */
    bool push(Task&& task);

    /**
     * @brief 批量推入任务（一次加锁）
     *
     * @param tasks 任务数组
     * @param n 任务数量
     * @return 实际推入的数量（队列满时停止）
     */
    size_t push_batch(const Task* tasks, size_t n);

    /**
     * @brief 批量推入任务（移动版本，一次加锁）
     *
     * PA-4：dispatch_batch 经此入口将任务字段级移动进环形缓冲区，
     * 消除每个任务 2 个 std::function + string 的复制。
     *
     * @param tasks 任务所有权数组（字段被移动消耗）
     * @param n 任务数量
     * @return 实际推入的数量（队列满时停止，剩余元素仍由调用方持有）
     */
    size_t push_batch_move(std::unique_ptr<Task>* tasks, size_t n);

    /**
     * @brief 弹出任务（由工作线程调用，从队列前端弹出）
     * 
     * @param task 用于接收任务的引用
     * @return 成功返回 true，队列空时返回 false
     */
    bool pop(Task& task);

    /**
     * @brief 窃取任务（由其他线程调用，从队列后端弹出）
     * 
     * @param task 用于接收任务的引用
     * @return 成功返回 true，队列空时返回 false
     */
    bool steal(Task& task);

    /**
     * @brief 获取队列大小
     * 
     * @return 队列中任务数量
     */
    size_t size() const;

    /**
     * @brief 检查队列是否为空
     * 
     * @return 队列为空返回 true，否则返回 false
     */
    bool empty() const;

    /**
     * @brief 清空队列
     */
    void clear();

private:
    // 使用包装器存储 Task，手动实现拷贝构造避免问题
    struct TaskWrapper {
        Task task;
        TaskWrapper() = default;
        TaskWrapper(const Task& t) {
            task.task_id = t.task_id;
            task.priority = t.priority;
            task.function = t.function;
            task.on_timeout = t.on_timeout;
            task.submit_time_ns = t.submit_time_ns;
            task.timeout_ms = t.timeout_ms;
            task.dependencies = t.dependencies;
            task.cancelled.store(t.cancelled.load(std::memory_order_acquire), std::memory_order_release);
        }
        // 手动实现拷贝构造
        TaskWrapper(const TaskWrapper& other) {
            task.task_id = other.task.task_id;
            task.priority = other.task.priority;
            task.function = other.task.function;
            task.on_timeout = other.task.on_timeout;
            task.submit_time_ns = other.task.submit_time_ns;
            task.timeout_ms = other.task.timeout_ms;
            task.dependencies = other.task.dependencies;
            task.cancelled.store(other.task.cancelled.load(std::memory_order_acquire), std::memory_order_release);
        }
        // 手动实现拷贝赋值
        TaskWrapper& operator=(const TaskWrapper& other) {
            if (this != &other) {
                task.task_id = other.task.task_id;
                task.priority = other.task.priority;
                task.function = other.task.function;
                task.on_timeout = other.task.on_timeout;
                task.submit_time_ns = other.task.submit_time_ns;
                task.timeout_ms = other.task.timeout_ms;
                task.dependencies = other.task.dependencies;
                task.cancelled.store(other.task.cancelled.load(std::memory_order_acquire), std::memory_order_release);
            }
            return *this;
        }
    };
    
    mutable std::mutex mutex_;           // 保护队列的互斥锁
    std::vector<TaskWrapper> queue_;     // 任务队列（固定大小，使用环形缓冲区）
    size_t front_index_{0};              // 前端索引（用于 pop）
    size_t back_index_{0};               // 后端索引（用于 push/steal）
    size_t capacity_;                    // 队列容量（0 表示无限制，实际使用固定大小）
    std::atomic<size_t> size_;          // 队列大小（原子变量，用于快速查询）
};

} // namespace kairo
