// test_api_doc_cancellation_fields.cpp
// D1 文档一致性测试：锁定 docs/API.md 中"任务协作取消 / 定时句柄 / schema 3"
// 的字段与签名宣称和实际公共结构一致。
//
// 复用 test_api_doc_status_fields 的模式：从 docs/API.md 提取字段列表，与
// include/executor/task_cancellation.hpp、timer.hpp、types.hpp 的真实成员
// （通过 requires 表达式在编译期枚举）比对，防止文档漂移。

#include <kairo/executor.hpp>
#include <kairo/task_cancellation.hpp>
#include <kairo/timer.hpp>
#include <kairo/types.hpp>

#include <gtest/gtest.h>

#include <cctype>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace kairo;

namespace {

std::string read_api_md() {
    // 测试二进制位于 build/tests/，docs 位于仓库根 docs/。
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

// 编译期枚举真实字段名（新增字段而忘记更新 requires 列表时编译失败，
// 与文档提取集合比对防止只改一侧）。
std::set<std::string> actual_cancellation_status_fields() {
    std::set<std::string> fields;
    if constexpr (requires { &CancellationStatus::request_count; }) {
        fields.insert("request_count");
    }
    if constexpr (requires { &CancellationStatus::queued_cancelled_count; }) {
        fields.insert("queued_cancelled_count");
    }
    if constexpr (requires { &CancellationStatus::running_request_count; }) {
        fields.insert("running_request_count");
    }
    if constexpr (requires { &CancellationStatus::completed_after_request_count; }) {
        fields.insert("completed_after_request_count");
    }
    return fields;
}

std::set<std::string> actual_timer_summary_fields() {
    std::set<std::string> fields;
    if constexpr (requires { &TimerStatusSummary::pending_count; }) {
        fields.insert("pending_count");
    }
    if constexpr (requires { &TimerStatusSummary::executed_count; }) {
        fields.insert("executed_count");
    }
    if constexpr (requires { &TimerStatusSummary::cancelled_count; }) {
        fields.insert("cancelled_count");
    }
    return fields;
}

std::set<std::string> actual_timer_status_fields() {
    std::set<std::string> fields;
    if constexpr (requires { &TimerStatus::timer_id; }) {
        fields.insert("timer_id");
    }
    if constexpr (requires { &TimerStatus::state; }) {
        fields.insert("state");
    }
    if constexpr (requires { &TimerStatus::periodic; }) {
        fields.insert("periodic");
    }
    if constexpr (requires { &TimerStatus::execution_count; }) {
        fields.insert("execution_count");
    }
    if constexpr (requires { &TimerStatus::active_callback_count; }) {
        fields.insert("active_callback_count");
    }
    if constexpr (requires { &TimerStatus::cancellation_count; }) {
        fields.insert("cancellation_count");
    }
    if constexpr (requires { &TimerStatus::next_execute_time; }) {
        fields.insert("next_execute_time");
    }
    return fields;
}

// 从一行文档中提取反引号包裹的字段名。
std::set<std::string> extract_backticked_fields(const std::string& line) {
    std::set<std::string> fields;
    size_t pos = 0;
    while ((pos = line.find('`', pos)) != std::string::npos) {
        const size_t end = line.find('`', pos + 1);
        if (end == std::string::npos) {
            break;
        }
        std::string token = line.substr(pos + 1, end - pos - 1);
        // 字段名：小写开头、无空格/标点（periodic/state 等无下划线也算）。
        if (!token.empty() && token.find(' ') == std::string::npos &&
            token.find('(') == std::string::npos &&
            token.find("）") == std::string::npos &&
            std::islower(static_cast<unsigned char>(token[0]))) {
            fields.insert(token);
        }
        pos = end + 1;
    }
    return fields;
}

// 返回包含 needle 的完整行（从行首截取，保证反引号配对不错位）。
std::string full_line_containing(const std::string& doc,
                                 const std::string& needle) {
    size_t pos = doc.find(needle);
    if (pos == std::string::npos) {
        return {};
    }
    const size_t line_begin = doc.rfind('\n', pos);
    const size_t begin = line_begin == std::string::npos ? 0 : line_begin + 1;
    const size_t line_end = doc.find('\n', pos);
    const size_t end =
        line_end == std::string::npos ? doc.size() : line_end;
    return doc.substr(begin, end - begin);
}

// 从"："分隔符之后提取反引号字段（跳过函数名 token 本身）。
std::set<std::string> extract_fields_after_colon(const std::string& line) {
    const size_t colon = line.find("：");
    if (colon == std::string::npos) {
        return {};
    }
    return extract_backticked_fields(line.substr(colon));
}

}  // namespace

TEST(ApiDocCancellationFields, CancellationStatusDocMatchesStruct) {
    const std::string doc = read_api_md();
    ASSERT_FALSE(doc.empty()) << "docs/API.md not found from test cwd";

    const std::string line =
        full_line_containing(doc, "取消生命周期独立计数");
    ASSERT_FALSE(line.empty())
        << "API.md must document get_cancellation_status counters";

    const auto documented = extract_fields_after_colon(line);
    const auto actual = actual_cancellation_status_fields();
    EXPECT_EQ(documented, actual)
        << "CancellationStatus fields and API.md bullet must stay in sync";
}

TEST(ApiDocCancellationFields, TimerSummaryDocMatchesStruct) {
    const std::string doc = read_api_md();
    ASSERT_FALSE(doc.empty());

    const std::string line =
        full_line_containing(doc, "get_timer_status_summary");
    ASSERT_FALSE(line.empty())
        << "API.md must document get_timer_status_summary counters";

    const auto documented = extract_fields_after_colon(line);
    const auto actual = actual_timer_summary_fields();
    EXPECT_EQ(documented, actual)
        << "TimerStatusSummary fields and API.md bullet must stay in sync";
}

TEST(ApiDocCancellationFields, TimerStatusDocMatchesStruct) {
    const std::string doc = read_api_md();
    ASSERT_FALSE(doc.empty());

    // TimerHandle::status() 的 TimerStatus 字段列表段（跨行，以"）"结束）。
    const std::string marker = "返回 `TimerStatus`（";
    size_t pos = doc.find(marker);
    ASSERT_NE(pos, std::string::npos)
        << "API.md must document TimerStatus fields";
    const size_t open_paren = pos + marker.size() - 1;
    const size_t close_paren = doc.find("）", open_paren);
    ASSERT_NE(close_paren, std::string::npos);
    const std::string segment = doc.substr(open_paren + 1, close_paren - open_paren - 1);

    const auto documented = extract_backticked_fields(segment);
    const auto actual = actual_timer_status_fields();
    EXPECT_EQ(documented, actual)
        << "TimerStatus fields and API.md segment must stay in sync";
}

TEST(ApiDocCancellationFields, CancellationApiSignaturesPresent) {
    const std::string doc = read_api_md();
    ASSERT_FALSE(doc.empty());

    // 关键签名与语义边界宣称必须存在；缺失即文档回归。
    for (const std::string& needle : {
             "request_task_cancel(const TaskHandle& handle) noexcept",
             "submit_cancellable(",
             "submit_cancellable_priority(",
             "submit_cancellable_after(",
             "submit_delayed(",
             "submit_delayed_cancellable(",
             "submit_periodic(",
             "submit_periodic_cancellable(",
             "CancelledBeforeDispatch",
             "CancellationRequestedAfterDispatch",
             "TaskCancelled(Explicit)",
             "TaskCancelled(DependencyCancelled)",
             "TaskCancelled(Shutdown)",
             "取消是**请求不是中断**",
         }) {
        EXPECT_NE(doc.find(needle), std::string::npos)
            << "API.md must keep documenting: " << needle;
    }
}

TEST(ApiDocCancellationFields, SnapshotSchemaVersionDocumented) {
    const std::string doc = read_api_md();
    ASSERT_FALSE(doc.empty());
    EXPECT_NE(doc.find("`schema_version`（当前为 **3**）"), std::string::npos)
        << "API.md must document the current snapshot schema version";

    // 代码侧 schema 版本必须与文档宣称一致。
    ExecutorSnapshot snapshot;
    EXPECT_EQ(snapshot.schema_version, 3u);
}
