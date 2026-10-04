#include <atomic>
#include <iostream>
#include <thread>
#include <vector>

#include "kairo/util/lockfree_queue.hpp"

#define TEST_ASSERT(condition, message)                                      \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::cerr << "FAILED: " << message << " at " << __FILE__      \
                      << ':' << __LINE__ << '\n';                          \
            return false;                                                    \
        }                                                                    \
    } while (0)

namespace {

using kairo::util::LockFreeQueue;

struct ReservationHook {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
};

void pause_reservation(void* context) {
    auto* hook = static_cast<ReservationHook*>(context);
    hook->entered.store(true, std::memory_order_release);
    while (!hook->release.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

bool test_size_empty_and_stats() {
    LockFreeQueue<int> queue(4, 1, true);
    TEST_ASSERT(queue.empty(), "queue should start empty");
    TEST_ASSERT(queue.size() == 0, "queue size should start at zero");

    TEST_ASSERT(queue.push(10), "push should succeed");
    TEST_ASSERT(queue.push(20), "second push should succeed");
    TEST_ASSERT(!queue.empty(), "queue should not be empty after pushes");
    TEST_ASSERT(queue.size() == 2, "queue size should report two items");

    int value = 0;
    TEST_ASSERT(queue.pop(value) && value == 10, "first pop value mismatch");
    TEST_ASSERT(queue.pop(value) && value == 20, "second pop value mismatch");
    TEST_ASSERT(queue.empty(), "queue should be empty after pops");

    const auto stats = queue.get_stats();
    TEST_ASSERT(stats.total_pushes == 2, "stats should count successful pushes");
    TEST_ASSERT(stats.total_pops == 2, "stats should count successful pops");
    return true;
}

bool test_batch_exact_and_pop_batch() {
    LockFreeQueue<int> queue(8, 1, true);
    int input[] = {1, 2, 3, 4};
    TEST_ASSERT(queue.push_batch_exact(input, 4), "exact batch push should succeed");

    int output[4] = {0, 0, 0, 0};
    const size_t popped = queue.pop_batch(output, 4);
    TEST_ASSERT(popped == 4, "pop_batch should return four items");
    for (int index = 0; index < 4; ++index) {
        TEST_ASSERT(output[index] == input[index], "batch order mismatch");
    }
    TEST_ASSERT(queue.empty(), "queue should be empty after batch pop");
    return true;
}

bool test_reservation_cancellation_is_observable() {
    LockFreeQueue<int> queue(2, 1, true);
    ReservationHook hook;
    queue.set_before_publish_hook(pause_reservation, &hook);

    std::atomic<bool> producer_result{true};
    std::thread producer([&] {
        producer_result.store(queue.push(1), std::memory_order_release);
    });

    while (!hook.entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    int ignored = 0;
    TEST_ASSERT(!queue.pop(ignored),
                "consumer must not observe a reserved-but-unpublished item");

    hook.release.store(true, std::memory_order_release);
    producer.join();

    const auto stats = queue.get_stats();
    TEST_ASSERT(!producer_result.load(std::memory_order_acquire),
                "stalled producer should fail after consumer cancellation");
    TEST_ASSERT(stats.reservation_cancelled_rejections == 1,
                "cancellation should be observable through queue stats");
    return true;
}

bool test_multi_producer_single_consumer_counts() {
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 10'000;
    LockFreeQueue<int> queue(4096, 1, true);

    std::vector<std::thread> producers;
    for (int producer = 0; producer < kProducers; ++producer) {
        producers.emplace_back([&, producer] {
            for (int index = 0; index < kPerProducer; ++index) {
                while (!queue.push(producer * kPerProducer + index)) {
                    std::this_thread::yield();
                }
            }
        });
    }

    std::atomic<int> received{0};
    std::atomic<int> sum{0};
    while (received.load(std::memory_order_acquire) < kProducers * kPerProducer) {
        int value = 0;
        if (queue.pop(value)) {
            received.fetch_add(1, std::memory_order_relaxed);
            sum.fetch_add(value, std::memory_order_relaxed);
        }
    }

    for (auto& producer : producers) {
        producer.join();
    }

    const int expected_sum =
        kProducers * (kPerProducer * (kPerProducer - 1) / 2) +
        (kProducers * (kProducers - 1) / 2) * kPerProducer * kPerProducer;
    TEST_ASSERT(sum.load(std::memory_order_relaxed) == expected_sum,
                "MPSC counter sum mismatch");
    return true;
}

} // namespace

int main() {
    bool ok = true;
    ok &= test_size_empty_and_stats();
    ok &= test_batch_exact_and_pop_batch();
    ok &= test_reservation_cancellation_is_observable();
    ok &= test_multi_producer_single_consumer_counts();
    std::cout << (ok ? "All Android lockfree queue core tests PASSED\n"
                     : "Android lockfree queue core tests FAILED\n");
    return ok ? 0 : 1;
}
