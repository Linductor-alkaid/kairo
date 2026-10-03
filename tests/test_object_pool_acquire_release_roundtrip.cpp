#include "util/object_pool.hpp"

#include <gtest/gtest.h>

namespace {

struct PoolValue {
    int value{0};
};

TEST(ObjectPoolReleaseGuard, AcquireReleaseRoundtripReusesSlot) {
    kairo::util::ObjectPool<PoolValue> pool(1);

    PoolValue* first = pool.acquire();
    ASSERT_NE(first, nullptr);
    first->value = 17;
    EXPECT_EQ(pool.acquire(), nullptr);

    pool.release(first);

    PoolValue* second = pool.acquire();
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second, first);

    pool.release(second);
}

}  // namespace
