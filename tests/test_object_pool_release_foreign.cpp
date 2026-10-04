#include "util/object_pool.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace {

struct PoolValue {
    int value{0};
};

TEST(ObjectPoolReleaseGuard, ForeignPointerThrowsLogicError) {
    kairo::util::ObjectPool<PoolValue> pool(1);
    PoolValue foreign;

    EXPECT_THROW(pool.release(&foreign), std::logic_error);
}

TEST(ObjectPoolReleaseGuard, MaximumAddressThrowsLogicError) {
    kairo::util::ObjectPool<PoolValue> pool(1);
    auto* pointer = reinterpret_cast<PoolValue*>(
        std::numeric_limits<std::uintptr_t>::max());

    EXPECT_THROW(pool.release(pointer), std::logic_error);
}

TEST(ObjectPoolReleaseGuard, UnalignedPointerThrowsLogicError) {
    kairo::util::ObjectPool<PoolValue> pool(1);
    PoolValue* value = pool.acquire();
    ASSERT_NE(value, nullptr);
    const auto unaligned_address = reinterpret_cast<std::uintptr_t>(value) + 1;
    auto* pointer = reinterpret_cast<PoolValue*>(unaligned_address);

    EXPECT_THROW(pool.release(pointer), std::logic_error);
    EXPECT_NO_THROW(pool.release(value));
}

TEST(ObjectPoolReleaseGuard, PointerImmediatelyAfterStorageThrowsLogicError) {
    kairo::util::ObjectPool<PoolValue> pool(2);
    PoolValue* first = pool.acquire();
    PoolValue* second = pool.acquire();
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    const auto first_address = reinterpret_cast<std::uintptr_t>(first);
    const auto second_address = reinterpret_cast<std::uintptr_t>(second);
    const auto node_size = second_address - first_address;
    auto* pointer = reinterpret_cast<PoolValue*>(second_address + node_size);

    EXPECT_THROW(pool.release(pointer), std::logic_error);
    EXPECT_NO_THROW(pool.release(first));
    EXPECT_NO_THROW(pool.release(second));
}

TEST(ObjectPoolReleaseGuard, InBoundsPointerCanBeReleased) {
    kairo::util::ObjectPool<PoolValue> pool(1);
    PoolValue* value = pool.acquire();
    ASSERT_NE(value, nullptr);

    EXPECT_NO_THROW(pool.release(value));
    EXPECT_EQ(pool.acquire(), value);
}

}  // namespace
