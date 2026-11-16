// test_api_doc_admission_fields.cpp
// D3 文档一致性测试：锁定 docs/API.md 中"总量有界 admission / 串行派发"的
// 字段与语义宣称和实际公共结构一致（复用 test_api_doc_cancellation_fields
// 模式，防止文档漂移）。

#include <kairo/config.hpp>
#include <kairo/executor.hpp>
#include <kairo/types.hpp>

#include <gtest/gtest.h>

#include <cctype>
#include <cstdio>
#include <set>
#include <string>
#include <type_traits>

using namespace kairo;

namespace {

// ---- requires 表达式的 C++17 替身（GCC 9 兼容）----
// 成员存在性探测：KAIRO_DOC_FIELD_PROBE(member) 生成一对重载，
// probe_<member><Struct>() 经 decltype(&Struct::member) 的默认模板参
// SFINAE——成员存在时 int 重载精确匹配胜出，否则回落 long 返回 false。
#define KAIRO_DOC_FIELD_PROBE(member)                                          \
    template <typename Struct>                                                 \
    constexpr bool probe_##member(long) noexcept { return false; }             \
    template <typename Struct, typename = decltype(&Struct::member)>           \
    constexpr bool probe_##member(int) noexcept { return true; }

KAIRO_DOC_FIELD_PROBE(task_exception_count)
KAIRO_DOC_FIELD_PROBE(submit_rejected_count)
KAIRO_DOC_FIELD_PROBE(timeout_count)
KAIRO_DOC_FIELD_PROBE(realtime_drop_count)
KAIRO_DOC_FIELD_PROBE(gpu_failure_count)
KAIRO_DOC_FIELD_PROBE(wait_timeout_count)
KAIRO_DOC_FIELD_PROBE(tuning_fallback_count)
KAIRO_DOC_FIELD_PROBE(capacity_exhausted_count)
KAIRO_DOC_FIELD_PROBE(total_count)
KAIRO_DOC_FIELD_PROBE(subject_quota_exhausted_count)
KAIRO_DOC_FIELD_PROBE(subject)
KAIRO_DOC_FIELD_PROBE(max_in_flight_tasks)
KAIRO_DOC_FIELD_PROBE(in_flight_submissions)

#undef KAIRO_DOC_FIELD_PROBE

// 表达式序列探测：把待检表达式放进仿函数 operator()（永不调用，
// 仅参与编译期 SFINAE），随库签名漂移即编译失败。
template <typename Probe, typename = void>
struct ExprCompiles : std::false_type {};
template <typename Probe>
struct ExprCompiles<Probe, std::void_t<decltype(std::declval<Probe&>()())>>
    : std::true_type {};

// ExecutorConfig{}.max_in_flight_tasks 可编译。
struct ProbeConfigMaxInFlight {
    void operator()() { (void)ExecutorConfig{}.max_in_flight_tasks; }
};

// Executor 观测 API（const 语境）。
struct ProbeAdmissionObservers {
    void operator()() {
        const Executor& ex = *static_cast<const Executor*>(nullptr);
        (void)ex.get_in_flight_submissions();
        (void)ex.get_max_in_flight_tasks();
    }
};

// Executor 分主体配额 API（可变语境）。
struct ProbeSubjectQuotaApi {
    void operator()() {
        Executor& ex = *static_cast<Executor*>(nullptr);
        ex.set_subject_max_in_flight_tasks("A", 1);
        ex.get_subject_max_in_flight_tasks("A");
        ex.get_subject_in_flight_submissions("A");
        (void)ex.get_subject_quota_status();
    }
};

std::string read_api_md() {
    const char* candidates[] = {
        "../docs/API.md",
        "../../docs/API.md",
        "../../../docs/API.md",
    };
    for (const char* path : candidates) {
        if (FILE* file = std::fopen(path, "rb")) {
            std::string content;
            char buffer[4096];
            size_t got = 0;
            while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
                content.append(buffer, got);
            }
            std::fclose(file);
            if (!content.empty()) {
                return content;
            }
        }
    }
    return {};
}

std::string read_readme_md() {
    const char* candidates[] = {
        "../README.md",
        "../../README.md",
        "../../../README.md",
    };
    for (const char* path : candidates) {
        if (FILE* file = std::fopen(path, "rb")) {
            std::string content;
            char buffer[4096];
            size_t got = 0;
            while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
                content.append(buffer, got);
            }
            std::fclose(file);
            if (!content.empty()) {
                return content;
            }
        }
    }
    return {};
}

std::set<std::string> actual_failure_status_fields() {
    std::set<std::string> fields;
    if constexpr (probe_task_exception_count<ExecutorFailureStatus>(0)) {
        fields.insert("task_exception_count");
    }
    if constexpr (probe_submit_rejected_count<ExecutorFailureStatus>(0)) {
        fields.insert("submit_rejected_count");
    }
    if constexpr (probe_timeout_count<ExecutorFailureStatus>(0)) {
        fields.insert("timeout_count");
    }
    if constexpr (probe_realtime_drop_count<ExecutorFailureStatus>(0)) {
        fields.insert("realtime_drop_count");
    }
    if constexpr (probe_gpu_failure_count<ExecutorFailureStatus>(0)) {
        fields.insert("gpu_failure_count");
    }
    if constexpr (probe_wait_timeout_count<ExecutorFailureStatus>(0)) {
        fields.insert("wait_timeout_count");
    }
    if constexpr (probe_tuning_fallback_count<ExecutorFailureStatus>(0)) {
        fields.insert("tuning_fallback_count");
    }
    if constexpr (probe_capacity_exhausted_count<ExecutorFailureStatus>(0)) {
        fields.insert("capacity_exhausted_count");
    }
    if constexpr (probe_total_count<ExecutorFailureStatus>(0)) {
        fields.insert("total_count");
    }
    return fields;
}

std::set<std::string> extract_backticked_fields(const std::string& line) {
    std::set<std::string> fields;
    size_t pos = 0;
    while ((pos = line.find('`', pos)) != std::string::npos) {
        const size_t end = line.find('`', pos + 1);
        if (end == std::string::npos) {
            break;
        }
        std::string token = line.substr(pos + 1, end - pos - 1);
        if (!token.empty() && token.find(' ') == std::string::npos &&
            token.find('(') == std::string::npos &&
            std::islower(static_cast<unsigned char>(token[0]))) {
            fields.insert(token);
        }
        pos = end + 1;
    }
    return fields;
}

std::string full_line_containing(const std::string& doc,
                                 const std::string& needle) {
    size_t pos = doc.find(needle);
    if (pos == std::string::npos) {
        return {};
    }
    const size_t line_begin = doc.rfind('\n', pos);
    const size_t begin = line_begin == std::string::npos ? 0 : line_begin + 1;
    const size_t line_end = doc.find('\n', pos);
    const size_t end = line_end == std::string::npos ? doc.size() : line_end;
    return doc.substr(begin, end - begin);
}

std::set<std::string> extract_fields_after_colon(const std::string& line) {
    const size_t colon = line.find("：");
    if (colon == std::string::npos) {
        return {};
    }
    return extract_backticked_fields(line.substr(colon));
}

}  // namespace

TEST(ApiDocAdmissionFields, FailureStatusDocMatchesStruct) {
    const std::string doc = read_api_md();
    ASSERT_FALSE(doc.empty()) << "docs/API.md not found from test cwd";

    const std::string line = full_line_containing(doc, "**ExecutorFailureStatus**");
    ASSERT_FALSE(line.empty()) << "API.md must document ExecutorFailureStatus fields";

    const auto documented = extract_fields_after_colon(line);
    const auto actual = actual_failure_status_fields();
    EXPECT_EQ(documented, actual)
        << "ExecutorFailureStatus fields and API.md bullet must stay in sync";
}

TEST(ApiDocAdmissionFields, AdmissionSectionAndConfigDocumented) {
    const std::string doc = read_api_md();
    ASSERT_FALSE(doc.empty());

    // §3.11 章节存在并覆盖关键语义宣称（8ba4f83 文档规范化重编号 3.10→3.11）。
    EXPECT_NE(doc.find("### 3.11 总量有界 admission"), std::string::npos)
        << "API.md must keep the bounded admission section";
    EXPECT_NE(doc.find("CapacityExhaustedException"), std::string::npos)
        << "API.md must document the capacity rejection exception";
    EXPECT_NE(doc.find("FailureKind::CapacityExhausted"), std::string::npos)
        << "API.md must document the capacity failure kind";
    EXPECT_NE(doc.find("set_max_in_flight_tasks"), std::string::npos)
        << "API.md must document runtime admission adjustment";

    // §7.1 配置表行（代码块中的 config.max_in_flight_tasks 不是表格行）。
    const std::string config_line =
        full_line_containing(doc, "`max_in_flight_tasks` | `size_t`");
    ASSERT_FALSE(config_line.empty())
        << "API.md config table must document max_in_flight_tasks";
    EXPECT_NE(config_line.find("queue_capacity"), std::string::npos)
        << "config doc must contrast max_in_flight_tasks with queue_capacity";
    static_assert(ExprCompiles<ProbeConfigMaxInFlight>::value,
                  "ExecutorConfig must expose max_in_flight_tasks");

    // 观测 API 存在。
    static_assert(ExprCompiles<ProbeAdmissionObservers>::value,
                  "Executor must expose admission observers");
}

TEST(ApiDocAdmissionFields, SerialDispatchSemanticsDocumented) {
    const std::string doc = read_api_md();
    ASSERT_FALSE(doc.empty());

    const std::string section = full_line_containing(doc, "派发与结算分离");
    ASSERT_FALSE(section.empty())
        << "API.md serial context section must document the dispatch/settlement split";
    EXPECT_NE(doc.find("ticket FIFO"), std::string::npos)
        << "API.md must document FIFO progress under multi-worker bursts";
}

TEST(ApiDocAdmissionFields, ReadmeBoundaryMentionsAdmission) {
    const std::string readme = read_readme_md();
    ASSERT_FALSE(readme.empty()) << "README.md not found from test cwd";
    EXPECT_NE(readme.find("max_in_flight_tasks"), std::string::npos)
        << "README capability boundary must mention total admission";
    EXPECT_NE(readme.find("queue_capacity"), std::string::npos)
        << "README must state queue_capacity is not a backpressure bound";
}

// 分主体在途份额（分桶 admission，issue #222）：锁定 docs/API.md §3.12
// 存在且覆盖新公开符号；types.hpp 新字段/新结构与文档宣称一致。
TEST(ApiDocAdmissionFields, SubjectBucketAdmissionDocumented) {
    const std::string doc = read_api_md();
    ASSERT_FALSE(doc.empty()) << "docs/API.md not found from test cwd";

    // §3.12 章节存在并覆盖新公开符号（与 §3.11 总量 admission 锁同风格）。
    EXPECT_NE(doc.find("### 3.12 分主体在途份额"), std::string::npos)
        << "API.md must keep the subject bucket admission section";
    EXPECT_NE(doc.find("set_subject_max_in_flight_tasks"), std::string::npos)
        << "API.md must document the subject quota setter";
    EXPECT_NE(doc.find("get_subject_quota_status"), std::string::npos)
        << "API.md must document the quota snapshot getter";
    EXPECT_NE(doc.find("TaskBuilder::subject"), std::string::npos)
        << "API.md must document the keyed TaskBuilder setter";
    EXPECT_NE(doc.find("subject_quota_exhausted_count"), std::string::npos)
        << "API.md must document the subject quota rejection counter";
    EXPECT_NE(doc.find("SubjectQuotaStatus"), std::string::npos)
        << "API.md must document the SubjectQuotaStatus snapshot type";

    // types.hpp 新字段与新结构真实存在（编译期锁，随库签名漂移即编译失败）。
    static_assert(probe_subject_quota_exhausted_count<ExecutorFailureStatus>(0),
                  "ExecutorFailureStatus must expose subject_quota_exhausted_count");
    static_assert(probe_subject<ExecutorFailureEvent>(0),
                  "ExecutorFailureEvent must expose the subject field");
    static_assert(probe_subject<SubjectQuotaStatus>(0) &&
                      probe_max_in_flight_tasks<SubjectQuotaStatus>(0) &&
                      probe_in_flight_submissions<SubjectQuotaStatus>(0),
                  "SubjectQuotaStatus must expose subject/max/in_flight fields");
    static_assert(ExprCompiles<ProbeSubjectQuotaApi>::value,
                  "Executor must expose the subject quota API surface");

    // 观测面语义宣称：§3.12 记载事件 subject 携带分桶键（空 = 总量拒绝）
    // 且 subject_quota_exhausted_count 是 capacity_exhausted_count 的子集
    //（语义跨续行，按位置窗口断言）。
    const size_t subject_count_pos = doc.find("`subject_quota_exhausted_count`");
    ASSERT_NE(subject_count_pos, std::string::npos)
        << "API.md §3.12 must document the subject quota rejection counter";
    const std::string semantics_window =
        doc.substr(subject_count_pos, 200);
    EXPECT_NE(semantics_window.find("capacity_exhausted_count"), std::string::npos)
        << "subject quota count must be documented alongside capacity count";
    EXPECT_NE(semantics_window.find("前者是后者的子集"), std::string::npos)
        << "subject quota count must be documented as a subset of capacity count";
}
