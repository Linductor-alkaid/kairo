#pragma once

#include <kairo/comm/channel.hpp>
#include <kairo/comm/fwd.hpp>
#include <kairo/comm/types.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kairo::comm {

struct TopicSubscriptionOptions {
    size_t capacity = 1024;
    DropPolicy drop_policy = DropPolicy::RejectNewest;
    bool enable_stats = true;
    std::string name;
};

struct TopicPublishResult {
    size_t matched_subscribers = 0;
    size_t delivered_subscribers = 0;
    size_t rejected_subscribers = 0;

    explicit operator bool() const noexcept { return rejected_subscribers == 0; }
};

namespace detail {

template <class T>
struct TopicSubscriptionState {
    explicit TopicSubscriptionState(TopicSubscriptionOptions options)
        : channel(ChannelOptions{options.capacity, options.drop_policy,
                                 options.enable_stats, std::move(options.name)}) {}

    MpscChannel<T> channel;
};

template <class T>
struct TopicState {
    mutable std::mutex mutex;
    std::unordered_map<uint64_t, std::shared_ptr<TopicSubscriptionState<T>>> subscriptions;
    uint64_t next_subscription_id = 1;
    bool closed = false;
};

} // namespace detail

template <class T>
class TopicSubscription {
public:
    TopicSubscription() = default;

    TopicSubscription(TopicSubscription&& other) noexcept
        : state_(std::move(other.state_)), topic_(std::move(other.topic_)), id_(other.id_) {
        other.id_ = 0;
    }

    TopicSubscription& operator=(TopicSubscription&& other) noexcept {
        if (this == &other) return *this;
        close();
        state_ = std::move(other.state_);
        topic_ = std::move(other.topic_);
        id_ = other.id_;
        other.id_ = 0;
        return *this;
    }

    TopicSubscription(const TopicSubscription&) = delete;
    TopicSubscription& operator=(const TopicSubscription&) = delete;

    ~TopicSubscription() { close(); }

    bool try_receive(T& out) {
        auto state = state_;
        return state && state->channel.try_receive(out);
    }

    template <class Rep, class Period>
    CommResult receive_for(T& out, std::chrono::duration<Rep, Period> timeout) {
        auto state = state_;
        if (!state) {
            return CommResult::failure(CommErrorCode::Closed,
                                       "topic subscription is closed");
        }
        return state->channel.receive_for(out, timeout);
    }

    void close() {
        auto state = state_;
        if (!state) return;

        if (auto topic = topic_.lock()) {
            std::lock_guard<std::mutex> lock(topic->mutex);
            const auto entry = topic->subscriptions.find(id_);
            if (entry != topic->subscriptions.end() && entry->second == state) {
                topic->subscriptions.erase(entry);
            }
        }

        state->channel.close();
    }

    bool is_closed() const {
        auto state = state_;
        return !state || state->channel.is_closed();
    }

    CommStats stats() const {
        auto state = state_;
        return state ? state->channel.stats() : CommStats{};
    }

    void set_event_callback(CommEventCallback callback) {
        auto state = state_;
        if (state) state->channel.set_event_callback(std::move(callback));
    }

private:
    using State = detail::TopicSubscriptionState<T>;
    using Registry = detail::TopicState<T>;

    TopicSubscription(std::shared_ptr<State> state, std::weak_ptr<Registry> topic, uint64_t id)
        : state_(std::move(state)), topic_(std::move(topic)), id_(id) {}

    std::shared_ptr<State> state_;
    std::weak_ptr<Registry> topic_;
    uint64_t id_ = 0;

    friend class Topic<T>;
};

template <class T>
class Topic {
    static_assert(std::is_copy_constructible_v<T>,
                  "Topic<T> requires a copy-constructible payload for fan-out");

public:
    explicit Topic(std::string name = {})
        : state_(std::make_shared<Registry>()), name_(std::move(name)) {}

    Topic(const Topic&) = delete;
    Topic& operator=(const Topic&) = delete;
    Topic(Topic&&) = delete;
    Topic& operator=(Topic&&) = delete;

    ~Topic() { close(); }

    TopicSubscription<T> subscribe(TopicSubscriptionOptions options = {}) {
        if (options.name.empty()) options.name = name_;
        auto subscription = std::make_shared<SubscriptionState>(std::move(options));

        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->closed) {
            subscription->channel.close();
            return TopicSubscription<T>{std::move(subscription), {}, 0};
        }

        const uint64_t id = state_->next_subscription_id++;
        state_->subscriptions.emplace(id, subscription);
        return TopicSubscription<T>{std::move(subscription), state_, id};
    }

    TopicPublishResult publish(const T& value) { return publish_impl(value); }

    TopicPublishResult publish(T&& value) {
        auto subscriptions = subscription_snapshot();
        TopicPublishResult result;
        result.matched_subscribers = subscriptions.size();
        for (size_t index = 0; index < subscriptions.size(); ++index) {
            const bool delivered = index + 1 == subscriptions.size()
                                       ? subscriptions[index]->channel.try_send(std::move(value))
                                       : subscriptions[index]->channel.try_send(value);
            if (delivered) {
                ++result.delivered_subscribers;
            } else {
                ++result.rejected_subscribers;
            }
        }
        return result;
    }

    size_t subscriber_count() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->subscriptions.size();
    }

    void close() {
        std::vector<std::shared_ptr<SubscriptionState>> subscriptions;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (state_->closed) return;
            state_->closed = true;
            subscriptions.reserve(state_->subscriptions.size());
            for (const auto& entry : state_->subscriptions) {
                subscriptions.push_back(entry.second);
            }
            state_->subscriptions.clear();
        }

        for (const auto& subscription : subscriptions) {
            subscription->channel.close();
        }
    }

private:
    using SubscriptionState = detail::TopicSubscriptionState<T>;
    using Registry = detail::TopicState<T>;

    TopicPublishResult publish_impl(const T& value) {
        auto subscriptions = subscription_snapshot();

        TopicPublishResult result;
        result.matched_subscribers = subscriptions.size();
        for (const auto& subscription : subscriptions) {
            if (subscription->channel.try_send(value)) {
                ++result.delivered_subscribers;
            } else {
                ++result.rejected_subscribers;
            }
        }
        return result;
    }

    std::vector<std::shared_ptr<SubscriptionState>> subscription_snapshot() const {
        std::vector<std::shared_ptr<SubscriptionState>> subscriptions;
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->closed) return subscriptions;
        subscriptions.reserve(state_->subscriptions.size());
        for (const auto& entry : state_->subscriptions) {
            subscriptions.push_back(entry.second);
        }
        return subscriptions;
    }

    std::shared_ptr<Registry> state_;
    std::string name_;
};

} // namespace kairo::comm
