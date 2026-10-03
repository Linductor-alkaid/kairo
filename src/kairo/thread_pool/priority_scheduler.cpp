#include "priority_scheduler.hpp"
#include <algorithm>
#include <memory>
#include <mutex>
#include <utility>

namespace kairo {

void PriorityScheduler::move_task_out(const std::unique_ptr<Task>& src, Task& out) {
    if (!src) return;
    // 出队即消费：const unique_ptr 指向的 Task 此后不再被读取，
    // 字段级移出后仅剩可析构空壳。
    move_task_fields(out, std::move(*src));
}

void PriorityScheduler::enqueue(const Task& task) {
    // 创建unique_ptr<Task>（复制字段）
    auto task_ptr = std::make_unique<Task>();
    copy_task_fields(*task_ptr, task);

    TaskPtrCompare cmp;

    switch (task.priority) {
        case TaskPriority::CRITICAL: {
            std::lock_guard<std::mutex> lock(critical_mutex_);
            critical_queue_.push_back(std::move(task_ptr));
            std::push_heap(critical_queue_.begin(), critical_queue_.end(), cmp);
            break;
        }
        case TaskPriority::HIGH: {
            std::lock_guard<std::mutex> lock(high_mutex_);
            high_queue_.push_back(std::move(task_ptr));
            std::push_heap(high_queue_.begin(), high_queue_.end(), cmp);
            break;
        }
        case TaskPriority::NORMAL: {
            std::lock_guard<std::mutex> lock(normal_mutex_);
            normal_queue_.push_back(std::move(task_ptr));
            std::push_heap(normal_queue_.begin(), normal_queue_.end(), cmp);
            break;
        }
        case TaskPriority::LOW: {
            std::lock_guard<std::mutex> lock(low_mutex_);
            low_queue_.push_back(std::move(task_ptr));
            std::push_heap(low_queue_.begin(), low_queue_.end(), cmp);
            break;
        }
        default:
            break;
    }
}

void PriorityScheduler::enqueue(Task&& task) {
    // PA-4: 移动版本 —— 字段级移动进 unique_ptr<Task>，无 function/string 复制。
    // priority 标量在 move_task_fields 移动后保持原值，可安全用于选队列。
    TaskPriority priority = task.priority;
    auto task_ptr = std::make_unique<Task>();
    move_task_fields(*task_ptr, std::move(task));

    TaskPtrCompare cmp;

    switch (priority) {
        case TaskPriority::CRITICAL: {
            std::lock_guard<std::mutex> lock(critical_mutex_);
            critical_queue_.push_back(std::move(task_ptr));
            std::push_heap(critical_queue_.begin(), critical_queue_.end(), cmp);
            break;
        }
        case TaskPriority::HIGH: {
            std::lock_guard<std::mutex> lock(high_mutex_);
            high_queue_.push_back(std::move(task_ptr));
            std::push_heap(high_queue_.begin(), high_queue_.end(), cmp);
            break;
        }
        case TaskPriority::NORMAL: {
            std::lock_guard<std::mutex> lock(normal_mutex_);
            normal_queue_.push_back(std::move(task_ptr));
            std::push_heap(normal_queue_.begin(), normal_queue_.end(), cmp);
            break;
        }
        case TaskPriority::LOW: {
            std::lock_guard<std::mutex> lock(low_mutex_);
            low_queue_.push_back(std::move(task_ptr));
            std::push_heap(low_queue_.begin(), low_queue_.end(), cmp);
            break;
        }
        default:
            break;
    }
}

size_t PriorityScheduler::enqueue_batch(std::unique_ptr<Task>* tasks, size_t n) {
    if (!tasks || n == 0) return 0;

    TaskPtrCompare cmp;

    // 每个优先级队列仅加锁一次，直接接管 unique_ptr 所有权（零字段复制）。
    auto push_class = [&](TaskQueue& queue, TaskPriority p) -> size_t {
        size_t count = 0;
        for (size_t i = 0; i < n; ++i) {
            if (!tasks[i] || tasks[i]->priority != p) continue;
            queue.push_back(std::move(tasks[i]));
            std::push_heap(queue.begin(), queue.end(), cmp);
            ++count;
        }
        return count;
    };

    size_t total = 0;
    {
        std::lock_guard<std::mutex> lock(critical_mutex_);
        total += push_class(critical_queue_, TaskPriority::CRITICAL);
    }
    {
        std::lock_guard<std::mutex> lock(high_mutex_);
        total += push_class(high_queue_, TaskPriority::HIGH);
    }
    {
        std::lock_guard<std::mutex> lock(normal_mutex_);
        total += push_class(normal_queue_, TaskPriority::NORMAL);
    }
    {
        std::lock_guard<std::mutex> lock(low_mutex_);
        total += push_class(low_queue_, TaskPriority::LOW);
    }
    return total;
}

bool PriorityScheduler::dequeue(Task& task) {
    TaskPtrCompare cmp;
    std::unique_ptr<Task> task_ptr;

    {
        std::lock_guard<std::mutex> lock(critical_mutex_);
        if (!critical_queue_.empty()) {
            std::pop_heap(critical_queue_.begin(), critical_queue_.end(), cmp);
            task_ptr = std::move(critical_queue_.back());
            critical_queue_.pop_back();
        }
    }
    if (task_ptr) {
        move_task_out(task_ptr, task);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(high_mutex_);
        if (!high_queue_.empty()) {
            std::pop_heap(high_queue_.begin(), high_queue_.end(), cmp);
            task_ptr = std::move(high_queue_.back());
            high_queue_.pop_back();
        }
    }
    if (task_ptr) {
        move_task_out(task_ptr, task);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(normal_mutex_);
        if (!normal_queue_.empty()) {
            std::pop_heap(normal_queue_.begin(), normal_queue_.end(), cmp);
            task_ptr = std::move(normal_queue_.back());
            normal_queue_.pop_back();
        }
    }
    if (task_ptr) {
        move_task_out(task_ptr, task);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(low_mutex_);
        if (!low_queue_.empty()) {
            std::pop_heap(low_queue_.begin(), low_queue_.end(), cmp);
            task_ptr = std::move(low_queue_.back());
            low_queue_.pop_back();
        }
    }
    if (task_ptr) {
        move_task_out(task_ptr, task);
        return true;
    }

    return false;
}

size_t PriorityScheduler::dequeue_batch(std::unique_ptr<Task>* out, size_t max_tasks) {
    if (max_tasks == 0 || !out) return 0;
    TaskPtrCompare cmp;
    size_t count = 0;

    auto drain = [&](TaskQueue& queue, std::mutex& m) {
        std::lock_guard<std::mutex> lock(m);
        while (!queue.empty() && count < max_tasks) {
            std::pop_heap(queue.begin(), queue.end(), cmp);
            std::unique_ptr<Task> ptr = std::move(queue.back());
            queue.pop_back();
            out[count] = std::move(ptr);
            ++count;
        }
    };

    drain(critical_queue_, critical_mutex_);
    drain(high_queue_, high_mutex_);
    drain(normal_queue_, normal_mutex_);
    drain(low_queue_, low_mutex_);

    return count;
}

size_t PriorityScheduler::size() const {
    std::scoped_lock lock(critical_mutex_, high_mutex_, normal_mutex_, low_mutex_);
    return critical_queue_.size() + high_queue_.size() +
           normal_queue_.size() + low_queue_.size();
}

bool PriorityScheduler::empty() const {
    std::scoped_lock lock(critical_mutex_, high_mutex_, normal_mutex_, low_mutex_);
    if (!critical_queue_.empty()) return false;
    if (!high_queue_.empty()) return false;
    if (!normal_queue_.empty()) return false;
    if (!low_queue_.empty()) return false;
    return true;
}

void PriorityScheduler::clear() {
    std::scoped_lock lock(critical_mutex_, high_mutex_, normal_mutex_, low_mutex_);
    critical_queue_.clear();
    high_queue_.clear();
    normal_queue_.clear();
    low_queue_.clear();
}

} // namespace kairo
