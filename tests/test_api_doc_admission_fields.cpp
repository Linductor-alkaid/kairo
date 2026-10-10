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

using namespace kairo;

namespace {

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
    if constexpr (requires { &ExecutorFailureStatus::task_exception_count; }) {
        fields.insert("task_exception_count");
    }
    if constexpr (requires { &ExecutorFailureStatus::submit_rejected_count; }) {
        fields.insert("submit_rejected_count");
    }
    if constexpr (requires { &ExecutorFailureStatus::timeout_count; }) {
        fields.insert("timeout_count");
    }
    if constexpr (requires { &ExecutorFailureStatus::realtime_drop_count; }) {
        fields.insert("realtime_drop_count");
    }
    if constexpr (requires { &ExecutorFailureStatus::gpu_failure_count; }) {
        fields.insert("gpu_failure_count");
    }
    if constexpr (requires { &ExecutorFailureStatus::wait_timeout_count; }) {
        fields.insert("wait_timeout_count");
    }
    if constexpr (requires { &ExecutorFailureStatus::tuning_fallback_count; }) {
        fields.insert("tuning_fallback_count");
    }
    if constexpr (requires { &ExecutorFailureStatus::capacity_exhausted_count; }) {
        fields.insert("capacity_exhausted_count");
    }
    if constexpr (requires { &ExecutorFailureStatus::total_count; }) {
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
    static_assert(requires { ExecutorConfig{}.max_in_flight_tasks; },
                  "ExecutorConfig must expose max_in_flight_tasks");

    // 观测 API 存在。
    static_assert(requires(const Executor& ex) {
        ex.get_in_flight_submissions();
        ex.get_max_in_flight_tasks();
    }, "Executor must expose admission observers");
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
