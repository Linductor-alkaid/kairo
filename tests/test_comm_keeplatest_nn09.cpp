/// @file test_comm_keeplatest_nn09.cpp
/// NN-09（0.7.2 §4.3）KeepLatest 滚动窗口语义的独立验证：
///
/// - 饱和置换只逐出最旧一条、计一次 overwrite；保留最新 capacity 条
///   （含新值）。旧实现 recycle_all_for_write 坍缩为 1 条且 overwrite
///   少计——此处按新语义钉死。
/// - capacity=1 行为不变（逐出 1 条 = 旧行为巧合一致）。
/// - 并发生产/消费竞争：无死锁、无双释放、逐条计账守恒（收到的 +
///   被逐出的 == 成功入队的）。
/// - RealtimeChannel（drain_for_cycle 路径）同语义。

#include <kairo/comm/channel.hpp>
#include <kairo/comm/mailbox.hpp>

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using kairo::comm::ChannelOptions;
using kairo::comm::CommStats;
using kairo::comm::DropPolicy;
using kairo::comm::MpscChannel;
using kairo::comm::RealtimeChannel;
using kairo::comm::RealtimeChannelOptions;
using namespace std::chrono_literals;

namespace {

ChannelOptions options(size_t capacity, DropPolicy policy) {
    ChannelOptions opts;
    opts.capacity = capacity;
    opts.drop_policy = policy;
    opts.enable_stats = true;
    return opts;
}

RealtimeChannelOptions realtime_options(size_t capacity, DropPolicy policy) {
    RealtimeChannelOptions opts;
    opts.capacity = capacity;
    opts.max_items_per_cycle = 64;
    opts.drop_policy = policy;
    return opts;
}

// 排空 MpscChannel 到空，返回收到的值序列。
std::vector<int> drain_all(MpscChannel<int>& channel) {
    std::vector<int> out;
    int value = 0;
    while (channel.try_receive(value)) {
        out.push_back(value);
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// 滚动窗口语义（单线程确定性）
// ---------------------------------------------------------------------------

TEST(CommKeepLatestNn09, RollingWindowEvictsOnlyOldestPerOverflow) {
    // capacity 3、发送 1..5：逐出 1、2（各计一次 overwrite），
    // 保留 [3,4,5]。
    MpscChannel<int> channel(options(3, DropPolicy::KeepLatest));
    for (int i = 1; i <= 5; ++i) {
        EXPECT_TRUE(channel.try_send(i));
    }

    const auto drained = drain_all(channel);
    EXPECT_EQ(drained, (std::vector<int>{3, 4, 5}))
        << "KeepLatest must retain the newest capacity items (rolling window)";
    EXPECT_EQ(channel.stats().overwritten_count, 2U)
        << "one overwrite per evicted item (evicted 2)";
}

TEST(CommKeepLatestNn09, CapacityOneBehavesLikeLatestValue) {
    // capacity=1：新旧行为一致（逐出 1 条 = 清空），钉死不回归。
    MpscChannel<int> channel(options(1, DropPolicy::KeepLatest));
    EXPECT_TRUE(channel.try_send(1));
    EXPECT_TRUE(channel.try_send(2));
    EXPECT_TRUE(channel.try_send(3));

    const auto drained = drain_all(channel);
    EXPECT_EQ(drained, (std::vector<int>{3}));
    EXPECT_EQ(channel.stats().overwritten_count, 2U);
}

TEST(CommKeepLatestNn09, UnderCapacityNoOverwriteCounted) {
    MpscChannel<int> channel(options(4, DropPolicy::KeepLatest));
    for (int i = 1; i <= 4; ++i) {
        EXPECT_TRUE(channel.try_send(i));
    }
    EXPECT_EQ(drain_all(channel), (std::vector<int>{1, 2, 3, 4}));
    EXPECT_EQ(channel.stats().overwritten_count, 0U);
}

TEST(CommKeepLatestNn09, RealtimeChannelRollingWindowBacklog) {
    // RealtimeChannel 的 drain_for_cycle 路径：capacity 3、发送 1..5
    // → 积压 [3,4,5]，overwrite 2。
    RealtimeChannel<int> channel(realtime_options(3, DropPolicy::KeepLatest));
    for (int i = 1; i <= 5; ++i) {
        EXPECT_TRUE(channel.try_send(i));
    }

    std::vector<int> drained;
    const uint64_t count = channel.drain_for_cycle([&](int v) { drained.push_back(v); });
    EXPECT_EQ(count, 3U);
    EXPECT_EQ(drained, (std::vector<int>{3, 4, 5}));
    EXPECT_EQ(channel.stats().overwritten_count, 2U);
}

TEST(CommKeepLatestNn09, ConsumerDrainMidStreamShiftsEvictionPoint) {
    // 消费者中途取走部分数据：置换点随之滚动（证明是窗口而非快照）。
    MpscChannel<int> channel(options(3, DropPolicy::KeepLatest));
    for (int i = 1; i <= 3; ++i) EXPECT_TRUE(channel.try_send(i));
    int value = 0;
    EXPECT_TRUE(channel.try_receive(value));  // 取走 1 → 剩 [2,3]
    EXPECT_TRUE(channel.try_send(4));         // [2,3,4]
    EXPECT_TRUE(channel.try_send(5));         // 逐出 2 → [3,4,5]
    EXPECT_EQ(drain_all(channel), (std::vector<int>{3, 4, 5}));
    EXPECT_EQ(channel.stats().overwritten_count, 1U);
}

// ---------------------------------------------------------------------------
// 并发压测：竞争下无死锁/双释放，计账守恒
// ---------------------------------------------------------------------------

TEST(CommKeepLatestNn09, ConcurrentProducersConsumerAccountingConserved) {
    // 2 生产者 × 4000 条、capacity 8 的 KeepLatest 通道，消费者并发
    // 接收。守恒式：received + overwritten == accepted（每条入队消息
    // 要么被消费、要么被置换逐出一次）。跑 4 轮。
    for (int round = 0; round < 4; ++round) {
        MpscChannel<int> channel(options(8, DropPolicy::KeepLatest));
        constexpr int kProducers = 2;
        constexpr int kPerProducer = 4000;

        std::atomic<int64_t> accepted{0};
        std::atomic<int64_t> received{0};
        std::atomic<bool> producers_done{false};

        std::vector<std::thread> producers;
        for (int p = 0; p < kProducers; ++p) {
            producers.emplace_back([&channel, &accepted, p] {
                for (int i = 0; i < kPerProducer; ++i) {
                    if (channel.try_send(p * kPerProducer + i)) {
                        accepted.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        std::thread consumer([&channel, &received, &producers_done] {
            int value = 0;
            while (!producers_done.load(std::memory_order_acquire)) {
                if (channel.try_receive(value)) {
                    received.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield();
                }
            }
            // 生产结束后排空残余。
            while (channel.try_receive(value)) {
                received.fetch_add(1, std::memory_order_relaxed);
            }
        });

        for (auto& t : producers) t.join();
        producers_done.store(true, std::memory_order_release);
        consumer.join();

        const auto stats = channel.stats();
        const int64_t received_total = received.load();
        EXPECT_EQ(received_total + static_cast<int64_t>(stats.overwritten_count),
                  accepted.load())
            << "round " << round << ": accounting broken — received " << received_total
            << " + overwritten " << stats.overwritten_count
            << " != accepted " << accepted.load();
    }
}

TEST(CommKeepLatestNn09, ConcurrentKeepLatestWithCloseNoDeadlock) {
    // 关闭与置换竞争：close 后 send 失败但已入队数据仍可收完；
    // 无死锁、无双重释放（对象析构干净——ASAN 泄漏检测兜底）。
    for (int round = 0; round < 4; ++round) {
        MpscChannel<int> channel(options(4, DropPolicy::KeepLatest));
        std::atomic<bool> stop{false};

        std::thread producer([&] {
            int i = 0;
            while (!stop.load(std::memory_order_acquire)) {
                (void)channel.try_send(i++);
                std::this_thread::yield();
            }
        });

        std::this_thread::sleep_for(20ms);
        int value = 0;
        while (channel.try_receive(value)) { /* 消费竞争 */ }
        channel.close();
        stop.store(true, std::memory_order_release);
        producer.join();

        // 关闭后排空：不挂起、不崩溃。
        while (channel.try_receive(value)) { }
    }
}
