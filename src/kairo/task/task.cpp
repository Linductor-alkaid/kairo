#include "task.hpp"
#include <atomic>

namespace kairo {

namespace {
    // 全局任务ID计数器，使用原子操作保证线程安全
    static std::atomic<uint64_t> g_task_id_counter{1};
}

/**
 * @brief Task 比较操作符（用于优先级队列）
 * 
 * 优先级高的任务优先（CRITICAL > HIGH > NORMAL > LOW）
 * 同优先级时，提交时间早的任务优先
 */
bool operator<(const Task& lhs, const Task& rhs) {
    // 优先级高的优先（数值大的优先）
    if (lhs.priority != rhs.priority) {
        return static_cast<int>(lhs.priority) < static_cast<int>(rhs.priority);
    }
    // 同优先级，提交时间早的优先（submit_time_ns 小的优先）
    return lhs.submit_time_ns > rhs.submit_time_ns;
}

/**
 * @brief Task 大于比较操作符
 */
bool operator>(const Task& lhs, const Task& rhs) {
    return rhs < lhs;
}

/**
 * @brief 创建任务ID（辅助函数）
 * 
 * 使用原子计数器生成唯一的任务ID，性能优于基于时间戳的实现
 */
std::string generate_task_id() {
    uint64_t id = g_task_id_counter.fetch_add(1, std::memory_order_relaxed);
    return "task_" + std::to_string(id);
}

/**
 * @brief 检查任务是否已取消
 */
bool is_task_cancelled(const Task& task) {
    return task.cancelled.load(std::memory_order_acquire);
}

/**
 * @brief 取消任务
 */
void cancel_task(Task& task) {
    task.cancelled.store(true, std::memory_order_release);
}

} // namespace kairo
