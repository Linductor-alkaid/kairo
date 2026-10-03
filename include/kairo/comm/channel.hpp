#pragma once

#include <kairo/comm/bounded_queue.hpp>
#include <kairo/comm/fwd.hpp>
#include <kairo/comm/types.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <thread>
#include <utility>

namespace kairo::comm {

/**
 * Preallocated bounded MPSC channel with one logical consumer.
 *
 * try_send(), try_receive(), close(), and state queries acquire no mutex and
 * allocate no queue storage after construction. Payload operations and event
 * callbacks are outside that guarantee. send_for()/receive_for() are explicitly
 * non-realtime waiting adapters over the same non-blocking core.
 */
template <class T>
class MpscChannel {
public:
    explicit MpscChannel(ChannelOptions options = {})
        : options_(normalize_options(std::move(options))),
          queue_(options_.capacity, options_.drop_policy, options_.enable_stats,
                 options_.name, "channel") {}

    bool try_send(const T& value) { return try_send_impl(value, true); }
    bool try_send(T&& value) { return try_send_impl(std::move(value), true); }

    template <class Rep, class Period>
    CommResult send_for(T value, std::chrono::duration<Rep, Period> timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            if (queue_.is_closed()) {
                std::optional<CommEvent> event;
                (void)queue_.enqueue(std::move(value), event, false);
                queue_.emit(event);
                return CommResult::failure(CommErrorCode::Closed, "channel is closed");
            }

            if (try_send_impl(std::move(value), false)) {
                return CommResult::success();
            }
            if (options_.drop_policy != DropPolicy::RejectNewest) {
                // A competing consumer/drop operation can make one bounded
                // attempt fail; retry it like ordinary lock-free contention.
                std::this_thread::yield();
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                std::optional<CommEvent> event;
                queue_.record_timeout(event);
                queue_.emit(event);
                return CommResult::failure(CommErrorCode::Timeout,
                                           "channel send timed out");
            }
            std::this_thread::yield();
        }
    }

    bool try_receive(T& out) {
        auto item = queue_.try_pop();
        if (!item) return false;
        out = std::move(item->value);
        return true;
    }

    template <class Rep, class Period>
    CommResult receive_for(T& out, std::chrono::duration<Rep, Period> timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            if (try_receive(out)) return CommResult::success();
            if (queue_.is_closed() && !queue_.has_active_enqueues()) {
                // Recheck after observing the final producer leave so a send
                // admitted before close cannot be missed.
                if (try_receive(out)) return CommResult::success();
                return CommResult::failure(CommErrorCode::Closed, "channel is closed");
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                std::optional<CommEvent> event;
                queue_.record_timeout(event);
                queue_.emit(event);
                return CommResult::failure(CommErrorCode::Timeout,
                                           "channel receive timed out");
            }
            std::this_thread::yield();
        }
    }

    void close() noexcept { queue_.close(); }
    bool is_closed() const noexcept { return queue_.is_closed(); }
    bool is_drained() const noexcept { return queue_.is_drained(); }
    bool empty() const noexcept { return queue_.empty(); }
    size_t size_approx() const noexcept { return queue_.size(); }
    size_t capacity() const noexcept { return queue_.capacity(); }
    CommStats stats() const noexcept { return queue_.stats(); }

    // Configuration-plane operation. Configure before concurrent use.
    void set_event_callback(CommEventCallback callback) {
        queue_.set_event_callback(std::move(callback));
    }

    bool is_synchronization_lock_free() const noexcept {
        return queue_.is_synchronization_lock_free();
    }

    bool is_lock_free() const noexcept { return is_synchronization_lock_free(); }

private:
    static ChannelOptions normalize_options(ChannelOptions options) {
        if (options.capacity == 0) options.capacity = 1;
        return options;
    }

    template <class U>
    bool try_send_impl(U&& value, bool record_full_rejection) {
        std::optional<CommEvent> event;
        const bool sent = queue_.enqueue(std::forward<U>(value), event,
                                         record_full_rejection);
        queue_.emit(event);
        return sent;
    }

    ChannelOptions options_;
    BoundedQueue<T> queue_;
};

} // namespace kairo::comm
