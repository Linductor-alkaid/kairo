#pragma once

#include "kairo/types.hpp"
#include "../task/task.hpp"
#include <vector>
#include <algorithm>
#include <memory>
#include <mutex>
#include <cstddef>

namespace kairo {

/**
 * @brief 优先级调度器
 * 
 * 管理4个优先级队列（CRITICAL, HIGH, NORMAL, LOW），
 * 提供线程安全的enqueue和dequeue接口。
 * 按优先级顺序调度任务，同优先级任务按提交时间（FIFO）调度。
 * 
 * 使用std::vector<std::unique_ptr<Task>> + 堆操作存储任务，
 * 相比shared_ptr减少引用计数开销和控制块内存分配。
 * Task包含std::atomic<bool>不可复制和移动，因此使用unique_ptr管理。
 *
 * @note 语义约定（CR-024，实测确认后文档化）：
 *  - 默认（aging 关闭）：严格优先级，无老化——持续的高优先级负载会让
 *    LOW/NORMAL 任务无限期等待。这是有意设计（实时场景要求高优先级零
 *    干扰）；需要防饿死的场景应在应用层拆分流量、使用独立执行器，或
 *    开启下述可选 aging。
 *  - 已知窗口（NN-05）：worker 会优先弹本地队列再查全局调度器，滞留在
 *    某个本地队列的高优先级任务对全局严格序不可见——洪泛尾段低优先级
 *    任务可能提前至多一个本地队列深度的时间执行（实测 ~100ms 量级）。
 *    aging 不改变该窗口（worker 扫描顺序未变）。
 *  - 可选 aging（CR-024，v0.7.0 M0，默认关闭）：经 set_aging_policy
 *    开启后，dequeue 时按堆顶任务的等待时长计算有效优先级
 *    effective = min(CRITICAL, base + waited / boost_interval)，在全部
 *    非空队列的堆顶中选 (effective, EDF, submit FIFO) 最优者弹出。
 *    队列内部堆序不重排；关闭时行为与逐位不变。开启会改变出队顺序
 *    （这是它的目的），并把 dequeue 的锁面从"短路单队列"变为
 *    "全队列临界区"——仅在需要防饿死时开启。
 */
class PriorityScheduler {
public:
    /**
     * @brief 构造函数
     */
    PriorityScheduler() = default;

    /**
     * @brief 析构函数
     */
    ~PriorityScheduler() = default;

    // 禁止拷贝和移动
    PriorityScheduler(const PriorityScheduler&) = delete;
    PriorityScheduler& operator=(const PriorityScheduler&) = delete;
    PriorityScheduler(PriorityScheduler&&) = delete;
    PriorityScheduler& operator=(PriorityScheduler&&) = delete;

    /**
     * @brief 配置可选的防饿死 aging（CR-024，默认关闭）
     *
     * 关闭（默认）时出队顺序与既有严格优先级语义逐位一致。开启后，
     * 堆顶任务等待每满 boost_interval 提升一级有效优先级（至多到
     * CRITICAL）；有效优先级超过其它队列堆顶基级时可在 dequeue 时
     * 跨级胜出。提升只影响出队选择，不改写任务自身的 priority。
     *
     * @param enabled 是否启用 aging
     * @param boost_interval_ns 每提升一级所需的最短等待（纳秒）；
     *        enabled 且值 <= 0 时按 1ns 处理（等待任务立即逐级到顶）
     */
    void set_aging_policy(bool enabled, int64_t boost_interval_ns) noexcept;

    /**
     * @brief 添加任务到优先级队列
     *
     * 根据任务的优先级将任务放入对应的队列。
     *
     * @param task 任务对象（会被复制为unique_ptr）
     */
    void enqueue(const Task& task);

    /**
     * @brief 添加任务到优先级队列（移动版本）
     *
     * PA-4：提交热路径经此入口将 Task 字段级移动进调度器内部的
     * unique_ptr<Task>，避免 std::function / string / vector 的逐跳复制。
     *
     * @param task 任务对象（字段被移动，priority 等标量仍可读）
     */
    void enqueue(Task&& task);

    /**
     * @brief 批量移动入队（每个出现的优先级队列仅加锁一次）
     *
     * PA-4：批量提交路径经此入口摊销锁获取，直接接管调用方
     * unique_ptr<Task> 的所有权（零字段复制/移动）。tasks 中每个
     * 元素被移走后置空。
     *
     * @param tasks 任务所有权数组（元素被移走消耗）
     * @param n 任务数量
     * @return 实际入队的任务数
     */
    size_t enqueue_batch(std::unique_ptr<Task>* tasks, size_t n);

    /**
     * @brief 从优先级队列获取任务（按优先级顺序）
     * 
     * 按优先级从高到低（CRITICAL -> HIGH -> NORMAL -> LOW）获取任务。
     * 同优先级任务按提交时间（FIFO）获取。
     * 
     * @param task 用于接收任务的引用
     * @return 如果成功获取任务返回true，队列为空返回false
     */
    bool dequeue(Task& task);

    /**
     * @brief 批量从优先级队列获取任务（按优先级顺序）
     *
     * 按优先级从高到低依次从各队列取任务，每个优先级队列仅加锁一次，
     * 最多取出 max_tasks 个任务。PA-4: 任务字段级移动到 out 指向的
     * unique_ptr<Task> 中（Task 含 atomic 不可移动构造，调用方提供
     * 已构造的槽位）。
     *
     * @param out 用于接收任务的缓冲区，写入 out[0..return-1]
     * @param max_tasks 最多取出的任务数
     * @return 实际取出的任务数
     */
    size_t dequeue_batch(std::unique_ptr<Task>* out, size_t max_tasks);

    /**
     * @brief 获取队列总大小
     * 
     * @return 所有优先级队列中的任务总数
     */
    size_t size() const;

    /**
     * @brief 检查队列是否为空
     * 
     * @return 如果所有队列都为空返回true，否则返回false
     */
    bool empty() const;

    /**
     * @brief 清空所有队列
     */
    void clear();

private:
    // unique_ptr比较器：比较Task对象（通过解引用）
    // 使用std::greater确保优先级高的任务在堆顶
    struct TaskPtrCompare {
        bool operator()(const std::unique_ptr<Task>& lhs, const std::unique_ptr<Task>& rhs) const {
            if (!lhs || !rhs) return false;
            // Task::operator< 返回true表示lhs优先级低于rhs
            // 对于std::greater，返回*lhs < *rhs时，rhs（优先级高）会在堆顶
            return *lhs < *rhs;
        }
    };

    // 使用vector存储unique_ptr<Task>，配合堆操作维护优先级顺序
    using TaskQueue = std::vector<std::unique_ptr<Task>>;

    TaskQueue critical_queue_;  // CRITICAL优先级队列
    TaskQueue high_queue_;      // HIGH优先级队列
    TaskQueue normal_queue_;    // NORMAL优先级队列
    TaskQueue low_queue_;       // LOW优先级队列

    // 每队列独立锁（细粒度锁，减少 enqueue/dequeue 竞争）
    mutable std::mutex critical_mutex_;
    mutable std::mutex high_mutex_;
    mutable std::mutex normal_mutex_;
    mutable std::mutex low_mutex_;

    // CR-024 可选 aging：atomic 保证 initialize 配置与 dequeue 读取之间
    // 无数据竞争（configure 发生在 worker 启动前，成本是一次 relaxed load）。
    std::atomic<bool> aging_enabled_{false};
    std::atomic<int64_t> aging_interval_ns_{100'000'000};  // 默认 100ms/级

    /// aging 开启路径：锁全部队列，选 (有效优先级, EDF, FIFO) 最优堆顶弹出。
    std::unique_ptr<Task> pop_best_with_aging();

    /** 从 unique_ptr<Task> 移动出到 Task&，供 dequeue/dequeue_batch 复用（PA-4） */
    static void move_task_out(const std::unique_ptr<Task>& src, Task& out);
};

} // namespace kairo
