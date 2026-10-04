# ObjectPool ABA 修复设计文档

**日期：** 2026-04-09  
**状态：** 已批准  
**范围：** `src/util/object_pool.hpp`

## 问题描述

`ObjectPool::acquire()` 使用无锁 free list（指针 CAS）实现，存在经典 ABA 问题：

1. 线程A 读 `free_list_` = Node1，读 `Node1->next` = Node2
2. 线程B acquire Node1，acquire Node2，release Node2，release Node1
   （Node1 重回 head，但 Node1->next 现在指向 Node2）
3. 线程A 的 CAS 成功（Node1 指针值相同），`free_list_` 被设为旧的 Node2
4. Node2 已被线程B 持有并在使用中 → 两个线程同时持有同一个 Node → SEGFAULT

该 bug 在 16+ 生产者线程的高并发场景下稳定触发，在 Windows 上尤为明显（Linux 因调度差异不易复现）。

## 修复方案

用 `std::mutex` 保护 free list 操作，彻底消除 ABA 问题。

### 理由

- `ObjectPool::acquire/release` 不是热路径（每次 `push_task` 调用一次）
- mutex 开销约 20-50ns，远小于 `std::function` 构造和任务执行开销
- 实现简单，正确性容易验证，无平台相关代码

## 变更内容

**文件：** `src/util/object_pool.hpp`

### 移除
- `std::atomic<Node*> free_list_`
- `Node` 中的 `std::atomic<Node*> next`
- `#include <atomic>`

### 新增
- `std::mutex mutex_`
- `Node* free_list_`（普通指针）
- `Node` 中的 `Node* next`（普通指针）
- `#include <mutex>`

### acquire() 伪代码
```cpp
T* acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!free_list_) return nullptr;
    Node* node = free_list_;
    free_list_ = node->next;
    return &node->data;
}
```

### release() 伪代码
```cpp
void release(T* obj) {
    if (!obj) return;
    Node* node = /* offsetof 计算 */;
    std::lock_guard<std::mutex> lock(mutex_);
    node->next = free_list_;
    free_list_ = node;
}
```

## 接口兼容性

`acquire()` / `release()` 签名不变，调用方（`lockfree_task_executor.cpp`、`realtime_thread_executor.hpp`）无需任何改动。

## 验证

修复后运行以下测试（原来在高并发下稳定 SEGFAULT）：
- `benchmark_adaptive_backoff`
- `benchmark_lockfree_mpsc`
- `test_lifecycle_mpsc`
