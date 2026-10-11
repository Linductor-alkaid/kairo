#include "priority_scheduler.hpp"
#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>

namespace kairo {

namespace {
constexpr int kQueueCount = 4;  // LOW, NORMAL, HIGH, CRITICAL
constexpr int kTopLevel = kQueueCount - 1;
}  // namespace

void PriorityScheduler::set_aging_policy(bool enabled, int64_t boost_interval_ns) noexcept {
    aging_interval_ns_.store(
        boost_interval_ns > 0 ? boost_interval_ns : 1, std::memory_order_relaxed);
    aging_enabled_.store(enabled, std::memory_order_release);
}

std::unique_ptr<Task> PriorityScheduler::pop_best_with_aging() {
    // 固定顺序锁全部队列（与 size/clear 的 scoped_lock 同序，无死锁面），
    // 锁内完成堆顶快照、有效优先级比较与弹出，避免 peek/pop 两阶段竞态。
    std::scoped_lock lock(critical_mutex_, high_mutex_, normal_mutex_, low_mutex_);
    // 下标即优先级等级：0=LOW … 3=CRITICAL（与 queue_level 编码一致）。
    TaskQueue* queues[kQueueCount] = {
        &low_queue_, &normal_queue_, &high_queue_, &critical_queue_};

    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t interval = aging_interval_ns_.load(std::memory_order_relaxed);

    int best_level = -1;
    // 最优键（大者胜）：(有效优先级, EDF 次序, submit FIFO)。
    // deadline 次序复用 Task::operator< 的约定：有 deadline 的按 deadline
    // 升序排在无 deadline 之前——编码为"小者优"的 rank。
    std::tuple<int, int64_t, int64_t> best_key{};
    for (int level = 0; level < kQueueCount; ++level) {
        auto& queue = *queues[level];
        if (queue.empty() || !queue.front()) {
            continue;
        }
        const Task& top = *queue.front();
        int effective = level;
        if (interval > 0) {
            const int64_t waited = now_ns - top.submit_time_ns;
            if (waited > 0) {
                const int64_t boost = waited / interval;
                effective = static_cast<int>(
                    std::min<int64_t>(kTopLevel, level + boost));
            }
        }
        // EDF rank：有 deadline 的按 -deadline（小者优），无 deadline 的排后。
        // 键整体为"大者胜"，故 submit FIFO 取负（早提交者大），无 deadline
        // 的哨兵取 int64_min（任何有效 deadline 的 -deadline 均大于它）。
        const int64_t deadline_rank =
            top.deadline_ns != 0 ? -top.deadline_ns
                                 : std::numeric_limits<int64_t>::min();
        std::tuple<int, int64_t, int64_t> key{effective, deadline_rank,
                                              -top.submit_time_ns};
        if (best_level < 0 || key > best_key) {
            best_level = level;
            best_key = key;
        }
    }
    if (best_level < 0) {
        return nullptr;
    }
    auto& queue = *queues[best_level];
    std::pop_heap(queue.begin(), queue.end(), TaskPtrCompare{});
    std::unique_ptr<Task> ptr = std::move(queue.back());
    queue.pop_back();
    return ptr;
}

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
    // CR-024：aging 开启时走全队列有效优先级决策；关闭时保持原短路
    // 逐队扫描（逐位不变）。
    if (aging_enabled_.load(std::memory_order_acquire)) {
        std::unique_ptr<Task> aged = pop_best_with_aging();
        if (!aged) {
            return false;
        }
        move_task_out(aged, task);
        return true;
    }

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

    // CR-024：aging 开启时逐个走全队列决策（批内保持与单 dequeue 相同的
    // 有效优先级全局序）；关闭时保持原逐队排空（逐位不变）。
    if (aging_enabled_.load(std::memory_order_acquire)) {
        size_t count = 0;
        while (count < max_tasks) {
            std::unique_ptr<Task> aged = pop_best_with_aging();
            if (!aged) {
                break;
            }
            out[count] = std::move(aged);
            ++count;
        }
        return count;
    }

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
