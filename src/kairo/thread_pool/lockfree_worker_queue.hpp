#pragma once

#include "../task/task.hpp"
#include "../util/lockfree_queue.hpp"
#include <atomic>
#include <utility>
#include <memory>
#include <vector>
#include <cstdint>
#include <exception>
#include <stdexcept>

namespace kairo {

/**
 * @brief 无锁工作线程本地队列
 *
 * 使用 LockFreeQueue (MPMC) + unique_ptr 包装实现高性能 push/pop。
 * Task 包含 std::function 和 atomic，不是 trivially copyable，
 * 因此使用 uintptr_t 存储指针，绕过类型限制。
 *
 * PA-7: 消费互斥已移除。底层队列的 pop 侧改为 CAS 认领（见
 * LockFreeQueue::pop_impl），owner 的 pop() 与其他 worker 的 steal()
 * 可以并发消费且恰好一次；steal 与 pop 同为 FIFO 最老端出队
 * （测试钉死 steal 必须取最老任务，而非 Chase-Lev 的另一端）。
 * 旧的共享 steal_buffer_ 批量预取也一并移除：它需要消费者间互斥，
 * 且其"减少锁获取次数"的唯一收益随互斥的消失而消失。
 * size() 现为队列自身的无锁近似值，不再获取任何锁。
 */
class LockFreeWorkerQueue {
public:
    explicit LockFreeWorkerQueue(size_t capacity = 1024)
        : main_queue_(capacity > 0 ? capacity : 1024) {}

    bool push(const Task& task) {
        auto* task_ptr = new Task();
        copy_task(*task_ptr, task);
        uintptr_t ptr = reinterpret_cast<uintptr_t>(task_ptr);
        if (!main_queue_.push(ptr)) {
            delete task_ptr;
            return false;
        }
        return true;
    }

    bool push(Task&& task) {
        auto* task_ptr = new Task();
        move_task(*task_ptr, std::move(task));
        uintptr_t ptr = reinterpret_cast<uintptr_t>(task_ptr);
        if (!main_queue_.push(ptr)) {
            delete task_ptr;
            return false;
        }
        return true;
    }

    size_t push_batch(const Task* tasks, size_t n) {
        if (!tasks || n == 0) return 0;

        std::vector<uintptr_t> ptrs;
        ptrs.reserve(n);

        for (size_t i = 0; i < n; ++i) {
            Task* task_ptr = nullptr;
            try {
                task_ptr = new Task();
                copy_task(*task_ptr, tasks[i]);
            } catch (...) {
                if (task_ptr) {
                    delete task_ptr;
                }
                for (size_t k = 0; k < ptrs.size(); ++k) {
                    delete reinterpret_cast<Task*>(ptrs[k]);
                }
                ptrs.clear();
                throw;
            }
            ptrs.push_back(reinterpret_cast<uintptr_t>(task_ptr));
        }

        size_t pushed = 0;
        const bool ok = main_queue_.push_batch(ptrs.data(), n, pushed);
        (void)ok;

        // 清理未推入的任务；底层部分成功时也可能返回 true 且 pushed < n。
        for (size_t i = pushed; i < n; ++i) {
            delete reinterpret_cast<Task*>(ptrs[i]);
        }
        return pushed;
    }

    // PA-4: 批量移动版本。直接接管调用方 unique_ptr<Task> 的所有权，
    // 零新分配、零字段复制（本队列内部即按 Task* 存储）。
    // 未推入的元素所有权仍归调用方，由调用方处理。
    size_t push_batch_move(std::unique_ptr<Task>* tasks, size_t n) {
        if (!tasks || n == 0) return 0;

        size_t pushed = 0;
        for (size_t i = 0; i < n; ++i) {
            uintptr_t ptr = reinterpret_cast<uintptr_t>(tasks[i].get());
            if (!main_queue_.push(ptr)) {
                break;
            }
            tasks[i].release();
            ++pushed;
        }
        return pushed;
    }

    bool pop(Task& task) {
        uintptr_t ptr;
        if (!main_queue_.pop(ptr)) {
            return false;
        }
        std::unique_ptr<Task> task_ptr(reinterpret_cast<Task*>(ptr));
        move_task(task, std::move(*task_ptr));
        return true;
    }

    // PA-7: 窃取与 pop 走同一条 CAS 认领出队路径，无锁、FIFO 最老端。
    bool steal(Task& task) {
        return pop(task);
    }

    size_t size() const {
        return main_queue_.size();
    }

    bool empty() const {
        return size() == 0;
    }

    void clear() {
        std::vector<std::unique_ptr<Task>> discarded;

        uintptr_t ptr;
        while (main_queue_.pop(ptr)) {
            discarded.emplace_back(reinterpret_cast<Task*>(ptr));
        }

        for (auto& task : discarded) {
            discard_task(*task);
        }
    }

    /**
     * @brief 析构函数
     */
    ~LockFreeWorkerQueue() {
        clear();
    }

private:
    static void copy_task(Task& dst, const Task& src) {
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

    static void move_task(Task& dst, Task&& src) {
        dst.task_id = std::move(src.task_id);
        dst.priority = src.priority;
        dst.function = std::move(src.function);
        dst.on_timeout = std::move(src.on_timeout);
        dst.submit_time_ns = src.submit_time_ns;
        dst.timeout_ms = src.timeout_ms;
        dst.dependencies = std::move(src.dependencies);
        dst.cancelled.store(src.cancelled.load(std::memory_order_acquire),
                           std::memory_order_release);
    }

    static void discard_task(Task& task) noexcept {
        if (!task.on_timeout) {
            return;
        }

        try {
            task.on_timeout(std::make_exception_ptr(std::runtime_error(
                "Task discarded before execution")));
        } catch (...) {
        }
    }

    util::LockFreeQueue<uintptr_t> main_queue_;
};

} // namespace kairo
