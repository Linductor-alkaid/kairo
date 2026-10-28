// 并发契约测试（docs/design/subject_bucket_admission.md「## 验收」9–10 与
// D-2「并发一致性契约」/ D-5「代际语义」；issue #222 / PR #223）：
// 配额热更新的严格可见性与弱一致、活键原位缩放、解除即刻生效、
// 解除-重建的代际隔离与孤儿计数不可见、并发交错的记账无泄漏。
//
// 块编号 // 验收 9x / // 验收 10x 对应设计文档「## 验收」第 9/10 条的
// 分解场景；// 契约 A / // 契约 B 对应 D-2 末尾与 D-5 的并发契约。
//
// TSAN 约束（CI thread-sanitizer job 的成员，见 .github/workflows/
// c-cpp.yml 对 test_deadline_policy 的排除注释）：任务体不得抛异常、
// 不得使用取消路径（排队取消会跨线程结算 TaskCancelled 异常，命中
// GCC libstdc++ 未插桩的 exception_ptr TSAN 误报）。分桶拒绝的 future
// 在提交线程上同步结算（同一异常对象同线程构造与消费），允许使用；
// 接纳任务体只做普通计算 + 原子计数。
//
// 风格与 test_subject_bucket_admission.cpp 一致：standalone 单 main，
// ADM_CHECK 显式求值（Release(-DNDEBUG) 下 assert 会被剥离），期望就绪
// 的 future 用 settled_soon 有界等待，每块 ex.shutdown()。
//
// 验收 1–8 的功能语义与零回归锚由 test_subject_bucket_admission.cpp
// （验收 8 显式引用 test_bounded_admission 最小场景）覆盖，此处不重复。
#include <kairo/executor.hpp>
#include <kairo/task_options.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

#define ADM_CHECK(cond)                                                       \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::fprintf(stderr,                                              \
                         "test_subject_quota_concurrency: FAILED %s (%s:%d)\n", \
                         #cond, __FILE__, __LINE__);                          \
            std::exit(1);                                                     \
        }                                                                     \
    } while (0)

// 期望立即就绪的 future 用有界等待：即使实现回归也不把 CI 挂满 120s。
template <typename Future>
bool settled_soon(Future& future, std::chrono::seconds budget) {
    return future.wait_for(budget) == std::future_status::ready;
}

// 取出 future 的拒绝消息：期望 CapacityExhaustedException；其他终态以
// 可读文本返回（后续 find 断言必然失败并携带实际消息，便于定位）。
// 分桶拒绝的异常在提交线程上构造并结算，此处同线程取出（无跨线程
// exception_ptr，满足 TSAN 约束）。
template <typename Future>
std::string capacity_rejection_message(Future& future) {
    try {
        (void)future.get();
    } catch (const kairo::CapacityExhaustedException& exception) {
        return exception.what();
    } catch (const std::exception& other) {
        return std::string("wrong exception: ") + other.what();
    }
    return "<future did not throw>";
}

}  // namespace

int main() {
    using kairo::Executor;
    using kairo::ExecutorConfig;

    // 验收 9a（契约 A 严格可见性，确定性）：份额 4 占满后 set(A,1) 返回，
    // 其后开始的提交必须按新份额判定——第 5 个带键提交被拒且消息含
    // subject_max_in_flight=1；全部终态后计数归零（验收 9 无泄漏半句）。
    {
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        // 全局上限不启用（默认 0）：只验证分桶维度。
        ADM_CHECK(ex.initialize(config));
        ex.set_subject_max_in_flight_tasks("A", 4);

        // 4 个阻塞任务占满份额（admission 计排队 + 运行中，1 个 worker
        // 足以构造；阻塞经 promise 释放，任务体不抛异常）。
        std::vector<std::promise<void>> gates(4);
        std::vector<std::future<void>> blocked;
        for (int i = 0; i < 4; ++i) {
            auto wait = gates[static_cast<size_t>(i)].get_future().share();
            blocked.push_back(ex.submit_auto(
                kairo::task([wait] { wait.wait(); }).subject("A")));
        }
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 4);

        // set(A,1) 完成后开始的提交：注册表互斥量的 happens-before 保证
        // 该接纳取得新快照 → 必须被拒。
        ex.set_subject_max_in_flight_tasks("A", 1);
        ADM_CHECK(ex.get_subject_max_in_flight_tasks("A") == 1);
        auto overflow = ex.submit_auto(kairo::task([] {
                                           ADM_CHECK(false &&
                                                     "rejected task must not run");
                                       }).subject("A"));
        ADM_CHECK(settled_soon(overflow, std::chrono::seconds(2)));
        const std::string message = capacity_rejection_message(overflow);
        ADM_CHECK(message.find("key=A") != std::string::npos);
        ADM_CHECK(message.find("subject_max_in_flight=1") != std::string::npos);
        // 拒绝不改变在途计数。
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 4);

        // 全部终态后归零。
        for (auto& gate : gates) {
            gate.set_value();
        }
        for (auto& future : blocked) {
            ADM_CHECK(settled_soon(future, std::chrono::seconds(2)));
            future.get();
        }
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 0);
        ex.shutdown();
    }

    // 验收 9b（D-5 活键原位缩放，同代）：N→M 调大保留既有计数（不重置），
    // 调小不驱逐已接纳任务、只约束后续提交；全部终态后归零且可再接纳。
    {
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        ADM_CHECK(ex.initialize(config));
        ex.set_subject_max_in_flight_tasks("A", 2);

        // 1 个阻塞在途（同代计数 1）。
        std::promise<void> gate1;
        auto wait1 = gate1.get_future().share();
        auto b1 = ex.submit_auto(
            kairo::task([wait1] { wait1.wait(); }).subject("A"));
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 1);

        // set(A,5) 调大：同代计数保留，第 2、3 个可接纳（计数 3，未重置）。
        ex.set_subject_max_in_flight_tasks("A", 5);
        ADM_CHECK(ex.get_subject_max_in_flight_tasks("A") == 5);
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 1);
        std::promise<void> gate2;
        std::promise<void> gate3;
        auto wait2 = gate2.get_future().share();
        auto wait3 = gate3.get_future().share();
        auto b2 = ex.submit_auto(
            kairo::task([wait2] { wait2.wait(); }).subject("A"));
        auto b3 = ex.submit_auto(
            kairo::task([wait3] { wait3.wait(); }).subject("A"));
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 3);

        // set(A,1) 调小：第 4 个被拒；已接纳的 3 个不被驱逐。
        ex.set_subject_max_in_flight_tasks("A", 1);
        ADM_CHECK(ex.get_subject_max_in_flight_tasks("A") == 1);
        auto b4 = ex.submit_auto(kairo::task([] {
                                     ADM_CHECK(false &&
                                               "rejected task must not run");
                                 }).subject("A"));
        ADM_CHECK(settled_soon(b4, std::chrono::seconds(2)));
        const std::string b4_message = capacity_rejection_message(b4);
        ADM_CHECK(b4_message.find("subject_max_in_flight=1") !=
                  std::string::npos);
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 3);

        // 全部释放后归零，且同份额可立即再接纳。
        gate1.set_value();
        gate2.set_value();
        gate3.set_value();
        for (std::future<void>* future : {&b1, &b2, &b3}) {
            ADM_CHECK(settled_soon(*future, std::chrono::seconds(2)));
            future->get();
        }
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 0);
        ADM_CHECK(ex.submit_auto(kairo::task([] { return 42; }).subject("A"))
                      .get() == 42);
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 0);
        ex.shutdown();
    }

    // 验收 10a（契约 B 代际隔离，确定性）：set(A,0) 解除 → 查询不可见；
    // set(A,4) 重建 = 新代从 0 起计（不继承旧代残量）；共存窗口内实际
    // 并发在途 = 旧代残量 4 + 新代份额 4 = 8，查询只反映当代 4；旧代
    // 终态释放不影响新代计数；全部终态后归零无泄漏。
    {
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        ADM_CHECK(ex.initialize(config));
        ex.set_subject_max_in_flight_tasks("A", 4);

        // gen1：4 个阻塞任务占满（全部接纳；阻塞释放逐索引可控）。
        std::vector<std::promise<void>> gen1_gates(4);
        std::vector<std::future<void>> gen1_futures;
        for (int i = 0; i < 4; ++i) {
            auto wait = gen1_gates[static_cast<size_t>(i)].get_future().share();
            gen1_futures.push_back(ex.submit_auto(
                kairo::task([wait] { wait.wait(); }).subject("A")));
        }
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 4);

        // set(A,0) 解除：即刻 erase，份额查询 0、在途查询 0、快照无 A。
        ex.set_subject_max_in_flight_tasks("A", 0);
        ADM_CHECK(ex.get_subject_max_in_flight_tasks("A") == 0);
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 0);
        ADM_CHECK(ex.get_subject_quota_status().empty());

        // set(A,4) 重建 = 新代从 0 起计：再接纳 4 个 gen2 阻塞任务。
        ex.set_subject_max_in_flight_tasks("A", 4);
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 0);
        std::vector<std::promise<void>> gen2_gates(4);
        std::vector<std::future<void>> gen2_futures;
        for (int i = 0; i < 4; ++i) {
            auto wait = gen2_gates[static_cast<size_t>(i)].get_future().share();
            gen2_futures.push_back(ex.submit_auto(
                kairo::task([wait] { wait.wait(); }).subject("A")));
        }
        // 共存窗口：gen1 残量 4（未释放）+ 新代 4 = 8 个在途任务并存；
        // 查询只反映当代 4。
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 4);
        {
            const auto snapshot = ex.get_subject_quota_status();
            ADM_CHECK(snapshot.size() == 1);
            ADM_CHECK(snapshot[0].subject == "A");
            ADM_CHECK(snapshot[0].max_in_flight_tasks == 4);
            ADM_CHECK(snapshot[0].in_flight_submissions == 4);
        }

        // 释放 2 个 gen1：孤儿释放对当代不可见，查询仍 4。
        gen1_gates[0].set_value();
        gen1_gates[1].set_value();
        ADM_CHECK(settled_soon(gen1_futures[0], std::chrono::seconds(2)));
        ADM_CHECK(settled_soon(gen1_futures[1], std::chrono::seconds(2)));
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 4);

        // 释放其余 gen1（单 worker 池为 FIFO 排队：gen2 排在全部 gen1
        // 之后，必须先清空 gen1 才能轮到 gen2 运行；此时查询仍 4）。
        gen1_gates[2].set_value();
        gen1_gates[3].set_value();
        ADM_CHECK(settled_soon(gen1_futures[2], std::chrono::seconds(2)));
        ADM_CHECK(settled_soon(gen1_futures[3], std::chrono::seconds(2)));
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 4);

        // 释放 2 个 gen2：当代计数 4 → 2。
        gen2_gates[0].set_value();
        gen2_gates[1].set_value();
        ADM_CHECK(settled_soon(gen2_futures[0], std::chrono::seconds(2)));
        ADM_CHECK(settled_soon(gen2_futures[1], std::chrono::seconds(2)));
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 2);

        // 全部释放 → 归零无泄漏。
        gen2_gates[2].set_value();
        gen2_gates[3].set_value();
        for (auto& future : gen2_futures) {
            ADM_CHECK(settled_soon(future, std::chrono::seconds(2)));
            future.get();
        }
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 0);
        ex.shutdown();
    }

    // 验收 10b（D-5 解除即刻生效）：set(A,0) 后不释放阻塞任务，新提交
    // 必须被接纳（旧份额 1 已满，若解除未生效则必然被拒）；新任务终态后
    // 份额查询为 0。
    {
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 2;
        config.max_threads = 2;
        ADM_CHECK(ex.initialize(config));
        ex.set_subject_max_in_flight_tasks("A", 1);

        std::promise<void> release;
        auto release_future = release.get_future().share();
        std::promise<void> started;
        auto started_future = started.get_future();
        auto blocker = ex.submit_auto(kairo::task([&] {
            started.set_value();
            release_future.wait();
        }).subject("A"));
        started_future.wait();
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 1);

        // 解除：即刻生效（不释放阻塞任务）。
        ex.set_subject_max_in_flight_tasks("A", 0);
        ADM_CHECK(ex.get_subject_quota_status().empty());

        // 新提交必须被接纳：空闲 worker 上立即执行完成。
        auto accepted_after = ex.submit_auto(
            kairo::task([] { return 7; }).subject("A"));
        ADM_CHECK(settled_soon(accepted_after, std::chrono::seconds(2)));
        ADM_CHECK(accepted_after.get() == 7);
        // 键已解除：份额与在途查询均 0，无容量类拒绝。
        ADM_CHECK(ex.get_subject_max_in_flight_tasks("A") == 0);
        ADM_CHECK(ex.get_subject_in_flight_submissions("A") == 0);
        const auto failure_status = ex.get_failure_status();
        ADM_CHECK(failure_status.capacity_exhausted_count == 0);
        ADM_CHECK(failure_status.subject_quota_exhausted_count == 0);

        release.set_value();
        ADM_CHECK(settled_soon(blocker, std::chrono::seconds(2)));
        blocker.get();
        ex.shutdown();
    }

    // 契约 A（弱一致）+ 契约 B（代际）并发交错 stress：单 Executor
    //（worker 2），全局 max_in_flight_tasks 保持 0（隔离分桶维度）。
    // 2 个提交线程 × 各 1000 次带键提交，1 个控制线程循环热更新份额
    // {2,1,4,0,3}（每档 ~1ms，循环至提交线程 join 后停止）。断言：全部 future 有界终态且分类正确；接纳数 == 任务体
    // 运行数（无越权执行）；终态后无泄漏；失败计数通道归属正确；收尾
    // 解除后快照为空。
    {
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 2;
        config.max_threads = 2;
        ADM_CHECK(ex.initialize(config));

        std::atomic<int> ran{0};
        std::atomic<int> accepted{0};
        std::atomic<int> rejected{0};
        std::atomic<bool> stop_control{false};

        // 控制线程：循环热更新份额，直至提交线程 join 后停止。
        std::thread controller([&] {
            const size_t shares[] = {2, 1, 4, 0, 3};
            while (!stop_control.load(std::memory_order_acquire)) {
                for (const size_t share : shares) {
                    if (stop_control.load(std::memory_order_acquire)) {
                        return;
                    }
                    ex.set_subject_max_in_flight_tasks("S", share);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        });

        // 提交线程：轻量任务体（原子计数，不抛异常、不取消）；拒绝的
        // future 在本线程同步结算（同线程消费异常，TSAN 安全）。每次提交
        // 都等待自身 future 终态（流水深度 ≤ 2），400/2000 总量的任务在
        // 数毫秒内排空——迭代次数取 1000/线程，保证控制线程能完整轮转
        // {2,1,4,0,3} 数圈（含 set(0) 解除后再重配的代际切换）。
        const auto submit_work = [&] {
            for (int i = 0; i < 1000; ++i) {
                auto future = ex.submit_auto(kairo::task([&ran] {
                    ran.fetch_add(1, std::memory_order_relaxed);
                }).subject("S"));
                // 拒绝即时结算；接纳由 worker 快速执行——2s 内必须终态。
                ADM_CHECK(settled_soon(future, std::chrono::seconds(2)));
                try {
                    (void)future.get();
                    accepted.fetch_add(1, std::memory_order_relaxed);
                } catch (const kairo::CapacityExhaustedException&) {
                    rejected.fetch_add(1, std::memory_order_relaxed);
                }
            }
        };
        std::thread submitter1(submit_work);
        std::thread submitter2(submit_work);
        submitter1.join();
        submitter2.join();
        stop_control.store(true, std::memory_order_release);
        controller.join();

        // (b) 无越权执行：任务体运行数 == 接纳数（被拒任务体从未运行）。
        ADM_CHECK(ran.load() == accepted.load());
        // (c) 无泄漏：全部终态后当代计数归零（键若恰被 set(0) 解除，
        // 查询同样返回 0）。
        ADM_CHECK(ex.get_subject_in_flight_submissions("S") == 0);
        // (d) 全局维度未启用：全部拒绝都是分桶拒绝——测试计数与
        // subject_quota_exhausted_count 一致，且等于 capacity_exhausted_count
        //（前者是后者子集）；分桶拒绝不进 submit_rejected 通道。
        const auto failure_status = ex.get_failure_status();
        ADM_CHECK(failure_status.subject_quota_exhausted_count ==
                  static_cast<uint64_t>(rejected.load()));
        ADM_CHECK(failure_status.capacity_exhausted_count ==
                  failure_status.subject_quota_exhausted_count);
        ADM_CHECK(failure_status.submit_rejected_count == 0);
        // (e) 收尾解除：快照为空。
        ex.set_subject_max_in_flight_tasks("S", 0);
        ADM_CHECK(ex.get_subject_quota_status().empty());
        ex.shutdown();
    }

    // 回归锚（验收 8）：无任何份额配置 + 不带键时行为不变，由
    // test_subject_bucket_admission.cpp 验收 8 块（引用
    // test_bounded_admission 最小容量场景）与本文件的零配置默认初始化
    // 共同覆盖；分桶维度关闭时不触碰注册表锁的零开销路径属性能语义，
    // 由 benchmark 套件观测，不在并发契约测试范围内。

    std::printf(
        "test_subject_quota_concurrency: all contract blocks passed\n");
    return 0;
}
