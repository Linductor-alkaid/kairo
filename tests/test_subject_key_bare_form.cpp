// 裸形态带键入口（SubjectKey）+ admitted_total 诊断计数测试
//（docs/design/subject_bucket_admission.md「## 修订」R-1/R-2/R-3 与
// D-2/D-5，issue #222 评论 A 项）：
//   1. submit_auto(kairo::subject(key), lambda) 接纳计入正确分桶，拒绝
//      语义与 TaskBuilder 形态一致（CapacityExhaustedException 消息含
//      key、subject_quota_exhausted_count 递增、事件携带 subject）；
//   2. 与 TaskBuilder 形态（task(fn).subject(key)）共享同一计数；
//   3. 裸形态（带键 / 不带键）路由决策行为一致；
//   4. SubjectQuotaStatus::admitted_total 语义（累计接纳、拒绝不计、
//      代际从 0 起计、未配置键不可见）；
//   5. SubjectKey explicit 消歧安全（编译期 static_assert）；
//   6. 零回归锚：不带键的 submit_auto / submit 行为不变。
//
// 风格与 test_subject_bucket_admission.cpp 一致：standalone 单 main，
// KEY_CHECK 显式求值（Release(-DNDEBUG) 下 assert 会被剥离，ex.initialize()
// 将不执行、facade 懒初始化为默认配置，测试既空转也可能死锁），期望就绪
// 的 future 用 settled_soon 有界等待，每块 ex.shutdown()。
//
// TSAN 约束（本文件加入 CI thread-sanitizer job 白名单，见
// .github/workflows/c-cpp.yml 对 test_deadline_policy 的排除注释）：
// 接纳任务体不抛异常、不使用取消路径（跨线程结算 TaskCancelled 异常会
// 命中 GCC libstdc++ 未插桩的 exception_ptr TSAN 误报）；被拒任务体不做
// 副作用断言（以原子计数事后核对），分桶拒绝的 future 在提交线程上同步
// 结算（同一异常对象同线程构造与消费），允许使用。
#include <kairo/executor.hpp>
#include <kairo/task_options.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

#define KEY_CHECK(cond)                                                          \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr,                                                 \
                         "test_subject_key_bare_form: FAILED %s (%s:%d)\n",      \
                         #cond, __FILE__, __LINE__);                             \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

// 期望立即就绪的 future 用有界等待：即使实现回归也不把 CI 挂满 120s。
template <typename Future>
bool settled_soon(Future& future, std::chrono::seconds budget) {
    return future.wait_for(budget) == std::future_status::ready;
}

// 取出 future 的拒绝消息：期望 CapacityExhaustedException；其他终态以
// 可读文本返回（后续 find 断言必然失败并携带实际消息，便于定位）。分桶
// 拒绝的异常在提交线程上构造并结算，此处同线程取出（无跨线程
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

// ---- 场景 5（编译期，消歧安全）：SubjectKey explicit ----
// explicit 构造阻断 const char* → SubjectKey 的隐式转换：
// is_convertible == false。
static_assert(!std::is_convertible<const char*, kairo::SubjectKey>::value,
              "SubjectKey must not be implicitly convertible from const char*");
static_assert(!std::is_convertible<std::string, kairo::SubjectKey>::value,
              "SubjectKey must not be implicitly convertible from std::string");
// 显式构造仍然可行：is_constructible == true。
static_assert(std::is_constructible<kairo::SubjectKey, const char*>::value,
              "SubjectKey must be explicitly constructible from const char*");
static_assert(std::is_constructible<kairo::SubjectKey, std::string>::value,
              "SubjectKey must be explicitly constructible from std::string");
// subject() 工厂返回 SubjectKey。
static_assert(std::is_same<decltype(kairo::subject(std::string("k"))),
                           kairo::SubjectKey>::value,
              "kairo::subject() must return kairo::SubjectKey");

// 注：`ex.submit_auto("lit", [] {})` 无法在本文件内断言（standalone 测试
// 不能编译失败自身），负编译由验证流程以独立 TU 单独确认。依据：explicit
// 使 (SubjectKey, F&&, Args&&...) 重载对 const char[4] 首参不可行（无隐式
// 转换），泛型重载 (F&&, Args&&...) 虽对首参精确匹配，但返回类型
// invoke_result<const char*, lambda>::type 代换失败被 SFINAE 移除，
// TaskBuilder/CpuGpuTask 重载不 viable → 无匹配重载，编译错误。

}  // namespace

int main() {
    using kairo::CapacityExhaustedException;
    using kairo::Executor;
    using kairo::ExecutorConfig;

    // 场景 1：裸形态接纳计入正确桶——A 份额 2 占满后第 3 个被拒
    //（消息含 key=tenant-A），failure 计数与事件携带分桶键，B 键不受影响。
    {
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        // 全局上限不启用（默认 0）：只验证分桶维度。
        KEY_CHECK(ex.initialize(config));

        ex.set_subject_max_in_flight_tasks("tenant-A", 2);
        ex.set_subject_max_in_flight_tasks("tenant-B", 2);

        std::promise<void> release;
        auto release_future = release.get_future().share();
        std::promise<void> started;
        auto started_future = started.get_future();

        // 裸形态带键入口：两个阻塞任务占满 A 份额（1 运行 + 1 排队）。
        std::vector<std::future<void>> a_blocked;
        a_blocked.push_back(ex.submit_auto(kairo::subject("tenant-A"), [&] {
            started.set_value();
            release_future.wait();
        }));
        a_blocked.push_back(ex.submit_auto(
            kairo::subject("tenant-A"), [&] { release_future.wait(); }));
        started_future.wait();
        KEY_CHECK(ex.get_subject_in_flight_submissions("tenant-A") == 2);

        // A 第 3 个（裸形态）：分桶拒绝，future 立即就绪，消息可区分。
        std::atomic<int> a3_ran{0};
        auto a3 = ex.submit_auto(kairo::subject("tenant-A"),
                                 [&] { a3_ran.fetch_add(1); });
        KEY_CHECK(settled_soon(a3, std::chrono::seconds(2)));
        const std::string a3_message = capacity_rejection_message(a3);
        KEY_CHECK(a3_message.find("key=tenant-A") != std::string::npos);
        KEY_CHECK(a3_message.find("subject_max_in_flight=2") !=
                  std::string::npos);
        // 拒绝不改变在途计数，被拒任务体未运行。
        KEY_CHECK(ex.get_subject_in_flight_submissions("tenant-A") == 2);
        KEY_CHECK(a3_ran.load() == 0);

        // B 键不受 A 影响：份额 2 内继续全部接纳。
        std::vector<std::future<void>> b_blocked;
        for (int i = 0; i < 2; ++i) {
            b_blocked.push_back(ex.submit_auto(
                kairo::subject("tenant-B"),
                [&] { release_future.wait(); }));
        }
        KEY_CHECK(ex.get_subject_in_flight_submissions("tenant-B") == 2);

        // failure 可观测：分桶拒绝同时递增两个计数（前者是后者子集），
        // recent 事件携带分桶键。
        const auto failure_status = ex.get_failure_status();
        KEY_CHECK(failure_status.subject_quota_exhausted_count == 1);
        KEY_CHECK(failure_status.capacity_exhausted_count == 1);
        const auto recent = ex.get_recent_failures(1);
        KEY_CHECK(recent.size() == 1);
        KEY_CHECK(recent.front().kind == kairo::FailureKind::CapacityExhausted);
        KEY_CHECK(recent.front().subject == "tenant-A");

        release.set_value();
        for (auto& future : a_blocked) {
            KEY_CHECK(settled_soon(future, std::chrono::seconds(2)));
            future.get();
        }
        for (auto& future : b_blocked) {
            KEY_CHECK(settled_soon(future, std::chrono::seconds(2)));
            future.get();
        }
        KEY_CHECK(ex.get_subject_in_flight_submissions("tenant-A") == 0);
        KEY_CHECK(ex.get_subject_in_flight_submissions("tenant-B") == 0);
        // 交叉核对：各桶累计接纳 = 各自接纳数（拒绝不计，场景 4 细化）。
        for (const auto& status : ex.get_subject_quota_status()) {
            if (status.subject == "tenant-A") {
                KEY_CHECK(status.admitted_total == 2);
            } else if (status.subject == "tenant-B") {
                KEY_CHECK(status.admitted_total == 2);
            }
        }
        ex.shutdown();
    }

    // 场景 2：与 TaskBuilder 形态等价——同一键混用两种形态共享同一计数；
    // 各占 1 个阻塞任务后，两种形态的第 3 个提交都被拒。
    {
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        KEY_CHECK(ex.initialize(config));
        ex.set_subject_max_in_flight_tasks("A", 2);

        std::promise<void> release;
        auto release_future = release.get_future().share();
        std::promise<void> started;
        auto started_future = started.get_future();

        // 裸形态 1 个 + TaskBuilder 形态 1 个：共享同一桶，合计占满份额 2。
        auto bare = ex.submit_auto(kairo::subject("A"), [&] {
            started.set_value();
            release_future.wait();
        });
        auto builder = ex.submit_auto(
            kairo::task([&] { release_future.wait(); }).subject("A"));
        started_future.wait();
        KEY_CHECK(ex.get_subject_in_flight_submissions("A") == 2);

        // 第 3 个（裸形态）与第 4 个（TaskBuilder 形态）：都被拒且消息一致
        //（同一接纳点、同一拒绝面）。
        auto bare3 = ex.submit_auto(kairo::subject("A"), [] {});
        KEY_CHECK(settled_soon(bare3, std::chrono::seconds(2)));
        const std::string bare3_message = capacity_rejection_message(bare3);
        KEY_CHECK(bare3_message.find("key=A") != std::string::npos);
        KEY_CHECK(bare3_message.find("subject_max_in_flight=2") !=
                  std::string::npos);

        auto builder3 = ex.submit_auto(kairo::task([] {}).subject("A"));
        KEY_CHECK(settled_soon(builder3, std::chrono::seconds(2)));
        const std::string builder3_message =
            capacity_rejection_message(builder3);
        KEY_CHECK(builder3_message.find("key=A") != std::string::npos);
        KEY_CHECK(builder3_message.find("subject_max_in_flight=2") !=
                  std::string::npos);

        KEY_CHECK(ex.get_failure_status().subject_quota_exhausted_count == 2);
        // 拒绝后两种形态在释放份额前仍不可接纳（计数共享）。
        KEY_CHECK(ex.get_subject_in_flight_submissions("A") == 2);

        release.set_value();
        KEY_CHECK(settled_soon(bare, std::chrono::seconds(2)));
        KEY_CHECK(settled_soon(builder, std::chrono::seconds(2)));
        bare.get();
        builder.get();
        KEY_CHECK(ex.get_subject_in_flight_submissions("A") == 0);
        ex.shutdown();
    }

    // 场景 3：路由决策行为一致——裸形态带键与不带键产出的 RoutingDecision
    //（status/reason/backend/任务名/推断注解）逐字段一致，accepted 指标
    // 增量一致，提交成功且 future 正常结算。
    {
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        KEY_CHECK(ex.initialize(config));

        const auto metrics_before = ex.get_scheduling_metrics();

        // 不带键裸形态：清空决策缓冲后提交，future 正常结算。
        ex.clear_recent_routing_decisions();
        auto unkeyed = ex.submit_auto([] { return 11; });
        KEY_CHECK(settled_soon(unkeyed, std::chrono::seconds(2)));
        KEY_CHECK(unkeyed.get() == 11);
        const auto unkeyed_decisions = ex.get_recent_routing_decisions();
        KEY_CHECK(unkeyed_decisions.size() == 1);
        const auto& unkeyed_decision = unkeyed_decisions.front();
        KEY_CHECK(unkeyed_decision.status == kairo::RoutingStatus::Accepted);
        KEY_CHECK(unkeyed_decision.reason == kairo::RoutingReason::DefaultPolicy);
        KEY_CHECK(unkeyed_decision.selected_backend ==
                  kairo::ExecutionBackend::DefaultAsync);

        // 带键裸形态：同一决策形态（同一 submit_auto_impl 路径）。
        ex.clear_recent_routing_decisions();
        auto keyed =
            ex.submit_auto(kairo::subject("route-probe"), [] { return 22; });
        KEY_CHECK(settled_soon(keyed, std::chrono::seconds(2)));
        KEY_CHECK(keyed.get() == 22);
        const auto keyed_decisions = ex.get_recent_routing_decisions();
        KEY_CHECK(keyed_decisions.size() == 1);
        const auto& keyed_decision = keyed_decisions.front();
        KEY_CHECK(keyed_decision.status == unkeyed_decision.status);
        KEY_CHECK(keyed_decision.reason == unkeyed_decision.reason);
        KEY_CHECK(keyed_decision.selected_backend ==
                  unkeyed_decision.selected_backend);
        KEY_CHECK(keyed_decision.selected_executor_name ==
                  unkeyed_decision.selected_executor_name);
        KEY_CHECK(keyed_decision.task_name == unkeyed_decision.task_name);
        KEY_CHECK(keyed_decision.diagnostics == unkeyed_decision.diagnostics);
        KEY_CHECK(keyed_decision.inferred_reason ==
                  unkeyed_decision.inferred_reason);

        // accepted 指标增量：两次提交各 +1（键不影响路由决策分类），
        // 无拒绝类增量。
        const auto metrics_after = ex.get_scheduling_metrics();
        KEY_CHECK(metrics_after.accepted_count - metrics_before.accepted_count ==
                  2);
        KEY_CHECK(metrics_after.rejected_count == metrics_before.rejected_count);
        KEY_CHECK(metrics_after.capacity_rejected_count ==
                  metrics_before.capacity_rejected_count);
        ex.shutdown();
    }

    // 场景 4：admitted_total——(a) 连续快速完成的不阻塞任务全部 settle 后
    // 累计 == N；(b) 被拒提交不计入；(c) set(key,0) 解除后重建 → 新代从 0
    // 重新累计；(d) 未配置键不出现在快照中。
    {
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 2;
        config.max_threads = 2;
        KEY_CHECK(ex.initialize(config));
        ex.set_subject_max_in_flight_tasks("A", 4);

        // (a) 4 个快速任务（份额 4 内全部接纳），全部 settle 后累计 == 4。
        std::vector<std::future<int>> fast;
        for (int i = 0; i < 4; ++i) {
            fast.push_back(
                ex.submit_auto(kairo::subject("A"), [i] { return i; }));
        }
        for (int i = 0; i < 4; ++i) {
            KEY_CHECK(settled_soon(fast[static_cast<size_t>(i)],
                                   std::chrono::seconds(2)));
            KEY_CHECK(fast[static_cast<size_t>(i)].get() == i);
        }
        {
            const auto snapshot = ex.get_subject_quota_status();
            KEY_CHECK(snapshot.size() == 1);
            KEY_CHECK(snapshot[0].subject == "A");
            KEY_CHECK(snapshot[0].admitted_total == 4);
            KEY_CHECK(snapshot[0].in_flight_submissions == 0);
        }

        // (b) 占满份额 4 后拒绝 1 个：admitted_total 保持 4+4=8（4 个快速
        // + 4 个阻塞均接纳；被拒的 1 个不计入），在途不受拒绝影响；全部
        // 释放后仍为 8。
        std::promise<void> release;
        auto release_future = release.get_future().share();
        std::promise<void> started;
        auto started_future = started.get_future();
        std::vector<std::future<void>> blocked;
        blocked.push_back(ex.submit_auto(kairo::subject("A"), [&] {
            started.set_value();
            release_future.wait();
        }));
        for (int i = 1; i < 4; ++i) {
            blocked.push_back(ex.submit_auto(
                kairo::subject("A"), [&] { release_future.wait(); }));
        }
        started_future.wait();
        KEY_CHECK(ex.get_subject_in_flight_submissions("A") == 4);
        auto rejected = ex.submit_auto(kairo::subject("A"), [] {});
        KEY_CHECK(settled_soon(rejected, std::chrono::seconds(2)));
        (void)capacity_rejection_message(rejected);
        {
            const auto snapshot = ex.get_subject_quota_status();
            KEY_CHECK(snapshot.size() == 1);
            KEY_CHECK(snapshot[0].admitted_total == 8);
            KEY_CHECK(snapshot[0].in_flight_submissions == 4);
        }
        release.set_value();
        for (auto& future : blocked) {
            KEY_CHECK(settled_soon(future, std::chrono::seconds(2)));
            future.get();
        }
        {
            const auto snapshot = ex.get_subject_quota_status();
            KEY_CHECK(snapshot.size() == 1);
            KEY_CHECK(snapshot[0].admitted_total == 8);
            KEY_CHECK(snapshot[0].in_flight_submissions == 0);
        }

        // (c) 解除后重建 = 新代从 0 起计（不继承旧代残量）。
        ex.set_subject_max_in_flight_tasks("A", 0);
        KEY_CHECK(ex.get_subject_quota_status().empty());
        ex.set_subject_max_in_flight_tasks("A", 3);
        {
            const auto snapshot = ex.get_subject_quota_status();
            KEY_CHECK(snapshot.size() == 1);
            KEY_CHECK(snapshot[0].admitted_total == 0);
            KEY_CHECK(snapshot[0].in_flight_submissions == 0);
        }
        auto gen2_a = ex.submit_auto(kairo::subject("A"), [] { return 1; });
        auto gen2_b = ex.submit_auto(kairo::subject("A"), [] { return 2; });
        KEY_CHECK(settled_soon(gen2_a, std::chrono::seconds(2)));
        KEY_CHECK(settled_soon(gen2_b, std::chrono::seconds(2)));
        KEY_CHECK(gen2_a.get() == 1);
        KEY_CHECK(gen2_b.get() == 2);
        {
            const auto snapshot = ex.get_subject_quota_status();
            KEY_CHECK(snapshot.size() == 1);
            KEY_CHECK(snapshot[0].admitted_total == 2);
        }

        // (d) 未配置键的查询：快照无该键条目（"未知即宽容"，不产生表项）。
        auto unconfigured = ex.submit_auto(
            kairo::subject("never-configured"), [] { return 3; });
        KEY_CHECK(settled_soon(unconfigured, std::chrono::seconds(2)));
        KEY_CHECK(unconfigured.get() == 3);
        {
            const auto snapshot = ex.get_subject_quota_status();
            KEY_CHECK(snapshot.size() == 1);
            KEY_CHECK(snapshot[0].subject == "A");
        }
        KEY_CHECK(ex.get_subject_in_flight_submissions("never-configured") ==
                  0);
        KEY_CHECK(ex.get_subject_max_in_flight_tasks("never-configured") == 0);
        ex.shutdown();
    }

    // 场景 5（运行时补充）：SubjectKey explicit 直构可用，subject() 工厂
    // 携带键原文（编译期 static_assert 见文件头匿名命名空间）。
    {
        kairo::SubjectKey direct(std::string("lit"));
        KEY_CHECK(direct.key == "lit");
        const auto factory = kairo::subject("tenant-A");
        KEY_CHECK(factory.key == "tenant-A");
        // R-2 键的敏感性约定：键逐字复制进拒绝消息与事件（构造期不改动）。
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        KEY_CHECK(ex.initialize(config));
        ex.set_subject_max_in_flight_tasks("exact-key", 1);
        std::promise<void> release;
        auto release_future = release.get_future().share();
        auto blocker = ex.submit_auto(kairo::subject("exact-key"), [&] {
            release_future.wait();
        });
        KEY_CHECK(ex.get_subject_in_flight_submissions("exact-key") == 1);
        auto overflow = ex.submit_auto(kairo::subject("exact-key"), [] {});
        KEY_CHECK(settled_soon(overflow, std::chrono::seconds(2)));
        const std::string message = capacity_rejection_message(overflow);
        KEY_CHECK(message.find("key=exact-key") != std::string::npos);
        const auto recent = ex.get_recent_failures(1);
        KEY_CHECK(recent.size() == 1);
        KEY_CHECK(recent.front().subject == "exact-key");
        release.set_value();
        KEY_CHECK(settled_soon(blocker, std::chrono::seconds(2)));
        blocker.get();
        ex.shutdown();
    }

    // 场景 6：零回归锚——不带键的 submit_auto / submit 行为不变。
    {
        // (i) 无份额配置：裸形态与 submit() 均正常接纳完成。
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        KEY_CHECK(ex.initialize(config));
        auto bare_unkeyed = ex.submit_auto([] { return 1; });
        KEY_CHECK(settled_soon(bare_unkeyed, std::chrono::seconds(2)));
        KEY_CHECK(bare_unkeyed.get() == 1);
        auto plain = ex.submit([] { return 2; });
        KEY_CHECK(settled_soon(plain, std::chrono::seconds(2)));
        KEY_CHECK(plain.get() == 2);
        KEY_CHECK(ex.get_subject_quota_status().empty());
        const auto failure_i = ex.get_failure_status();
        KEY_CHECK(failure_i.capacity_exhausted_count == 0);
        KEY_CHECK(failure_i.subject_quota_exhausted_count == 0);
        ex.shutdown();
    }
    {
        // (ii) 全局配置：不带键裸形态按总量语义拒绝（消息为总量文本、
        // 事件 subject 为空、只递增 capacity_exhausted_count）；带未配置键
        // 的裸形态同样只按总量语义拒绝且不产生分桶表项。
        Executor ex;
        ExecutorConfig config;
        config.min_threads = 1;
        config.max_threads = 1;
        config.max_in_flight_tasks = 2;
        KEY_CHECK(ex.initialize(config));

        std::promise<void> release;
        auto release_future = release.get_future().share();
        std::promise<void> started;
        auto started_future = started.get_future();
        std::vector<std::future<void>> blocked;
        blocked.push_back(ex.submit([&] {
            started.set_value();
            release_future.wait();
        }));
        blocked.push_back(ex.submit([&] { release_future.wait(); }));
        started_future.wait();
        KEY_CHECK(ex.get_in_flight_submissions() == 2);

        std::atomic<int> must_not_run{0};
        auto bare_unkeyed = ex.submit_auto(
            [&] { must_not_run.fetch_add(1, std::memory_order_relaxed); });
        KEY_CHECK(settled_soon(bare_unkeyed, std::chrono::seconds(2)));
        const std::string unkeyed_message =
            capacity_rejection_message(bare_unkeyed);
        KEY_CHECK(unkeyed_message.find(
                      "In-flight submission capacity exhausted") !=
                  std::string::npos);
        KEY_CHECK(unkeyed_message.find("subject_max_in_flight") ==
                  std::string::npos);

        auto bare_unconfigured = ex.submit_auto(
            kairo::subject("unconfigured"), [&] {
                must_not_run.fetch_add(1, std::memory_order_relaxed);
            });
        KEY_CHECK(settled_soon(bare_unconfigured, std::chrono::seconds(2)));
        const std::string unconfigured_message =
            capacity_rejection_message(bare_unconfigured);
        KEY_CHECK(unconfigured_message.find(
                      "In-flight submission capacity exhausted") !=
                  std::string::npos);
        KEY_CHECK(unconfigured_message.find("subject_max_in_flight") ==
                  std::string::npos);
        // 未配置键经裸形态流入也不产生分桶表项。
        KEY_CHECK(ex.get_subject_quota_status().empty());
        // 被拒任务体均未运行。
        KEY_CHECK(must_not_run.load() == 0);

        const auto failure_status = ex.get_failure_status();
        KEY_CHECK(failure_status.capacity_exhausted_count == 2);
        KEY_CHECK(failure_status.subject_quota_exhausted_count == 0);
        const auto recent = ex.get_recent_failures(2);
        KEY_CHECK(recent.size() == 2);
        for (const auto& event : recent) {
            KEY_CHECK(event.kind == kairo::FailureKind::CapacityExhausted);
            KEY_CHECK(event.subject.empty());
        }
        KEY_CHECK(ex.get_in_flight_submissions() == 2);

        release.set_value();
        for (auto& future : blocked) {
            KEY_CHECK(settled_soon(future, std::chrono::seconds(2)));
            future.get();
        }
        KEY_CHECK(ex.get_in_flight_submissions() == 0);
        ex.shutdown();
    }

    std::printf("test_subject_key_bare_form: all blocks passed\n");
    return 0;
}
