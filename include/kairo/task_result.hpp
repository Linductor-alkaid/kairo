#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

namespace kairo {

/**
 * @brief 轻量单次分配的任务结果句柄（0.7.3 §5.0，roadmap 堆分配优化阶段二）。
 *
 * submit_v2 的返回载体：std::future 形状的 get/wait 语义 + 无锁快速
 * 路径（任务已完成后 get() 不再加锁）。结算协议与可观测性设计见
 * docs/design/scheduling_runtime.md §16。
 */
template<typename T>
class TaskResult;

namespace detail {

/** @brief get() 路径命中计数（进程级，relaxed 语义；诊断口径）。 */
struct TaskResultGetCounters {
    std::atomic<uint64_t> fast_path{0};  // 无锁取得（首次检查或 spin 内）
    std::atomic<uint64_t> slow_path{0};  // mutex + cv 等待
};

inline TaskResultGetCounters& task_result_get_counters() noexcept {
    static TaskResultGetCounters counters;
    return counters;
}

/**
 * @brief TaskResult 的共享结算状态（与提交上下文同一控制块，单次分配）。
 *
 * 双原子协议：settled（CAS 选举唯一结算者，语义同 facade 各路径的
 * promise_ready；settle_value/settle_exception 内建选举，输家空操作）
 * 与 ready（发布标志：先写值/异常，后 release 存 ready）。分离的
 * 原因：get() 快速路径在 ready==true 时无锁读值，若沿用"先 CAS
 * ready 再写值"形态，消费者会在值写入前观察到 ready。
 * 唤醒经持锁 notify_all，消除丢失唤醒窗（§16.1）。
 */
template<typename T>
struct TaskResultSharedState {
    std::mutex mutex;                  // 仅慢速等待路径
    std::condition_variable cv;
    std::atomic_bool settled{false};   // 结算者选举
    std::atomic_bool ready{false};     // 值/异常发布标志
    std::optional<T> value;            // 胜者写入；retrieve 移出
    std::exception_ptr exception;

    TaskResultSharedState() = default;
    TaskResultSharedState(const TaskResultSharedState&) = delete;
    TaskResultSharedState& operator=(const TaskResultSharedState&) = delete;

    /** @brief CAS 选举唯一结算者；输家直接放弃（幂等安全）。 */
    bool elect_writer() noexcept {
        bool expected = false;
        return settled.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    /**
     * @brief 值结算（内建 CAS 选举：输家空操作，返回是否胜出）。
     *
     * T 的移动构造抛异常时（病态类型）降级为异常结算，不产生
     * "已选举未发布"的永久挂起。发布顺序：值 → ready(release) →
     * 持锁 notify。
     */
    bool settle_value(T&& v) {
        if (!elect_writer()) {
            return false;
        }
        try {
            value.emplace(std::move(v));
        } catch (...) {
            exception = std::current_exception();
        }
        ready.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mutex);
        cv.notify_all();
        return true;
    }

    /** @brief 异常结算（内建 CAS 选举；返回是否胜出）。 */
    bool settle_exception(std::exception_ptr e) {
        if (!elect_writer()) {
            return false;
        }
        exception = std::move(e);
        ready.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mutex);
        cv.notify_all();
        return true;
    }

    /**
     * @brief broken promise 兑现：任务包装从未运行即被销毁时结算
     * future_error(broken_promise)，使 get() 不永久阻塞（与
     * std::promise 析构的 broken_promise 行为对齐）。幂等。
     */
    void break_promise_if_pending() noexcept {
        if (elect_writer()) {
            try {
                exception = std::make_exception_ptr(
                    std::future_error(std::future_errc::broken_promise));
            } catch (...) {
                // 分配失败无法交付异常对象；保持 ready 以解除等待者。
            }
            ready.store(true, std::memory_order_release);
            std::lock_guard<std::mutex> lock(mutex);
            cv.notify_all();
        }
    }

    /** @brief 取值（仅 ready 后调用；值被移出，异常则重抛）。 */
    T retrieve_value() {
        if (exception) {
            std::rethrow_exception(exception);
        }
        return std::move(*value);
    }
};

/** @brief void 特化：无值存储。 */
template<>
struct TaskResultSharedState<void> {
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic_bool settled{false};
    std::atomic_bool ready{false};
    std::exception_ptr exception;

    TaskResultSharedState() = default;
    TaskResultSharedState(const TaskResultSharedState&) = delete;
    TaskResultSharedState& operator=(const TaskResultSharedState&) = delete;

    bool elect_writer() noexcept {
        bool expected = false;
        return settled.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    bool settle_value() {
        if (!elect_writer()) {
            return false;
        }
        ready.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mutex);
        cv.notify_all();
        return true;
    }

    bool settle_exception(std::exception_ptr e) {
        if (!elect_writer()) {
            return false;
        }
        exception = std::move(e);
        ready.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mutex);
        cv.notify_all();
        return true;
    }

    void break_promise_if_pending() noexcept {
        if (elect_writer()) {
            try {
                exception = std::make_exception_ptr(
                    std::future_error(std::future_errc::broken_promise));
            } catch (...) {
            }
            ready.store(true, std::memory_order_release);
            std::lock_guard<std::mutex> lock(mutex);
            cv.notify_all();
        }
    }

    // 非 noexcept：异常终态经 rethrow 交付（noexcept 会把重抛变成
    // terminate，破坏与 submit 的异常语义同构）。
    void retrieve_value() {
        if (exception) {
            std::rethrow_exception(exception);
        }
    }
};

// get() 的三段等待：快速检查 → 有限 spin（各段命中计 fast）→
// mutex + cv（计 slow）。返回时 ready 必为 true。
template<typename State>
void wait_until_ready_and_count(State& state) {
    if (state.ready.load(std::memory_order_acquire)) {
        task_result_get_counters().fast_path.fetch_add(
            1, std::memory_order_relaxed);
        return;
    }
    for (int spin = 0; spin < 100; ++spin) {
        if (state.ready.load(std::memory_order_acquire)) {
            task_result_get_counters().fast_path.fetch_add(
                1, std::memory_order_relaxed);
            return;
        }
        std::this_thread::yield();
    }
    task_result_get_counters().slow_path.fetch_add(
        1, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(state.mutex);
    state.cv.wait(lock, [&state] {
        return state.ready.load(std::memory_order_acquire);
    });
}

}  // namespace detail

/**
 * @brief 轻量任务结果句柄（std::future 形状，单消费）。
 *
 * - get()：等待就绪（快速路径无锁，§16.1）并取值；异常重抛；
 *   取值后本句柄置无效（valid() == false），与 std::future::get()
 *   的单消费契约一致。
 * - wait()/wait_for()/wait_until()：等待就绪；wait_for/until 返回
 *   std::future_status（永不 deferred）。
 * - is_ready()：非阻塞就绪查询（无效句柄返回 false）。
 * - 可移动、不可拷贝；句柄非线程安全，共享状态线程安全。
 */
template<typename T>
class TaskResult {
public:
    TaskResult() noexcept = default;
    TaskResult(TaskResult&&) noexcept = default;
    TaskResult& operator=(TaskResult&&) noexcept = default;
    TaskResult(const TaskResult&) = delete;
    TaskResult& operator=(const TaskResult&) = delete;

    T get() {
        if (!state_) {
            throw std::future_error(std::future_errc::no_state);
        }
        detail::wait_until_ready_and_count(*state_);
        if constexpr (std::is_void_v<T>) {
            state_->retrieve_value();
            state_.reset();
        } else {
            T result = state_->retrieve_value();
            state_.reset();
            return result;
        }
    }

    void wait() const {
        if (!state_) {
            throw std::future_error(std::future_errc::no_state);
        }
        detail::wait_until_ready_and_count(*state_);
    }

    template<typename Rep, typename Period>
    std::future_status wait_for(
        const std::chrono::duration<Rep, Period>& timeout) const {
        return wait_until(std::chrono::steady_clock::now() + timeout);
    }

    template<typename Clock, typename Duration>
    std::future_status wait_until(
        const std::chrono::time_point<Clock, Duration>& deadline) const {
        if (!state_) {
            throw std::future_error(std::future_errc::no_state);
        }
        if (state_->ready.load(std::memory_order_acquire)) {
            return std::future_status::ready;
        }
        std::unique_lock<std::mutex> lock(state_->mutex);
        const bool ready = state_->cv.wait_until(lock, deadline, [this] {
            return state_->ready.load(std::memory_order_acquire);
        });
        return ready ? std::future_status::ready
                     : std::future_status::timeout;
    }

    bool is_ready() const noexcept {
        return state_ && state_->ready.load(std::memory_order_acquire);
    }

    bool valid() const noexcept { return state_ != nullptr; }

private:
    friend class Executor;

    explicit TaskResult(
        std::shared_ptr<detail::TaskResultSharedState<T>> state) noexcept
        : state_(std::move(state)) {}

    std::shared_ptr<detail::TaskResultSharedState<T>> state_;
};

/**
 * @brief CPU/GPU 双路径任务的显式取值适配器（0.7.3 §5.1）。
 *
 * R 非 void 且路由选中 GPU 时必须提供 gpu_extract：在 GPU worker
 * 线程、GPU callable 返回之后调用（与执行器自身 promise 结算点一致：
 * async 配置 = launch 完成点，sync 配置 = 设备同步完成点）。设备搬运
 * 与同步责任在用户侧——需要 stream 完成语义时由 GPU callable 自行
 * 同步；gpu_extract 阻塞会拖住 GPU worker，属显式选择。见
 * docs/design/scheduling_runtime.md §17.1。
 */
template<typename R>
struct ResultAdapter {
    std::function<R()> gpu_extract;
};

}  // namespace kairo
