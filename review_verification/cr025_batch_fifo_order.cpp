// CR-025: batch tasks share one submit_time_ns (thread_pool.cpp:1111-1123);
// Task::operator< tie-breaks equal priorities by submit_time_ns, so all tasks
// in one batch compare "equivalent" and the heap (std::push_heap/pop_heap in
// priority_scheduler.cpp) gives NO FIFO guarantee within a batch.
//
// Part 1 (mechanism): drive PriorityScheduler directly — enqueue_batch of N
// same-priority/same-timestamp tasks, dequeue one by one, record execution
// order vs submission order.
//
// Part 2 (end-to-end): single-worker ThreadPool (min=max=1), repeated
// try_submit_batch(32) rounds; each task records its global execution index;
// compare with submission index. Any mismatch => in-batch FIFO violated.
#include "kairo/thread_pool/thread_pool.hpp"
#include "kairo/thread_pool/priority_scheduler.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

using namespace kairo;
namespace chrono = std::chrono;

int main() {
    setbuf(stdout, nullptr);
    int total_misplaced = 0;

    // ---------------- Part 1: direct PriorityScheduler ----------------
    {
        constexpr int kBatch = 32;
        constexpr int kRounds = 200;
        PriorityScheduler scheduler;
        int mismatch_rounds = 0;
        for (int round = 0; round < kRounds; ++round) {
            std::vector<std::unique_ptr<Task>> batch(kBatch);
            std::vector<int> out(kBatch, -1);
            int cursor = 0;
            for (int i = 0; i < kBatch; ++i) {
                batch[i] = std::make_unique<Task>();
                batch[i]->priority = TaskPriority::NORMAL;
                batch[i]->submit_time_ns = 1234567890LL;  // identical on purpose
                int idx = i;
                batch[i]->function = [&out, &cursor, idx]() {
                    out[cursor++] = idx;
                };
            }
            scheduler.enqueue_batch(batch.data(), kBatch);
            Task t;
            while (scheduler.dequeue(t)) {
                if (t.function) t.function();
            }
            bool bad = false;
            for (int k = 0; k < kBatch; ++k) {
                if (out[k] != k) {
                    ++total_misplaced;
                    bad = true;
                }
            }
            if (bad && mismatch_rounds == 0) {
                printf("[scheduler-direct] round %d order: ", round);
                for (int k = 0; k < kBatch; ++k) printf("%d ", out[k]);
                printf("\n");
            }
            if (bad) ++mismatch_rounds;
        }
        printf("[scheduler-direct] rounds=%d mismatch_rounds=%d "
               "misplaced_positions=%d/%d\n",
               kRounds, mismatch_rounds, total_misplaced, kRounds * kBatch);
    }

    // ---------------- Part 2: end-to-end, single worker ----------------
    {
        constexpr int kBatch = 32;
        constexpr int kRounds = 50;
        ThreadPool pool;
        ThreadPoolConfig config;
        config.min_threads = 1;
        config.max_threads = 1;  // single worker: no parallel reordering
        config.queue_capacity = 1000;
        if (!pool.initialize(config)) {
            printf("FATAL: initialize failed\n");
            return 2;
        }

        int mismatch_rounds = 0;
        int pool_misplaced = 0;
        for (int round = 0; round < kRounds; ++round) {
            std::vector<std::function<void()>> tasks(kBatch);
            std::vector<int> out(kBatch, -1);
            std::atomic<int> cursor{0};
            for (int i = 0; i < kBatch; ++i) {
                int idx = i;
                tasks[i] = [&out, &cursor, idx]() { out[cursor.fetch_add(1)] = idx; };
            }
            if (!pool.try_submit_batch(std::move(tasks))) {
                printf("FATAL: batch rejected round %d\n", round);
                return 2;
            }
            if (!pool.try_wait_for_completion(chrono::seconds(5))) {
                printf("FATAL: round %d did not finish in 5s\n", round);
                return 2;
            }
            bool bad = false;
            for (int k = 0; k < kBatch; ++k) {
                if (out[k] != k) {
                    ++pool_misplaced;
                    bad = true;
                }
            }
            if (bad && mismatch_rounds == 0) {
                printf("[pool-1-worker] round %d order: ", round);
                for (int k = 0; k < kBatch; ++k) printf("%d ", out[k]);
                printf("\n");
            }
            if (bad) ++mismatch_rounds;
        }
        printf("[pool-1-worker] rounds=%d mismatch_rounds=%d "
               "misplaced_positions=%d/%d\n",
               kRounds, mismatch_rounds, pool_misplaced, kRounds * kBatch);
        total_misplaced += pool_misplaced;
    }

    printf("CR-025 verdict: %s (total misplaced positions = %d)\n",
           total_misplaced > 0
               ? "CONFIRMED (same-priority batch execution order deviates from "
                 "submission order)"
               : "NOT REPRODUCED",
           total_misplaced);
    return total_misplaced > 0 ? 0 : 1;
}
