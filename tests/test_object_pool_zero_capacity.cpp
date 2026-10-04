/**
 * @file test_object_pool_zero_capacity.cpp
 * @brief Regression test for P-008: ObjectPool must reject capacity=0
 *
 * Background (see ~/.hermes/state/executor/2026-06-09.md, plan P-008):
 *   The previous ObjectPool constructor accepted capacity=0 silently. The
 *   free-list build loop uses `i < capacity - 1` and `storage_[capacity - 1]`,
 *   both of which underflow / out-of-range when capacity == 0, producing UB
 *   (typically a segfault on the first `storage_[i]->next = ...` access).
 *   This test pins down the contract: capacity=0 must throw
 *   std::invalid_argument, and a normal-capacity pool must still work.
 */

#include "util/object_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <iostream>
#include <stdexcept>

namespace {

// Simple trivially-default-constructible value type. We intentionally do NOT
// use TaskWrapper here to keep this test decoupled from the rest of the
// executor; the fix lives in the constructor and is independent of T.
struct PoolValue {
    int x{0};
};

// 1. The exact failure mode: constructing with capacity=0 must throw
//    std::invalid_argument rather than entering UB.
TEST(ObjectPoolZeroCapacity, ConstructorThrowsOnZero) {
    bool threw_invalid_argument = false;
    try {
        kairo::util::ObjectPool<PoolValue> pool(0);
        // If we reach here, the fix is missing.
        ADD_FAILURE() << "ObjectPool(0) should have thrown std::invalid_argument";
    } catch (const std::invalid_argument&) {
        threw_invalid_argument = true;
    } catch (const std::exception& e) {
        ADD_FAILURE() << "ObjectPool(0) threw the wrong exception type: "
                      << typeid(e).name() << " what=\"" << e.what() << "\"";
    } catch (...) {
        ADD_FAILURE() << "ObjectPool(0) threw a non-std exception";
    }
    EXPECT_TRUE(threw_invalid_argument)
        << "Expected std::invalid_argument to be thrown for capacity=0";
}

// 2. Sanity: a normal capacity still works after the fix. If the throw
//    path accidentally corrupted shared state (it shouldn't — we throw
//    from a local ctor with no side effects yet), this would catch it.
TEST(ObjectPoolZeroCapacity, NormalCapacityStillWorks) {
    kairo::util::ObjectPool<PoolValue> pool(8);
    PoolValue* a = pool.acquire();
    ASSERT_NE(a, nullptr) << "Fresh pool of capacity 8 should hand out an object";
    a->x = 42;
    PoolValue* b = pool.acquire();
    ASSERT_NE(b, nullptr);
    // Different objects on successive acquires.
    EXPECT_NE(a, b);
    b->x = 7;
    pool.release(a);
    pool.release(b);
    // Pool should be reusable.
    PoolValue* c = pool.acquire();
    EXPECT_NE(c, nullptr);
    pool.release(c);
}

// 3. Default-constructed pool (capacity=1024) is unaffected by the fix.
TEST(ObjectPoolZeroCapacity, DefaultCapacityUnaffected) {
    kairo::util::ObjectPool<PoolValue> pool;
    PoolValue* p = pool.acquire();
    ASSERT_NE(p, nullptr);
    pool.release(p);
}

// 4. Exhausted pool returns nullptr (regression guard — the fix must not
//    alter the steady-state contract of acquire()).
TEST(ObjectPoolZeroCapacity, ExhaustionReturnsNullptr) {
    kairo::util::ObjectPool<PoolValue> pool(3);
    PoolValue* objs[3];
    for (int i = 0; i < 3; ++i) {
        objs[i] = pool.acquire();
        ASSERT_NE(objs[i], nullptr) << "Acquire #" << i << " should succeed";
    }
    EXPECT_EQ(pool.acquire(), nullptr) << "Fourth acquire on capacity-3 pool must be nullptr";
    for (int i = 0; i < 3; ++i) pool.release(objs[i]);
}

}  // namespace
