/**
 * 多线程测试 - 定位段错误问题
 */

#include "kairo/lockfree_task_executor.hpp"
#include "kairo/util/lockfree_queue.hpp"
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <set>
#include <cstdint>

using namespace kairo;

struct ProducerStallHook {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
};

static void stall_before_publish(void* context) {
    auto* hook = static_cast<ProducerStallHook*>(context);
    hook->entered.store(true, std::memory_order_release);
    while (!hook->release.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

static int test_reserved_head_is_skipped() {
    std::cout << "[P-002] reserved head skip regression test ...\n";
    util::LockFreeQueue<int> q(16, 1, true);
    ProducerStallHook hook;
    q.set_before_publish_hook(stall_before_publish, &hook);

    std::atomic<bool> first_result{true};
    std::thread stalled([&]() { first_result.store(q.push(1), std::memory_order_release); });
    while (!hook.entered.load(std::memory_order_acquire)) std::this_thread::yield();

    q.set_before_publish_hook(nullptr, nullptr);
    if (!q.push(2) || !q.push(3)) {
        std::cout << "  FAIL: later producers could not publish\n";
        hook.release.store(true, std::memory_order_release);
        stalled.join();
        return 1;
    }

    int values[2] = {};
    const size_t popped = q.pop_batch(values, 2);
    hook.release.store(true, std::memory_order_release);
    stalled.join();

    int retry_value = 4;
    bool retry_ok = false;
    for (int retry = 0; retry < 1000 && !retry_ok; ++retry) {
        retry_ok = q.push(retry_value);
        if (!retry_ok) std::this_thread::yield();
    }
    int recovered = 0;
    const bool recovered_ok = q.pop(recovered);
    const auto stats = q.get_stats();

    if (popped != 2 || values[0] != 2 || values[1] != 3 || first_result.load(std::memory_order_acquire) ||
        !retry_ok || !recovered_ok || recovered != 4 || stats.ready_count != 0) {
        std::cout << "  FAIL: stalled reservation was not safely skipped\n";
        return 1;
    }
    std::cout << "  PASS: later producers and consumer bypass reserved head\n";
    return 0;
}

static int test_lockfree_queue_reservation_cancellation_accounting() {
    std::cout << "[P-003] LockFreeQueueReservationCancellationAccounting ...\n";
    util::LockFreeQueue<int> q(16, 1, true);
    ProducerStallHook hook;
    q.set_before_publish_hook(stall_before_publish, &hook);

    std::atomic<bool> producer_result{true};
    std::thread producer([&]() { producer_result.store(q.push(42), std::memory_order_release); });
    while (!hook.entered.load(std::memory_order_acquire)) std::this_thread::yield();

    const auto reserved = q.get_stats();
    int item = 0;
    const bool consumed = q.pop(item);
    hook.release.store(true, std::memory_order_release);
    producer.join();

    const auto resolved = q.get_stats();
    if (reserved.reserved_count != 1 || reserved.reservation_wait_yields != 64 ||
        consumed || producer_result.load(std::memory_order_acquire) ||
        resolved.reservation_count != resolved.total_pushes + resolved.cancelled_reservation_count ||
        resolved.reservation_count != 1 || resolved.cancelled_reservation_count != 1 ||
        resolved.fail_reason != util::LockFreeQueueFailReason::ReservationCancelled) {
        std::cout << "  FAIL: reservation cancellation accounting is inconsistent\n";
        return 1;
    }
    std::cout << "  PASS: published + cancelled equals reserved\n";
    return 0;
}

static int test_exact_batch_cancelled_head_does_not_block_later_items() {
    std::cout << "[P-001] exact batch cancelled head regression test ...\n";
    util::LockFreeQueue<int> q(16, 1, true);
    ProducerStallHook hook;
    q.set_before_publish_hook(stall_before_publish, &hook);

    const int batch[] = {1, 2};
    std::atomic<bool> batch_result{true};
    std::thread stalled([&]() {
        batch_result.store(q.push_batch_exact(batch, 2), std::memory_order_release);
    });
    while (!hook.entered.load(std::memory_order_acquire)) std::this_thread::yield();

    int discarded[2] = {};
    const size_t popped = q.pop_batch(discarded, 2);
    hook.release.store(true, std::memory_order_release);
    stalled.join();

    q.set_before_publish_hook(nullptr, nullptr);
    if (!q.push(3) || !q.push(4)) {
        std::cout << "  FAIL: later producers could not publish\n";
        return 1;
    }

    int values[2] = {};
    const size_t recovered = q.pop_batch(values, 2);
    const auto stats = q.get_stats();
    if (popped != 0 || batch_result.load(std::memory_order_acquire) || recovered != 2 ||
        values[0] != 3 || values[1] != 4 || stats.ready_count != 0) {
        std::cout << "  FAIL: cancelled exact batch left a queue hole\n";
        return 1;
    }
    std::cout << "  PASS: cancelled exact batch does not block later items\n";
    return 0;
}

static void test_basic_multithread_submit() {
    std::cout << "测试: 多线程提交任务\n";

    LockFreeTaskExecutor executor(4096);
    executor.start();

    std::atomic<int> submitted{0};
    std::vector<std::thread> threads;

    // 启动4个线程，每个提交1000个任务
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < 1000; ++i) {
                bool success = executor.push_task([]() {
                    // 空任务
                });
                if (success) {
                    submitted.fetch_add(1);
                }
            }
            std::cout << "线程 " << t << " 完成\n";
        });
    }

    // 等待所有线程完成
    for (auto& t : threads) {
        t.join();
    }

    std::cout << "提交任务数: " << submitted.load() << "\n";

    // 等待任务处理完成
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::cout << "处理任务数: " << executor.processed_count() << "\n";

    executor.stop();

    std::cout << "测试完成！\n";
}

// 260626P003: 验证 LockFreeQueue::push_batch 在 16 个生产者并发
// push_batch(16) + 1 个消费者快速 pop_batch 的高压下,既不丢失数据
// 也不产生重复或槽位错乱。
//
// 关键不变量:
//   1) 消费总数 == 生产总数 (无丢失、无重复)
//   2) 每个 producer 内部的 seq 是 [0, kBatchesPerProducer*kItemsPerBatch)
//      内的不重复整数(允许乱序消费 + 部分成功,但不允许重复或越界)
//   3) 不同 producer 的 payload 不会混淆
//
// 修复前: push_batch 在 CAS 成功后覆盖式写入 buffer_/sequences_,
// 可能在高竞争下与并发 push/pop 交叉导致槽位错乱、丢失或重复。
// 修复后: CAS 之前预校验所有目标槽位 sequences_[index] == pos+i,
// 不一致则放弃本轮预留重试,从而保证「已发布」的 batch 在写入期间
// 不会被其他线程改动。
//
// 260626P003-fix: 修正测试逻辑。push_batch 在 available < count 时
// 返回 true 且 pushed < count(部分成功),这些元素已经成功入队,
// 必须计入 per_producer_pushed。原实现把 ok && pushed<count 误判为
// 「完全失败」,导致 popped > pushed。CI 现象: pushed=4080 popped=4095,
// producer 8 got=1007 expected=992 (差 15,正好是被误判的部分推送数)。
// 修正后: 仅 !ok 算失败(MAX_RETRIES 耗尽或 available==0);
// ok=true 则按 pushed 计入(无论是否达到 batch_size)。
// 同时将 seq 范围上界从「expected」解耦为 kBatchesPerProducer*kItemsPerBatch,
// 以正确处理部分推送导致的非连续 seq 序列。
static int test_push_batch_cas_retry_consistency() {
    std::cout << "[P-260626-003] push_batch CAS retry consistency test ...\n";

    constexpr int kProducers = 16;
    constexpr int kBatchesPerProducer = 100;
    constexpr int kItemsPerBatch = 16;
    constexpr int kMaxSeq = kBatchesPerProducer * kItemsPerBatch;  // 1600
    constexpr size_t kCapacity = 4096;

    using Payload = uint64_t;
    auto make_payload = [](int p, int s) -> Payload {
        return (static_cast<uint64_t>(static_cast<uint32_t>(p)) << 32) |
               static_cast<uint32_t>(s);
    };
    auto producer_of = [](Payload x) -> int {
        return static_cast<int>(x >> 32);
    };
    auto seq_of = [](Payload x) -> int {
        return static_cast<int>(x & 0xffffffffu);
    };

    util::LockFreeQueue<Payload> q(kCapacity, 1, true);

    std::atomic<bool> start_flag{false};
    std::atomic<int> producers_finished{0};       // 完成全部 batch 的 producer 数
    std::atomic<int> producer_failure_count{0};   // 仅统计 !ok (完全失败)
    std::atomic<int> producer_partial_count{0};   // 统计 ok && pushed < kItemsPerBatch
    std::vector<std::atomic<int>> per_producer_pushed(kProducers);
    for (int i = 0; i < kProducers; ++i) per_producer_pushed[i].store(0);

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p]() {
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            Payload buf[kItemsPerBatch];
            for (int b = 0; b < kBatchesPerProducer; ++b) {
                for (int i = 0; i < kItemsPerBatch; ++i) {
                    buf[i] = make_payload(p, b * kItemsPerBatch + i);
                }
                size_t pushed = 0;
                bool ok = q.push_batch(buf, kItemsPerBatch, pushed);
                if (!ok) {
                    // 260626P003-fix: 只有 push_batch 返回 false (MAX_RETRIES
                    // 耗尽或 available==0) 才算完全失败 — pushed 必然为 0。
                    producer_failure_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    // 260626P003-fix: ok=true 表示 pushed 个元素已经成功入队,
                    // 必须计入 per_producer_pushed,无论是否达到 kItemsPerBatch。
                    // 部分成功(available < count)是 push_batch 的合法返回值。
                    if (pushed > 0) {
                        per_producer_pushed[p].fetch_add(
                            static_cast<int>(pushed), std::memory_order_relaxed);
                    }
                    if (pushed != static_cast<size_t>(kItemsPerBatch)) {
                        producer_partial_count.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                }
            }
            producers_finished.fetch_add(1, std::memory_order_release);
        });
    }

    // 消费者: 跑一个独立的 pop 循环,直到所有 producer 都 join 后,再
    // pop 几次确认队列空,最后退出。
    constexpr size_t kPopBufSize = 256;
    std::vector<Payload> pop_buf(kPopBufSize);
    std::set<Payload> seen;
    int64_t total_popped = 0;

    std::thread consumer([&]() {
        while (!start_flag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        while (true) {
            const size_t popped = q.pop_batch(pop_buf.data(), kPopBufSize);
            for (size_t i = 0; i < popped; ++i) seen.insert(pop_buf[i]);
            total_popped += static_cast<int64_t>(popped);
            // 所有 producer 的 push_batch 均已返回后才允许以一次空 pop 判定
            // 队列耗尽；此前空 pop 只表示当前消费速度快于生产速度。
            if (popped == 0 &&
                producers_finished.load(std::memory_order_acquire) == kProducers) {
                break;
            }
            std::this_thread::yield();
        }
    });

    // 启动所有 producer
    start_flag.store(true, std::memory_order_release);

    // 等待所有 producer
    for (auto& t : producers) t.join();

    // consumer 会在 idle_polls >= 1000 时自动退出;此时 producer 已全部
    // join,所有成功入队的元素都已被 pop。
    consumer.join();

    int64_t total_pushed = 0;
    for (int p = 0; p < kProducers; ++p) {
        total_pushed += per_producer_pushed[p].load(std::memory_order_relaxed);
    }

    std::cout << "  producers pushed total = " << total_pushed << "\n";
    std::cout << "  consumer popped total  = " << total_popped << "\n";
    std::cout << "  seen unique payloads   = " << seen.size() << "\n";
    std::cout << "  push_batch failures    = " << producer_failure_count.load()
              << "\n";
    std::cout << "  push_batch partials    = " << producer_partial_count.load()
              << "\n";

    int rc = 0;

    // 1) 不丢失: popped == pushed (sum of per_producer_pushed)
    if (total_popped != total_pushed) {
        std::cout << "  FAIL: lost or duplicated items (pushed=" << total_pushed
                  << " popped=" << total_popped << ")\n";
        rc = 1;
    }

    // 2) 无重复
    if (static_cast<int64_t>(seen.size()) != total_popped) {
        std::cout << "  FAIL: duplicate payloads detected (unique=" << seen.size()
                  << " popped=" << total_popped << ")\n";
        rc = 1;
    }

    // 3) 无错位: 每个 (producer, seq) 配对只出现一次,且 seq 范围合法
    std::vector<std::vector<int>> per_producer_seqs(kProducers);
    for (Payload x : seen) {
        int p = producer_of(x);
        if (p < 0 || p >= kProducers) {
            std::cout << "  FAIL: payload has out-of-range producer id " << p << "\n";
            rc = 1;
            continue;
        }
        per_producer_seqs[p].push_back(seq_of(x));
    }
    // 260626P003-fix: seq 范围上界是 kMaxSeq (1600),不再用 expected 作为
    // 上界 — 部分推送会导致 seq 非连续 [0, expected),但只要每个 seq 在
    // [0, kMaxSeq) 内且唯一,就合法。
    for (int p = 0; p < kProducers; ++p) {
        const auto& v = per_producer_seqs[p];
        int expected_count = per_producer_pushed[p].load(std::memory_order_relaxed);
        if (static_cast<int>(v.size()) != expected_count) {
            std::cout << "  FAIL: producer " << p << " size mismatch (got="
                      << v.size() << " expected=" << expected_count << ")\n";
            rc = 1;
        }
        std::vector<char> marker(kMaxSeq, 0);
        bool seq_ok = true;
        for (int s : v) {
            if (s < 0 || s >= kMaxSeq) { seq_ok = false; break; }
            if (marker[s]) { seq_ok = false; break; }
            marker[s] = 1;
        }
        if (!seq_ok) {
            std::cout << "  FAIL: producer " << p
                      << " has out-of-range or duplicate seq\n";
            rc = 1;
        }
    }

    if (rc == 0) {
        std::cout << "  PASS: push_batch CAS retry consistency holds\n";
    }
    return rc;
}

int main() {
    test_basic_multithread_submit();
    int rc2 = test_push_batch_cas_retry_consistency();
    int rc3 = test_reserved_head_is_skipped();
    int rc4 = test_exact_batch_cancelled_head_does_not_block_later_items();
    int rc5 = test_lockfree_queue_reservation_cancellation_accounting();
    std::cout << "\n=== P-260626-003 test result: "
              << (rc2 == 0 && rc3 == 0 && rc4 == 0 && rc5 == 0 ? "PASS" : "FAIL") << " ===\n";
    return rc2 || rc3 || rc4 || rc5;
}
