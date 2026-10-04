# ObjectPool ABA Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 `ObjectPool` 的 free list 从无锁指针 CAS 改为 mutex 保护，消除 ABA 问题导致的 SEGFAULT。

**Architecture:** 仅修改 `src/util/object_pool.hpp`，将 `std::atomic<Node*>` 替换为普通指针 + `std::mutex`，接口不变。

**Tech Stack:** C++20, std::mutex, std::lock_guard

---

### Task 1: 修改 object_pool.hpp

**Files:**
- Modify: `src/util/object_pool.hpp`

- [ ] **Step 1: 替换整个文件内容**

将 `src/util/object_pool.hpp` 改为以下内容：

```cpp
#pragma once

#include <memory>
#include <mutex>
#include <vector>

namespace executor {
namespace util {

/**
 * @brief Object pool for task allocation
 *
 * Pre-allocates objects and provides mutex-protected acquire/release operations.
 * Uses a free-list approach. Mutex protection eliminates ABA problem present
 * in the previous lock-free CAS implementation.
 */
template<typename T>
class ObjectPool {
public:
    explicit ObjectPool(size_t capacity = 1024) : capacity_(capacity) {
        // Pre-allocate all objects
        storage_.reserve(capacity);
        for (size_t i = 0; i < capacity; ++i) {
            storage_.emplace_back(std::make_unique<Node>());
        }

        // Build free list
        for (size_t i = 0; i < capacity - 1; ++i) {
            storage_[i]->next = storage_[i + 1].get();
        }
        storage_[capacity - 1]->next = nullptr;

        free_list_ = storage_[0].get();
    }

    /**
     * @brief Acquire an object from the pool
     * @return Pointer to object, or nullptr if pool is exhausted
     */
    T* acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!free_list_) return nullptr;
        Node* node = free_list_;
        free_list_ = node->next;
        return &node->data;
    }

    /**
     * @brief Release an object back to the pool
     * @param obj Pointer to object to release
     */
    void release(T* obj) {
        if (!obj) return;

        Node* node = reinterpret_cast<Node*>(
            reinterpret_cast<char*>(obj) - offsetof(Node, data)
        );

        std::lock_guard<std::mutex> lock(mutex_);
        node->next = free_list_;
        free_list_ = node;
    }

private:
    struct Node {
        T data;
        Node* next{nullptr};
    };

    size_t capacity_;
    std::vector<std::unique_ptr<Node>> storage_;
    std::mutex mutex_;
    Node* free_list_{nullptr};
};

} // namespace util
} // namespace executor
```

- [ ] **Step 2: 构建验证**

```bash
cd build && cmake --build . --config Debug -j 2>&1 | grep -E "error C|error LNK|Build FAILED"
```

期望输出：无错误，所有 target 构建成功。

- [ ] **Step 3: 运行验证测试**

```bash
cd build && ctest -C Debug -R "benchmark_adaptive_backoff|benchmark_lockfree_mpsc|test_lifecycle_mpsc" --output-on-failure -V
```

期望输出：
```
100% tests passed, 0 tests failed out of 3
```

- [ ] **Step 4: 运行全量测试**

```bash
cd build && ctest -C Debug --output-on-failure --timeout 120
```

期望输出：
```
100% tests passed, 0 tests failed out of 52
```

- [ ] **Step 5: Commit**

```bash
cd g:/myproject/executor
git add src/util/object_pool.hpp
git commit -m "fix: replace lock-free CAS free list with mutex in ObjectPool to eliminate ABA problem"
```
