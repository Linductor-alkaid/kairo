#pragma once

#include "kairo/types.hpp"
#include <string>
#include <utility>

namespace kairo {

/**
 * @brief 字段级移动 Task（dst <- src）
 *
 * Task 含 std::atomic<bool> 不可聚合移动，提交 -> 调度 -> 分发 -> 本地队列
 * -> 弹出全链路（PA-4）统一经此助手做字段级移动，避免逐跳复制
 * std::function / string / vector。priority 等标量按值拷贝且不清空源，
 * 允许调用方在移动后继续读取这些标量。
 */
inline void move_task_fields(Task& dst, Task&& src) noexcept {
    dst.task_id = std::move(src.task_id);
    dst.priority = src.priority;
    dst.function = std::move(src.function);
    dst.on_timeout = std::move(src.on_timeout);
    dst.submit_time_ns = src.submit_time_ns;
    dst.timeout_ms = src.timeout_ms;
    dst.dependencies = std::move(src.dependencies);
    dst.cancelled.store(src.cancelled.load(std::memory_order_acquire),
                        std::memory_order_release);
    dst.qos = src.qos;
    dst.deadline_ns = src.deadline_ns;
}

/**
 * @brief 字段级拷贝 Task（dst <- src）
 *
 * 同 move_task_fields 的拷贝语义版本，供仍需共享所有权的路径使用。
 */
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
    dst.qos = src.qos;
    dst.deadline_ns = src.deadline_ns;
}

/**
 * @brief Task 比较操作符（用于优先级队列）
 * 
 * 优先级高的任务优先（CRITICAL > HIGH > NORMAL > LOW）
 * 同优先级时，提交时间早的任务优先
 */
bool operator<(const Task& lhs, const Task& rhs);

/**
 * @brief Task 大于比较操作符
 */
bool operator>(const Task& lhs, const Task& rhs);

/**
 * @brief 创建任务ID（辅助函数）
 * 
 * 使用原子计数器生成唯一的任务ID，性能优于基于时间戳的实现
 * 
 * @return 唯一的任务ID字符串
 */
std::string generate_task_id();

/**
 * @brief 检查任务是否已取消
 * 
 * @param task 任务对象
 * @return 如果任务已取消，返回 true
 */
bool is_task_cancelled(const Task& task);

/**
 * @brief 取消任务
 * 
 * @param task 任务对象
 */
void cancel_task(Task& task);

} // namespace kairo
