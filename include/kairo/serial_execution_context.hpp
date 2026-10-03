#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <map>
#include <optional>
#include <set>
#include <thread>
#include <utility>

namespace kairo {

/** A small FIFO, single-threaded context usable with Executor::submit_on.
 *
 * 生命周期契约（CR-012）：公开对象析构只 detach 共享状态，不销毁它。
 * Executor::submit_on* 的派发/守卫闭包持有 shared_ptr<Shared>，公开对象
 * 先行析构后它们仍可安全调用——post_reserved 返回 false（facade 据此按
 * ExecutorStopping 结算业务 future）、abandon 为幂等空操作。共享状态在
 * 最后一个持有者释放后销毁，不存在对已析构对象的触达。
 */
class SerialExecutionContext {
public:
    using Ticket = uint64_t;

    class Shared {
    public:
        Shared() : worker_([this] { run(); }) {}

        std::optional<Ticket> reserve() {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return std::nullopt;
            const Ticket ticket = next_ticket_++;
            reserved_.insert(ticket);
            return ticket;
        }

        bool post_reserved(Ticket ticket, std::function<void()> task) {
            if (!task) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (detached_) return false;
                if (reserved_.find(ticket) == reserved_.end()) return false;
                abandon_locked(ticket);
                cv_.notify_one();
                return false;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (detached_) return false;
                if (stopping_) {
                    // shutdown() normally clears all reservations while holding
                    // this mutex.  Preserve already-published callbacks if a
                    // late publisher arrives after that point.
                    if (reserved_.find(ticket) != reserved_.end()) {
                        abandon_locked(ticket);
                    }
                    return false;
                }
                if (reserved_.erase(ticket) == 0) {
                    // A ticket can only be published once.  In particular, do
                    // not erase the callback accepted by an earlier publisher.
                    return false;
                }
                pending_.emplace(ticket, std::move(task));
                release_ready_locked();
            }
            cv_.notify_one();
            return true;
        }

        void abandon(Ticket ticket) noexcept {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (detached_) return;
                abandon_locked(ticket);
            }
            cv_.notify_one();
        }

        void shutdown() noexcept {
            bool should_join = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_) {
                    should_join = true;
                } else {
                    stopping_ = true;
                    // Reservations belong to facade wrappers that may still be
                    // waiting for a worker to publish their callback.  Skipping
                    // them here prevents one such wrapper from blocking later
                    // already-published work during shutdown.
                    for (const auto ticket : reserved_) {
                        skipped_.insert(ticket);
                    }
                    reserved_.clear();
                    release_ready_locked();
                }
            }
            if (!should_join) cv_.notify_all();
            if (worker_.joinable() && std::this_thread::get_id() != worker_.get_id())
                worker_.join();
        }

        bool is_stopped() const noexcept {
            std::lock_guard<std::mutex> lock(mutex_);
            return stopping_;
        }

        bool detached() const noexcept {
            std::lock_guard<std::mutex> lock(mutex_);
            return detached_;
        }

    private:
        friend class SerialExecutionContext;

        // 公开对象析构时调用：先走常规 shutdown（结算预留、join worker），
        // 再置 detached。此后 facade 闭包的 post_reserved/abandon 全部短路，
        // 不会再触达任何会随公开对象语义消失的状态；共享状态本体由
        // shared_ptr 保活到所有闭包释放。
        void detach() noexcept {
            shutdown();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                detached_ = true;
            }
            cv_.notify_all();
        }

        void run() noexcept {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                    if (stopping_ && queue_.empty()) return;
                    task = std::move(queue_.front());
                    queue_.pop();
                }
                try { task(); } catch (...) { /* submit_on owns exception delivery */ }
            }
        }

        void abandon_locked(Ticket ticket) noexcept {
            reserved_.erase(ticket);
            pending_.erase(ticket);
            // Ignore duplicate/late abandonment after the ticket has already
            // crossed the ready watermark.
            if (ticket >= next_ready_ && ticket < next_ticket_) {
                skipped_.insert(ticket);
            }
            release_ready_locked();
        }

        void release_ready_locked() {
            for (;;) {
                auto skipped = skipped_.find(next_ready_);
                if (skipped != skipped_.end()) {
                    skipped_.erase(skipped);
                    ++next_ready_;
                    continue;
                }
                auto pending = pending_.find(next_ready_);
                if (pending == pending_.end()) return;
                queue_.push(std::move(pending->second));
                pending_.erase(pending);
                ++next_ready_;
            }
        }

        mutable std::mutex mutex_;
        std::condition_variable cv_;
        std::queue<std::function<void()>> queue_;
        std::map<Ticket, std::function<void()>> pending_;
        std::set<Ticket> reserved_;
        std::set<Ticket> skipped_;
        Ticket next_ticket_ = 0;
        Ticket next_ready_ = 0;
        bool stopping_ = false;
        bool detached_ = false;
        std::thread worker_;
    };

    SerialExecutionContext() : state_(std::make_shared<Shared>()) {}
    ~SerialExecutionContext() {
        if (state_) state_->detach();
    }

    SerialExecutionContext(const SerialExecutionContext&) = delete;
    SerialExecutionContext& operator=(const SerialExecutionContext&) = delete;
    // 可移动：移动后原对象不拥有共享状态（析构不再 shutdown，语义归新对象）。
    SerialExecutionContext(SerialExecutionContext&& other) noexcept
        : state_(std::move(other.state_)) {}
    SerialExecutionContext& operator=(SerialExecutionContext&& other) noexcept {
        if (this != &other) {
            if (state_) state_->detach();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    bool post(std::function<void()> task) {
        if (!state_) return false;
        auto ticket = state_->reserve();
        return ticket && state_->post_reserved(*ticket, std::move(task));
    }

    std::optional<Ticket> reserve() {
        if (!state_) return std::nullopt;
        return state_->reserve();
    }

    bool post_reserved(Ticket ticket, std::function<void()> task) {
        if (!state_) return false;
        return state_->post_reserved(ticket, std::move(task));
    }

    void abandon(Ticket ticket) noexcept {
        if (!state_) return;
        state_->abandon(ticket);
    }

    void shutdown() noexcept {
        if (!state_) return;
        state_->shutdown();
    }

    bool is_stopped() const noexcept {
        if (!state_) return true;
        return state_->is_stopped();
    }

    // Executor::submit_on* 专用：取得生命周期安全句柄。公开对象在其后任何
    // 时刻析构均不影响闭包对共享状态的合法访问（见类注释）。
    const std::shared_ptr<Shared>& shared_state() const noexcept { return state_; }

private:
    std::shared_ptr<Shared> state_;
};

} // namespace kairo
