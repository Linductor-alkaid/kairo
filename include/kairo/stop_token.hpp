#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

#if defined(__ANDROID__)
#include <version>
#endif

// KAIRO_STOP_TOKEN_FORCE_FALLBACK 仅用于测试：在桌面编译时强制实例化
// Android fallback 实现，验证 StopSource / StopToken / JThread 的运行时语义。
#if defined(KAIRO_STOP_TOKEN_FORCE_FALLBACK) || \
    (defined(__ANDROID__) && !defined(__cpp_lib_jthread))

namespace kairo {

namespace detail {
class StopState {
public:
    std::atomic<bool> stop_requested{false};
};
class JThread;
} // namespace detail

/**
 * @brief Android 下替代 std::stop_token 的最小停止请求令牌。
 *
 * 只表达“已请求停止”这一状态；不会中断任何底层阻塞调用。桌面平台使用
 * std::stop_token 别名，不经过此实现。
 */
class StopToken {
public:
    StopToken() noexcept = default;

    [[nodiscard]] bool stop_requested() const noexcept {
        return state_ != nullptr &&
               state_->stop_requested.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool stop_possible() const noexcept {
        return state_ != nullptr;
    }

    friend bool operator==(const StopToken& lhs, const StopToken& rhs) noexcept {
        return lhs.state_ == rhs.state_;
    }

    friend bool operator!=(const StopToken& lhs, const StopToken& rhs) noexcept {
        return !(lhs == rhs);
    }

private:
    friend class StopSource;
    friend class detail::JThread;

    explicit StopToken(std::shared_ptr<detail::StopState> state) noexcept
        : state_(std::move(state)) {}

    std::shared_ptr<detail::StopState> state_;
};

/**
 * @brief Android 下替代 std::stop_source 的最小停止请求源。
 */
class StopSource {
public:
    StopSource() : state_(std::make_shared<detail::StopState>()) {}

    [[nodiscard]] StopToken get_token() const noexcept {
        return StopToken(state_);
    }

    [[nodiscard]] bool stop_requested() const noexcept {
        return state_->stop_requested.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool stop_possible() const noexcept { return true; }

    bool request_stop() noexcept {
        bool expected = false;
        return state_->stop_requested.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
    }

private:
    std::shared_ptr<detail::StopState> state_;
};

namespace detail {

class JThread {
public:
    JThread() noexcept = default;

    template <typename F, typename... Args>
    explicit JThread(F&& function, Args&&... args)
        : source_() {
        const StopToken token = source_.get_token();
        thread_ = std::thread(
            [token,
             function = std::forward<F>(function),
             ... args = std::forward<Args>(args)]() mutable {
                using Function = decltype(function);
                if constexpr (std::is_invocable_v<Function&, StopToken,
                                                  decltype(args)...>) {
                    std::invoke(std::move(function), token, std::move(args)...);
                } else {
                    std::invoke(std::move(function), std::move(args)...);
                }
            });
    }

    JThread(const JThread&) = delete;
    JThread& operator=(const JThread&) = delete;

    JThread(JThread&&) noexcept = default;
    JThread& operator=(JThread&& other) noexcept {
        if (this != &other) {
            if (joinable()) {
                request_stop();
                join();
            }
            source_ = std::move(other.source_);
            thread_ = std::move(other.thread_);
        }
        return *this;
    }

    ~JThread() {
        if (joinable()) {
            request_stop();
            join();
        }
    }

    [[nodiscard]] bool joinable() const noexcept { return thread_.joinable(); }
    void join() { thread_.join(); }
    void detach() { thread_.detach(); }
    [[nodiscard]] std::thread::id get_id() const noexcept { return thread_.get_id(); }

    [[nodiscard]] StopSource get_stop_source() noexcept { return source_; }
    [[nodiscard]] StopToken get_stop_token() const noexcept { return source_.get_token(); }
    bool request_stop() noexcept { return source_.request_stop(); }

private:
    StopSource source_;
    std::thread thread_;
};

} // namespace detail

} // namespace kairo

#else

#include <stop_token>
#include <thread>

namespace kairo {

using StopToken = std::stop_token;
using StopSource = std::stop_source;

namespace detail {
using JThread = std::jthread;
} // namespace detail

} // namespace kairo

#endif

namespace kairo {

namespace detail {

/**
 * @brief 任务执行期间安装当前线程停止令牌的 RAII 守卫（嵌套安全：
 * 析构恢复前值；inline thread_local 保证跨编译单元同一实例）。
 */
class ScopedStopToken {
public:
    explicit ScopedStopToken(const StopToken& token) noexcept
        : previous_(exchange_current(token)) {}

    ScopedStopToken(const ScopedStopToken&) = delete;
    ScopedStopToken& operator=(const ScopedStopToken&) = delete;

    ~ScopedStopToken() { exchange_current(previous_); }

    static StopToken& current_slot() noexcept { return current_; }

private:
    inline static thread_local StopToken current_;
    StopToken previous_;

    static StopToken exchange_current(const StopToken& token) noexcept {
        StopToken previous = current_;
        current_ = token;
        return previous;
    }
};

} // namespace detail

/**
 * @brief 当前任务执行线程上的协作停止令牌（0.7.1 §3.2 CancelIfLate）。
 *
 * 任务声明 `DeadlinePolicy::CancelIfLate` 且错过 deadline 时，facade 在
 * 调用用户代码前安装该任务的停止令牌；用户代码经
 * `kairo::get_stop_token()` 轮询停止请求并自行退出（协作式，
 * 无抢占）。非任务上下文、或任务未声明 CancelIfLate/未错过时，返回的
 * 空令牌 `stop_possible() == false` 且 `stop_requested()` 恒为 false。
 */
inline StopToken get_stop_token() noexcept {
    return detail::ScopedStopToken::current_slot();
}

} // namespace kairo
