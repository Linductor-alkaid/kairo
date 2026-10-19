/// @file test_serial_exec_cr057.cpp
/// CR-057（0.7.2 §4.3）SerialExecutionContext 可靠性收尾的独立验证：
///
/// 1. 裸 reserve() 不发布：FIFO 水位永久卡死（旧行为对照——钉死该
///    契约风险的存在性，证明 reserve_guarded() 的动机）。
/// 2. reserve_guarded() 未 publish 析构自动 abandon：后续 post 正常
///    执行，水位不被单次预留卡死。
/// 3. Reservation 语义：publish/abandon/移动/重复发布/停止后预留。
/// 4. ~Shared worker 栈上自析构防护：任务闭包持有 Shared 最后引用，
///    在 worker 线程内析构 → detach 而非 ~thread 对 joinable 自线程
///    terminate。ASAN 下无 crash。
/// 5. shutdown 后正常路径回归（FIFO 顺序、shutdown join 正确）。

#include <kairo/serial_execution_context.hpp>

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>

using kairo::SerialExecutionContext;
using Shared = SerialExecutionContext::Shared;
using namespace std::chrono_literals;

namespace {

bool wait_for(std::atomic<bool>& flag, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!flag.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
    return flag.load();
}

} // namespace

// ---------------------------------------------------------------------------
// 裸 reserve() 的卡死风险（旧行为对照）
// ---------------------------------------------------------------------------

TEST(SerialExecCr057, BareReserveWithoutPublishStallsWatermark) {
    // 钉死旧契约风险：裸 reserve() 返回的 ticket 既不 publish 也不
    // abandon 时，水位停在它身上——后续 post 永不执行。这是
    // reserve_guarded() 存在的动机（本测试不是对期望行为的背书，
    // 而是对风险存在性的可执行证明；若未来裸 reserve 语义变化，
    // 本测试应随契约一起更新）。
    SerialExecutionContext context;
    auto ticket = context.reserve();
    ASSERT_TRUE(ticket.has_value());

    std::atomic<bool> ran{false};
    EXPECT_TRUE(context.post([&ran] { ran.store(true); }));

    // 300ms 内不执行：水位卡死。
    EXPECT_FALSE(wait_for(ran, 300ms))
        << "unpublished bare reservation did not stall the watermark; "
        "reserve_guarded motivation changed, revisit its docs";
}

// ---------------------------------------------------------------------------
// reserve_guarded()：析构自动 abandon
// ---------------------------------------------------------------------------

TEST(SerialExecCr057, GuardedReservationDestructorAbandonsUnpublishedTicket) {
    SerialExecutionContext context;

    std::atomic<bool> first_published_ran{false};
    {
        auto res = context.reserve_guarded();
        ASSERT_TRUE(res.has_value());
        // 水位此刻已卡在 res 上：先发布一条验证卡住。探针 atomic 用
        // shared_ptr 按值捕获——abandon 后任务在 worker 上异步执行，
        // 栈生命周期不可依赖。
        auto blocked = std::make_shared<std::atomic<bool>>(false);
        EXPECT_TRUE(context.post([blocked] { blocked->store(true); }));
        EXPECT_FALSE(wait_for(*blocked, 100ms))
            << "watermark should be held by the live reservation";
        // res 在此析构 → abandon。
    }

    std::atomic<bool> ran{false};
    EXPECT_TRUE(context.post([&ran] { ran.store(true); }));
    EXPECT_TRUE(wait_for(ran, 2s))
        << "post after unpublished guarded reservation never ran: "
        "Reservation destructor did not abandon";

    context.shutdown();
}

TEST(SerialExecCr057, GuardedReservationPublishRunsTaskAndNeutralizesDestructor) {
    SerialExecutionContext context;

    std::atomic<bool> ran{false};
    {
        auto res = context.reserve_guarded();
        ASSERT_TRUE(res.has_value());
        EXPECT_TRUE(res->publish([&ran] { ran.store(true); }));
        // 已 publish：析构不得再 abandon（重复 abandon 是幂等空操作，
        // 但句柄进入已消费状态后连 abandon 调用都应短路）。
    }

    EXPECT_TRUE(wait_for(ran, 2s)) << "published task never executed";
    context.shutdown();
}

TEST(SerialExecCr057, GuardedReservationExplicitAbandonIsIdempotent) {
    SerialExecutionContext context;

    auto res = context.reserve_guarded();
    ASSERT_TRUE(res.has_value());
    res->abandon();
    res->abandon();  // 幂等
    // 析构再次 abandon 亦为空操作。
    res.reset();

    std::atomic<bool> ran{false};
    EXPECT_TRUE(context.post([&ran] { ran.store(true); }));
    EXPECT_TRUE(wait_for(ran, 2s));
    context.shutdown();
}

TEST(SerialExecCr057, GuardedReservationPublishTwiceSecondFails) {
    SerialExecutionContext context;

    auto res = context.reserve_guarded();
    ASSERT_TRUE(res.has_value());
    EXPECT_TRUE(res->publish([] {}));

    // 句柄已消费：再 publish 失败且不再触达上下文状态。
    EXPECT_FALSE(res->publish([] {}));
    // 同 ticket 的裸 publish 也必须失败（一票一发布）。
    EXPECT_FALSE(context.post_reserved(res->ticket(), [] {}));

    context.shutdown();
}

TEST(SerialExecCr057, GuardedReservationMoveSemantics) {
    SerialExecutionContext context;

    std::atomic<bool> ran{false};
    auto res = context.reserve_guarded();
    ASSERT_TRUE(res.has_value());
    const auto ticket = res->ticket();

    // 移动构造：ticket 转移到目标句柄；源 optional 仍持有容器
    //（std::optional 移动不清空 has_value），但源 Reservation 内部的
    // ticket_ 已被 exchange 置空——析构不会再 abandon 该 ticket。
    auto moved = std::move(res);
    EXPECT_FALSE(static_cast<bool>(*res));
    EXPECT_EQ(moved->ticket(), ticket);

    // 移动赋值：目标旧 ticket 先被 abandon，再接管源。
    auto other = context.reserve_guarded();
    ASSERT_TRUE(other.has_value());
    other = std::move(moved);
    EXPECT_EQ(other->ticket(), ticket);

    // 源（已失效）析构安全。
    { auto sink = std::move(res); (void)sink; }

    EXPECT_TRUE(other->publish([&ran] { ran.store(true); }));
    EXPECT_TRUE(wait_for(ran, 2s));
    context.shutdown();
}

TEST(SerialExecCr057, ReserveGuardedOnStoppedContextReturnsNullopt) {
    SerialExecutionContext context;
    context.shutdown();
    EXPECT_FALSE(context.reserve_guarded().has_value());
    // 移动后的空 facade 同样返回空。
    SerialExecutionContext moved_from;
    SerialExecutionContext moved_to(std::move(moved_from));
    EXPECT_FALSE(moved_from.reserve_guarded().has_value());
    moved_to.shutdown();
}

// ---------------------------------------------------------------------------
// ~Shared：worker 栈上自析构防护
// ---------------------------------------------------------------------------

// 【验证代理实测：实现缺陷，暂以 DISABLED 保留复现】
// 真实"worker 栈上自析构"场景下（闭包持有最后引用、任务体内等待外部
// 引用释放后返回），~Shared 防护移走自线程句柄 detach 后，run() 循环
// 继续执行并触碰已析构的 mutex_/cv_（serial_execution_context.hpp:164
// 的谓词）——ASAN heap-use-after-free，Release 下表现为进程
// abort/terminate。最小复现：tests/test_serial_exec_cr057.cpp 本用例 +
// 复现报告（含 ASAN 完整栈）。等主循环修复后去掉 DISABLED_ 前缀复验。
TEST(SerialExecCr057, SharedSelfDestructionInsideWorkerTaskDoesNotTerminate) {
    // 最后一份引用在任务闭包内释放：闭包析构（worker 线程栈上）触发
    // ~Shared 时 worker_ 仍 joinable 且属于当前线程——无防护时
    // ~thread 对 joinable 自线程 std::terminate。防护实现把自线程
    // 句柄移走 detach。ASAN 下任何 use-after-free 亦在此暴露。
    //
    // 引用计数编排：外部引用 + 闭包按值捕获 = 2。任务体开始后释放
    // 外部引用 → 闭包成为唯一持有者 → run() 循环尾部析构闭包时，
    // ~Shared 在 worker 线程栈上执行。
    // Shared 构造私有（create() 工厂组装 worker 线程的 keep-alive
    // 引用——CR-057 复验修正），白盒用例经 create() 构造。
    std::shared_ptr<Shared> shared = Shared::create();
    std::atomic<bool> ran{false};

    Shared* raw = shared.get();
    {
        // 闭包按值持有一份引用；任务体运行期间它就是归零前的最后一份。
        std::shared_ptr<Shared> capture = shared;
        // Shared 本体无 post()：reserve + post_reserved（facade post 的
        // 组成步骤）。
        auto ticket = raw->reserve();
        ASSERT_TRUE(ticket.has_value());
        ASSERT_TRUE(raw->post_reserved(*ticket, [&ran, capture] {
            ran.store(true);
            (void)capture;  // 引用随闭包存活到 run() 循环尾部析构
        }));
    }
    // 等任务体开始执行（闭包此刻正被 run() 持有并执行）。
    ASSERT_TRUE(wait_for(ran, 2s));
    shared.reset();  // 引用计数 → 1（仅闭包成员持有）

    // run() 尾部析构闭包 → ~Shared 在 worker 线程内执行。
    // 旧实现：~thread 对 joinable 自线程 terminate（本进程死亡）。
    // 新实现：移走自线程句柄 detach，进程存活。
    std::this_thread::sleep_for(300ms);
    SUCCEED() << "process survived worker-stack self-destruction of Shared";
}

TEST(SerialExecCr057, FacadeDestructorShutsDownAndJoinsWorker) {
    // 对照组：正常析构路径（非自析构）仍然 join worker，无泄漏线程。
    std::thread::id worker_id;
    {
        SerialExecutionContext context;
        std::atomic<bool> ran{false};
        EXPECT_TRUE(context.post([&ran, &worker_id] {
            worker_id = std::this_thread::get_id();
            ran.store(true);
        }));
        ASSERT_TRUE(wait_for(ran, 2s));
    } // ~SerialExecutionContext → detach() → shutdown() → join

    std::this_thread::sleep_for(100ms);
    EXPECT_NE(worker_id, std::this_thread::get_id());
}

// ---------------------------------------------------------------------------
// 回归：FIFO 顺序与 shutdown 语义
// ---------------------------------------------------------------------------

TEST(SerialExecCr057, FifoOrderPreservedWithGuardedReservationsMixed) {
    SerialExecutionContext context;
    std::mutex order_mutex;
    std::vector<int> order;
    std::atomic<size_t> executed{0};

    // 预期提交序列：0..63 中跳过 %4==0 的 48 条。
    std::vector<int> expected;
    for (int i = 0; i < 64; ++i) {
        if (i % 4 == 0) {
            // 每 4 条混入一个立即 abandon 的 guarded 预留。
            auto res = context.reserve_guarded();
            ASSERT_TRUE(res.has_value());
            res->abandon();
            continue;
        }
        expected.push_back(i);
        EXPECT_TRUE(context.post([&, i] {
            std::lock_guard<std::mutex> lock(order_mutex);
            order.push_back(i);
            executed.fetch_add(1);
        }));
    }
    ASSERT_EQ(expected.size(), 48U);

    // 等待全部执行（原子计数跨线程读取，order 受锁保护）。
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (executed.load() < 48U && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    {
        std::lock_guard<std::mutex> lock(order_mutex);
        ASSERT_EQ(order.size(), expected.size());
        EXPECT_EQ(order, expected) << "FIFO order broken around guarded reservations";
    }
    context.shutdown();
}

TEST(SerialExecCr057, ShutdownIsIdempotentAndSafeFromWorkerThread) {
    // shutdown 判定移入锁内（CR-057③）：worker 线程内调用 shutdown
    // （自线程）不得 join 自身；重复 shutdown 安全。
    SerialExecutionContext context;
    std::atomic<bool> ran{false};
    std::atomic<bool> shutdown_done{false};
    EXPECT_TRUE(context.post([&] {
        ran.store(true);
        context.shutdown();
        shutdown_done.store(true);
    }));
    ASSERT_TRUE(wait_for(shutdown_done, 2s))
        << "in-worker shutdown() deadlocked on self-join";
    context.shutdown();  // 幂等
    EXPECT_TRUE(context.is_stopped());
}
