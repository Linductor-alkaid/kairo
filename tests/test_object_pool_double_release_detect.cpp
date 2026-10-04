#include "util/object_pool.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

namespace {

struct PoolValue {
    int value{0};
};

TEST(ObjectPoolReleaseGuard, DoubleReleaseThrowsLogicError) {
    kairo::util::ObjectPool<PoolValue> pool(1);
    PoolValue* value = pool.acquire();
    ASSERT_NE(value, nullptr);

    pool.release(value);

    EXPECT_THROW(pool.release(value), std::logic_error);
}

}  // namespace
