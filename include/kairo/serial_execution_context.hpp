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

    class Shared : public std::enable_shared_from_this<Shared> {
    public:
        // CR-057（复验修正）：构造私有、经 create() 组装，worker 线程
        // 按值持有真实控制块的 shared_ptr；run() 内部另有
        // shared_from_this() 自持引用贯穿循环（见 run 注释）。
        // ~Shared 只可能在 run() 返回后于 worker 栈上发生（由析构体内
        // 的自析构防护 detach 自线程句柄）。
        static std::shared_ptr<Shared> create() {
            std::shared_ptr<Shared> self = std::shared_ptr<Shared>(new Shared());
            self->worker_ = std::thread([self] { self->run(); });
            return self;
        }

        ~Shared() {
            // CR-057：worker 线程内自析构防护。任务闭包持有本 Shared 的
            // 最后一个引用、且任务正在 worker 上执行时，闭包析构触发本
            // 析构——worker_ 仍 joinable 且属于当前线程，放任成员析构会
            // 在 ~thread() 中 std::terminate。此时刻引用计数已为 1，
            // 不可能有并发 shutdown()（其需要另一份引用），同锁内判定
            // 并移走自线程句柄 detach 是安全的；非自线程场景正常路径
            // 已由 shutdown() join 完毕（joinable 为 false）。
            std::thread self;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (worker_.joinable() &&
                    worker_.get_id() == std::this_thread::get_id()) {
                    self = std::move(worker_);
                }
            }
            if (self.joinable()) {
                self.detach();
            }
        }

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
            bool should_join_worker = false;
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
                // CR-057：join 判定移入锁内——run() 的自退出路径会在同一把
                // 锁下把自己的线程对象移走并 detach，锁外的 joinable() 读
                // 与之构成数据竞争。
                should_join_worker = worker_.joinable() &&
                                     std::this_thread::get_id() != worker_.get_id();
            }
            if (!should_join) cv_.notify_all();
            if (should_join_worker) worker_.join();
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

        Shared() = default;

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
            // CR-057：自持引用贯穿循环。任务闭包是本循环的局部变量，
            // 其析构（发生在循环体内）可能释放最后一外部引用——无自持
            // 引用时对象会在 run() 栈帧活跃期间被析构，后续循环触碰已
            // 析构成员（UAF）。shared_from_this() 在 create() 采纳后
            // 必然有效（线程函数持引用在先）。
            const auto self_keep_alive = shared_from_this();
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

    SerialExecutionContext() : state_(Shared::create()) {}
    ~SerialExecutionContext() {
        if (state_) state_->detach();
    }

    /**
     * @brief 强制"要么 publish 要么 abandon"的预留句柄（0.7.2 §4.3，CR-057）。
     *
     * 裸 reserve() 的风险：预留一个 ticket 后既不 post_reserved 也不
     * abandon，FIFO 水位（next_ready_）永久停在该 ticket——整个上下文
     * 的后续 post 全部卡死。Reservation 以析构兜底：未显式 publish 且
     * 未 release 时自动 abandon（幂等；detached 状态下 abandon 本就是
     * 空操作），水位不可能被单次预留永久卡住。
     *
     * 典型用法：
     * @code
     * auto res = context.reserve_guarded();
     * if (!res) return;  // 已停止
     * if (!try_prepare()) return;  // 析构自动 abandon，不阻塞水位
     * res->publish(task);          // 显式发布（此后析构不再 abandon）
     * @endcode
     */
    class Reservation {
    public:
        Reservation() = default;
        Reservation(Reservation&& other) noexcept
            : state_(std::move(other.state_)),
              ticket_(std::exchange(other.ticket_, std::nullopt)) {}
        Reservation& operator=(Reservation&& other) noexcept {
            if (this != &other) {
                release_unpublished();
                state_ = std::move(other.state_);
                ticket_ = std::exchange(other.ticket_, std::nullopt);
            }
            return *this;
        }
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;

        ~Reservation() {
            release_unpublished();
        }

        /** @brief 关联的 ticket（未持有返回 0；ticket 0 由 reserve 产出，
         *  调用方应先经 operator bool 判持）。 */
        Ticket ticket() const noexcept { return ticket_.value_or(0); }
        explicit operator bool() const noexcept { return ticket_.has_value(); }

        /** @brief 发布任务（等价 post_reserved）。发布成功后句柄进入
         *  "已消费"状态，析构不再 abandon。 */
        bool publish(std::function<void()> task) {
            if (!state_ || !ticket_) return false;
            const bool published = state_->post_reserved(*ticket_, std::move(task));
            if (published) {
                ticket_.reset();
            } else {
                // post_reserved 失败（stopped/detached/重复发布）：ticket
                // 已在上下文侧终态化，句柄同步失效即可，无需再 abandon。
                ticket_.reset();
            }
            return published;
        }

        /** @brief 显式放弃预留（等价 abandon；幂等）。 */
        void abandon() noexcept {
            if (!state_ || !ticket_) return;
            state_->abandon(*ticket_);
            ticket_.reset();
        }

    private:
        friend class SerialExecutionContext;
        Reservation(std::shared_ptr<Shared> state, Ticket ticket) noexcept
            : state_(std::move(state)), ticket_(ticket) {}

        void release_unpublished() noexcept {
            if (state_ && ticket_) {
                state_->abandon(*ticket_);
            }
            ticket_.reset();
        }

        std::shared_ptr<Shared> state_;
        std::optional<Ticket> ticket_;
    };

    /** @brief RAII 预留：上下文已停止时返回空 optional（与 reserve()
     *  的 nullopt 语义一致）。 */
    std::optional<Reservation> reserve_guarded() {
        if (!state_) return std::nullopt;
        auto ticket = state_->reserve();
        if (!ticket) return std::nullopt;
        return Reservation(state_, *ticket);
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
