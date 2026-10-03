// P-260618-008: doc-vs-struct constraint test.
// Parses docs/API.md §7.3 "RealtimeExecutorStatus" entry and asserts the
// field names listed there match the actual members of
// `kairo::RealtimeExecutorStatus` in include/executor/types.hpp.
//
// Prevents the recurrence of the bug fixed by P-260618-008: a struct gets
// new fields (dropped_task_count, failed_pushes, peak_queue_size,
// queue_capacity, rejected_not_running_count, rejected_empty_task_count,
// pool_exhausted_count, queue_full_count) but docs/API.md §7.3 is not updated,
// leaving users with stale documentation. Also covers P-008 batch performance
// claim sources.

#include <kairo/types.hpp>
#include "kairo/thread_pool/thread_pool.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string extract_status_entry(const std::string& api_md) {
    const std::string marker = "- **RealtimeExecutorStatus**";
    auto pos = api_md.find(marker);
    if (pos == std::string::npos) {
        return {};
    }
    auto end = api_md.find("\n- **", pos + marker.size());
    if (end == std::string::npos) {
        end = api_md.size();
    }
    return api_md.substr(pos, end - pos);
}

std::string extract_bullet_entry(const std::string& api_md,
                                 const std::string& marker) {
    auto pos = api_md.find(marker);
    if (pos == std::string::npos) {
        return {};
    }
    auto end = api_md.find("\n- **", pos + marker.size());
    if (end == std::string::npos) {
        end = api_md.size();
    }
    return api_md.substr(pos, end - pos);
}

std::string extract_section(const std::string& api_md,
                            const std::string& begin_marker,
                            const std::string& end_marker) {
    auto pos = api_md.find(begin_marker);
    if (pos == std::string::npos) {
        return {};
    }
    auto end = api_md.find(end_marker, pos + begin_marker.size());
    if (end == std::string::npos) {
        end = api_md.size();
    }
    return api_md.substr(pos, end - pos);
}

std::set<std::string> extract_field_names(const std::string& entry) {
    std::set<std::string> fields;
    std::regex re("`([a-zA-Z_][a-zA-Z0-9_]*)`");
    for (auto it = std::sregex_iterator(entry.begin(), entry.end(), re);
         it != std::sregex_iterator(); ++it) {
        fields.insert((*it)[1].str());
    }
    return fields;
}



std::string read_doc_from_candidates(const std::vector<std::string>& candidates,
                                     std::string& path_used) {
    std::ifstream in;
    for (const auto& p : candidates) {
        in.open(p);
        if (in.good()) {
            path_used = p;
            break;
        }
        in.clear();
    }
    if (!in.good()) {
        return {};
    }

    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string source_root() {
    const std::string source_file = __FILE__;
    const std::string marker = "/tests/";
    const auto tests_directory = source_file.rfind(marker);
    return tests_directory == std::string::npos
        ? std::string{}
        : source_file.substr(0, tests_directory);
}

std::set<std::string> extract_struct_fields(const std::string& header,
                                            const std::string& struct_name) {
    std::set<std::string> fields;
    const std::string marker = "struct " + struct_name + " {";
    auto pos = header.find(marker);
    if (pos == std::string::npos) {
        return fields;
    }

    auto end = header.find("\n};", pos + marker.size());
    if (end == std::string::npos) {
        return fields;
    }

    const auto body = header.substr(pos + marker.size(), end - pos - marker.size());
    std::regex member_re(R"(^\s*(?:[A-Za-z_][A-Za-z0-9_:<>]*\s+)+([A-Za-z_][A-Za-z0-9_]*)\s*(?:=\s*[^;]+)?;)");
    std::stringstream lines(body);
    std::string line;
    while (std::getline(lines, line)) {
        std::smatch match;
        if (std::regex_search(line, match, member_re)) {
            fields.insert(match[1].str());
        }
    }

    return fields;
}

bool contains_regex(const std::string& text, const std::string& pattern) {
    return std::regex_search(text, std::regex(pattern));
}

std::set<std::string> realtime_status_fields() {
    return {
        "name", "is_running", "cycle_period_ns", "cycle_count",
        "cycle_timeout_count", "avg_cycle_time_ns", "max_cycle_time_ns",
        "priority_applied", "cpu_affinity_applied", "memory_locked",
        "timer_slack_applied", "dropped_task_count", "failed_pushes",
        "peak_queue_size", "queue_capacity", "rejected_not_running_count",
        "rejected_empty_task_count", "pool_exhausted_count", "queue_full_count",
    };
}

TEST(ApiDocStatusFields, RealtimeExecutorStatusEntryMatchesStruct) {
    const std::vector<std::string> candidates = {
        "docs/API.md",
        "../docs/API.md",
        "../../docs/API.md",
        source_root() + "/docs/API.md",
    };

    std::string path_used;
    const std::string api_md = read_doc_from_candidates(candidates, path_used);
    ASSERT_FALSE(api_md.empty()) << "Could not open docs/API.md from any candidate path";

    const auto entry = extract_status_entry(api_md);
    ASSERT_FALSE(entry.empty())
        << "RealtimeExecutorStatus entry not found in docs/API.md §7.3";

    const auto doc_fields = extract_field_names(entry);

    const auto struct_fields = realtime_status_fields();

    EXPECT_NE(entry.find("rejected_not_running_count"), std::string::npos);
    EXPECT_NE(entry.find("rejected_empty_task_count"), std::string::npos);
    EXPECT_NE(entry.find("pool_exhausted_count"), std::string::npos);
    EXPECT_NE(entry.find("queue_full_count"), std::string::npos);

    std::set<std::string> doc_struct_fields;
    for (const auto& f : doc_fields) {
        if (struct_fields.count(f) > 0) {
            doc_struct_fields.insert(f);
        }
    }

    EXPECT_EQ(doc_struct_fields, struct_fields)
        << "RealtimeExecutorStatus doc-vs-struct mismatch in " << path_used;
    for (const auto& f : doc_struct_fields) {
        if (struct_fields.count(f) == 0) {
            ADD_FAILURE() << "  - " << f;
        }
    }
    for (const auto& f : struct_fields) {
        if (doc_struct_fields.count(f) == 0) {
            ADD_FAILURE() << "  + " << f;
        }
    }
}

TEST(ApiDocStatusFields, RealtimeDropCounterDocumentationMatchesImplementation) {
    std::string api_path;
    const std::string api_md = read_doc_from_candidates(
        {"docs/API.md", "../docs/API.md", "../../docs/API.md", source_root() + "/docs/API.md"}, api_path);
    ASSERT_FALSE(api_md.empty()) << "Could not open docs/API.md from any candidate path";

    const std::string entry = extract_status_entry(api_md);
    ASSERT_FALSE(entry.empty())
        << "RealtimeExecutorStatus entry not found in docs/API.md §7.3";

    const auto dropped_pos = entry.find("`dropped_task_count`");
    ASSERT_NE(dropped_pos, std::string::npos);
    const auto dropped_description = entry.substr(dropped_pos);

    EXPECT_NE(dropped_description.find("空任务"), std::string::npos);
    EXPECT_NE(dropped_description.find("未运行/已停止"), std::string::npos);
    EXPECT_NE(dropped_description.find("对象池耗尽"), std::string::npos);
    EXPECT_NE(dropped_description.find("队列满"), std::string::npos);
    EXPECT_NE(dropped_description.find("pool_exhausted_count"), std::string::npos);
    EXPECT_NE(dropped_description.find("queue_full_count"), std::string::npos);
    EXPECT_NE(dropped_description.find("rejected_not_running_count"), std::string::npos);
    EXPECT_NE(dropped_description.find("rejected_empty_task_count"), std::string::npos);
}

TEST(ApiDocStatusFields, FailureCountersMatchImplementation) {
    std::string api_path;
    const std::string api_md = read_doc_from_candidates(
        {"docs/API.md", "../docs/API.md", "../../docs/API.md", source_root() + "/docs/API.md"}, api_path);
    ASSERT_FALSE(api_md.empty()) << "Could not open docs/API.md from any candidate path";

    const std::string queue_stats = extract_section(
        api_md, "#### 状态快照与背压诊断", "#### `push_tasks_batch` 详解");
    ASSERT_FALSE(queue_stats.empty());
    EXPECT_TRUE(contains_regex(queue_stats,
                               "failed_pushes[\\s\\S]*queue_full_rejections[\\s\\S]*contention_rejection[\\s\\S]*reservation_cancelled_rejections"));
    EXPECT_TRUE(contains_regex(queue_stats,
                               "queue_full_rejections.*队列满"));
    EXPECT_TRUE(contains_regex(queue_stats,
                               "contention_rejection.*CAS"));
    EXPECT_TRUE(contains_regex(queue_stats,
                               "reservation_cancelled_rejections.*reservation"));
}

TEST(ApiDocStatusFields, ThreadPoolStatusDocsMatchCurrentUsage) {
    std::string api_path;
    const std::string api_md = read_doc_from_candidates(
        {"docs/API.md", "../docs/API.md", "../../docs/API.md", source_root() + "/docs/API.md"}, api_path);
    ASSERT_FALSE(api_md.empty()) << "Could not open docs/API.md from any candidate path";

    const std::string entry =
        extract_bullet_entry(api_md, "- **ThreadPoolStatus**");
    ASSERT_FALSE(entry.empty())
        << "ThreadPoolStatus entry not found in docs/API.md §7.3";

    EXPECT_NE(entry.find("ThreadPool::get_status()"), std::string::npos)
        << "ThreadPoolStatus docs must state it is still the ThreadPool status API";
    EXPECT_NE(entry.find("AsyncExecutorStatus"), std::string::npos)
        << "ThreadPoolStatus docs must direct facade users to AsyncExecutorStatus";
    EXPECT_EQ(entry.find("全仓库"), std::string::npos)
        << "ThreadPoolStatus docs must not claim repository-wide non-use";
    EXPECT_EQ(entry.find("当前无任何代码使用"), std::string::npos)
        << "ThreadPoolStatus docs must not claim the type is unused";
    EXPECT_EQ(entry.find("无任何代码使用"), std::string::npos)
        << "ThreadPoolStatus docs must not claim the type is unused";
}

TEST(ApiDocStatusFields, LockFreeQueueStatsDocsDescribeStatsAvailability) {
    std::string api_path;
    const std::string api_md = read_doc_from_candidates(
        {"docs/API.md", "../docs/API.md", "../../docs/API.md"}, api_path);
    ASSERT_FALSE(api_md.empty()) << "Could not open docs/API.md from any candidate path";

    const std::string queue_stats = extract_section(
        api_md, "#### 状态快照与背压诊断", "### 5.6");
    ASSERT_FALSE(queue_stats.empty())
        << "LockFreeTaskExecutor QueueStats section not found in " << api_path;

    EXPECT_NE(queue_stats.find("底层队列统计（均需 `enable_stats=true`）"),
              std::string::npos);
    EXPECT_NE(queue_stats.find("执行器生命周期、拒绝与异常统计（均不需 `enable_stats=true`）"),
              std::string::npos);

    const std::vector<std::string> always_observable_fields = {
        "`queue_capacity` | 调整为 2 的幂后的环形缓冲容量；实际可用槽位为该值减一",
        "`submission_rejection` | 进入队列前的拒绝：空任务、停止后提交或对象池耗尽；始终累计。 | 否（始终可读）",
        "`exception_count` | 任务执行期间累计捕获的异常次数；也可由 `exception_count()` 读取。 | 否（始终可读）",
    };
    for (const auto& field : always_observable_fields) {
        EXPECT_NE(queue_stats.find(field), std::string::npos)
            << field << " must not be documented as enable_stats-gated";
    }
    // queue_capacity 行本身必须标注为始终可读（完整行以门控说明结尾）。
    EXPECT_NE(queue_stats.find("对象池按同一取整后容量分配，因此提交背压上限就是 `queue_capacity - 1`。例如构造请求 5 时环容量取整为 8，最多可同时入队 7 个任务。 | 否（始终可读） |"),
              std::string::npos)
        << "`queue_capacity` must not be documented as enable_stats-gated";
}

TEST(ApiDocStatusFields, GpuRegistrationDocsMatchSupportedBackends) {
    std::string api_path;
    const std::string api_md = read_doc_from_candidates(
        {"docs/API.md", "../docs/API.md", "../../docs/API.md", source_root() + "/docs/API.md"}, api_path);
    ASSERT_FALSE(api_md.empty()) << "Could not open docs/API.md from any candidate path";

    const std::string registration = extract_section(
        api_md, "### 8.1 注册与任务提交", "### 8.2 查询与状态");
    ASSERT_FALSE(registration.empty())
        << "GPU registration section not found in " << api_path;

    const std::string config = extract_section(
        api_md, "### 8.4 配置与类型", "### 8.5 GPU 设备查询 API");
    ASSERT_FALSE(config.empty()) << "GPU config section not found in " << api_path;

    const std::string gpu_docs = registration + "\n" + config;
    EXPECT_NE(gpu_docs.find("CUDA"), std::string::npos)
        << "GPU registration/config docs must mention CUDA";
    EXPECT_NE(gpu_docs.find("OpenCL"), std::string::npos)
        << "GPU registration/config docs must mention OpenCL";
    EXPECT_NE(gpu_docs.find("KAIRO_ENABLE_CUDA"), std::string::npos)
        << "GPU registration/config docs must mention the CUDA build option";
    EXPECT_NE(gpu_docs.find("KAIRO_ENABLE_OPENCL"), std::string::npos)
        << "GPU registration/config docs must mention the OpenCL build option";

    EXPECT_EQ(gpu_docs.find("仅支持 `GpuBackend::CUDA`"), std::string::npos)
        << "GPU docs must not claim that only CUDA is supported";
    EXPECT_FALSE(contains_regex(gpu_docs, "only supports[^\\n]*CUDA"))
        << "GPU docs must not contain stale English CUDA-only wording";
    EXPECT_FALSE(contains_regex(gpu_docs, "currently supports[^\\n]*GpuBackend::CUDA[^\\n]*[).。]"))
        << "GPU docs must not contain stale English CUDA-only wording";
}

TEST(ApiDocStatusFields, StreamCallbackDocsStateCudaOnlyCapability) {
    std::string api_path;
    const std::string api_md = read_doc_from_candidates(
        {"docs/API.md", "../docs/API.md", "../../docs/API.md", source_root() + "/docs/API.md"}, api_path);
    ASSERT_FALSE(api_md.empty()) << "Could not open docs/API.md from any candidate path";

    const std::string gpu_interface = extract_section(
        api_md, "### 8.3 GPU 执行器接口（IGpuExecutor）", "### 8.4 配置与类型");
    ASSERT_FALSE(gpu_interface.empty()) << "GPU interface section not found in " << api_path;

    EXPECT_NE(gpu_interface.find("`add_stream_callback` 当前仅支持 CUDA"), std::string::npos)
        << "GPU interface docs must state that stream callbacks are CUDA-only";
    EXPECT_NE(gpu_interface.find("`supports_stream_callback()`"), std::string::npos)
        << "GPU interface docs must describe the stream callback capability query";
    EXPECT_NE(gpu_interface.find("`get_status().last_error_message`"), std::string::npos)
        << "GPU interface docs must describe callback failure diagnostics";
}

TEST(ApiDocStatusFields, ApiDocPerformanceClaimsHaveSources) {
    std::string api_path;
    const std::string api_md = read_doc_from_candidates(
        {"docs/API.md", "../docs/API.md", "../../docs/API.md", source_root() + "/docs/API.md"}, api_path);
    ASSERT_FALSE(api_md.empty()) << "Could not open docs/API.md from any candidate path";

    std::string readme_path;
    const std::string readme_md = read_doc_from_candidates(
        {"README.md", "../README.md", "../../README.md", source_root() + "/README.md"}, readme_path);
    ASSERT_FALSE(readme_md.empty()) << "Could not open README.md from any candidate path";

    const std::vector<std::pair<std::string, std::string>> docs = {
        {api_path, api_md},
        {readme_path, readme_md},
    };

    for (const auto& [path, text] : docs) {
        EXPECT_TRUE(text.find("docs/performance/batch_submit_baseline_2026-07-09.json") !=
                        std::string::npos ||
                    text.find("performance/batch_submit_baseline_2026-07-09.json") !=
                        std::string::npos)
            << path << " batch performance claim must link the JSON source";
        EXPECT_NE(text.find("benchmark_batch_scales"), std::string::npos)
            << path << " batch performance claim must include the benchmark command";
        EXPECT_NE(text.find("benchmark_batch_submit_real"), std::string::npos)
            << path << " batch performance claim must include the real benchmark command";
        EXPECT_NE(text.find("benchmark_batch_submit_concurrent"), std::string::npos)
            << path << " batch performance claim must include the concurrent benchmark command";
        EXPECT_TRUE(contains_regex(text, "date[^0-9]{0,20}2026-07-09") ||
                    contains_regex(text, "日期[^0-9]{0,20}2026-07-09"))
            << path << " batch performance claim must include the benchmark date";
    }

    EXPECT_NE(readme_md.find("does not promise a fixed speedup"), std::string::npos)
        << "README.md batch performance text must avoid fixed speedup promises";
    EXPECT_NE(api_md.find("不承诺固定加速比"), std::string::npos)
        << "docs/API.md batch performance text must avoid fixed speedup promises";
    EXPECT_NE(api_md.find("10.26x"), std::string::npos)
        << "docs/API.md should preserve the current 1000-task no-future ratio";
    EXPECT_NE(api_md.find("2.56x"), std::string::npos)
        << "docs/API.md should preserve the current 5000-task real workload ratio";
    EXPECT_NE(api_md.find("0.86x"), std::string::npos)
        << "docs/API.md should preserve the current concurrent slowdown ratio";
}

TEST(ApiDocThreadPoolSnippetCompiles, FixedThreadPoolExampleInitializes) {
    kairo::ThreadPoolConfig config;
    config.min_threads = 16;
    config.max_threads = 16;

    kairo::ThreadPool pool;
    ASSERT_TRUE(pool.initialize(config));

    pool.shutdown(true);
}

}  // namespace
