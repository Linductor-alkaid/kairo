/// @file test_lockfree_worker_queue_cr023.cpp
/// CR-023（0.7.2 §4.3 补充）LockFreeWorkerQueue::push 两个 overload 的
/// 独立验证：字段拷贝（std::function 的拷贝/移动赋值）抛出时归还
/// task_ptr 再重抛——不再泄漏节点内存。
///
/// 泄漏的直接证据由 ASAN（LeakSanitizer）子集提供：每个触发异常的
/// push 恰好分配一个 Task 节点，修复前该节点在异常路径上泄漏，
/// LSan 在进程退出时报 "detected memory leaks"。本文件在普通构建下
/// 钉死行为面：异常原样传播、队列状态不变、队列仍可用。

#include "kairo/thread_pool/lockfree_worker_queue.hpp"
#include "kairo/task/task.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <utility>

using namespace kairo;

namespace {

/// 拷贝构造在 armed 置位后抛出；首次拷贝（装入 std::function 时）
/// 不抛——使异常精确落在 push() 内部的字段拷贝处。
/// 注意：不得声明 move 构造/赋值（哪怕 = default）——noexcept move
/// 会让 libstdc++ 的 std::function 移动赋值走真正的搬移而不退化成
/// 拷贝，move overload 的注入点随之消失（与本仓库
/// test_lockfree_worker_queue.cpp 的 ThrowOnCopyAfterArmed 同一约束）。
struct ThrowOnArmedCopy {
    std::shared_ptr<std::atomic<bool>> armed;

    explicit ThrowOnArmedCopy(std::shared_ptr<std::atomic<bool>> flag)
        : armed(std::move(flag)) {}

    ThrowOnArmedCopy(const ThrowOnArmedCopy& other) : armed(other.armed) {
        if (armed && armed->load(std::memory_order_acquire)) {
            throw std::bad_alloc();
        }
    }

    ThrowOnArmedCopy& operator=(const ThrowOnArmedCopy&) = default;

    void operator()() const {}
};

void arm_throwing_copy_task(Task& task, std::shared_ptr<std::atomic<bool>> armed) {
    task.task_id = "cr023";
    task.priority = TaskPriority::NORMAL;
    // 装入 std::function 的这次拷贝是第一次拷贝：armed 未置位，成功。
    task.function = ThrowOnArmedCopy(std::move(armed));
}

} // namespace

TEST(LockfreeWorkerQueueCr023, PushConstRefThrowsAndKeepsQueueIntact) {
    auto armed = std::make_shared<std::atomic<bool>>(false);
    LockFreeWorkerQueue queue(16);

    // 对照：未武装的正常 push 可用。
    Task ok_task;
    ok_task.task_id = "ok";
    ok_task.function = [] {};
    ASSERT_TRUE(queue.push(ok_task));
    ASSERT_EQ(queue.size(), 1U);

    // 先装入（armed=false 的首次拷贝成功），再武装。
    Task explosive;
    arm_throwing_copy_task(explosive, armed);
    armed->store(true, std::memory_order_release);

    // CR-023：copy_task 抛出 → 原样重抛（不得吞掉），节点已归还。
    EXPECT_THROW(queue.push(explosive), std::bad_alloc);

    // 队列状态不变：失败的 push 不入队（无半节点）。
    EXPECT_EQ(queue.size(), 1U);

    // 队列仍可用。
    Task out;
    ASSERT_TRUE(queue.pop(out));
    EXPECT_EQ(out.task_id, "ok");
    EXPECT_TRUE(queue.empty());
}

TEST(LockfreeWorkerQueueCr023, PushRvalueRelocatesHandlerWithoutThrowAndKeepsQueueIntact) {
    // move overload 的 try/catch（CR-023）在 libstdc++ 13 上不可经
    // std::function 注入异常：实测（见 fn_move_probe，2026-10-19）
    // std::function 的移动构造/移动赋值对"仅有 throwing 拷贝"的
    // handler 也按逐字节搬移处理、绝不调用拷贝构造——move_task 的
    // function 移动赋值在此工具链上不可能抛出，该 catch 只对未来的
    // 标准库实现变化兜底。本用例钉死现实行为：右值 push 正常搬移
    // handler、正常入队、队列完好。
    auto armed = std::make_shared<std::atomic<bool>>(false);
    LockFreeWorkerQueue queue(16);

    Task explosive;
    explosive.task_id = "cr023";
    explosive.function = ThrowOnArmedCopy(armed);
    armed->store(true, std::memory_order_release);

    // 不抛（libstdc++ 13 搬移语义），任务完整入队。
    EXPECT_TRUE(queue.push(std::move(explosive)));
    EXPECT_EQ(queue.size(), 1U);

    Task out;
    ASSERT_TRUE(queue.pop(out));
    EXPECT_EQ(out.task_id, "cr023");

    // 队列仍可用。
    Task ok_task;
    ok_task.task_id = "after";
    ok_task.function = [] {};
    EXPECT_TRUE(queue.push(ok_task));
    Task out2;
    ASSERT_TRUE(queue.pop(out2));
    EXPECT_EQ(out2.task_id, "after");
}
