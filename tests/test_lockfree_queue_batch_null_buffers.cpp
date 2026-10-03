#include <gtest/gtest.h>

#include "kairo/util/lockfree_queue.hpp"

#include <cstddef>

using kairo::util::LockFreeQueue;

TEST(LockFreeQueueBatchNullBuffersTest, PushBatchRejectsNullItemsWhenCountPositive) {
    LockFreeQueue<int> q(8);
    size_t pushed = 42;

    EXPECT_FALSE(q.push_batch(nullptr, 1, pushed));
    EXPECT_EQ(pushed, 0u);
    EXPECT_EQ(q.size(), 0u);
}

TEST(LockFreeQueueBatchNullBuffersTest, PushBatchExactRejectsNullItemsWhenCountPositive) {
    LockFreeQueue<int> q(8);

    EXPECT_FALSE(q.push_batch_exact(nullptr, 1));
    EXPECT_EQ(q.size(), 0u);
}

TEST(LockFreeQueueBatchNullBuffersTest, PopBatchRejectsNullItemsWhenMaxCountPositive) {
    LockFreeQueue<int> q(8);
    ASSERT_TRUE(q.push(7));

    EXPECT_EQ(q.pop_batch(nullptr, 1), 0u);
    EXPECT_EQ(q.size(), 1u);

    int out = 0;
    ASSERT_TRUE(q.pop(out));
    EXPECT_EQ(out, 7);
}

TEST(LockFreeQueueBatchNullBuffersTest, NullItemsAllowedForZeroLengthBatches) {
    LockFreeQueue<int> q(8);
    size_t pushed = 42;

    EXPECT_TRUE(q.push_batch(nullptr, 0, pushed));
    EXPECT_EQ(pushed, 0u);
    EXPECT_TRUE(q.push_batch_exact(nullptr, 0));
    EXPECT_EQ(q.pop_batch(nullptr, 0), 0u);
}
