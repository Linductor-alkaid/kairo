#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include "kairo/monitor/task_monitor.hpp"

using namespace kairo::monitor;

#define TEST_ASSERT(condition, message)                                      \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":"  \
                      << __LINE__ << std::endl;                              \
            return false;                                                    \
        }                                                                    \
    } while (0)

static bool test_record_start_complete_success() {
    std::cout << "Testing record_task_start / record_task_complete (success)..."
              << std::endl;
    TaskMonitor m;
    m.record_task_start("t1", "type_a");
    m.record_task_complete("t1", true, 100);
    auto s = m.get_statistics("type_a");
    TEST_ASSERT(s.total_count == 1, "total_count should be 1");
    TEST_ASSERT(s.success_count == 1, "success_count should be 1");
    TEST_ASSERT(s.fail_count == 0, "fail_count should be 0");
    TEST_ASSERT(s.timeout_count == 0, "timeout_count should be 0");
    TEST_ASSERT(s.total_execution_time_ns == 100, "total_execution_time_ns");
    TEST_ASSERT(s.max_execution_time_ns == 100, "max_execution_time_ns");
    TEST_ASSERT(s.min_execution_time_ns == 100, "min_execution_time_ns");
    std::cout << "  record start/complete success: PASSED" << std::endl;
    return true;
}

static bool test_record_start_complete_fail() {
    std::cout << "Testing record_task_complete (fail)..." << std::endl;
    TaskMonitor m;
    m.record_task_start("t1", "type_b");
    m.record_task_complete("t1", false, 200);
    auto s = m.get_statistics("type_b");
    TEST_ASSERT(s.total_count == 1, "total_count should be 1");
    TEST_ASSERT(s.success_count == 0, "success_count should be 0");
    TEST_ASSERT(s.fail_count == 1, "fail_count should be 1");
    TEST_ASSERT(s.total_execution_time_ns == 200, "total_execution_time_ns");
    std::cout << "  record complete fail: PASSED" << std::endl;
    return true;
}

static bool test_record_timeout() {
    std::cout << "Testing record_task_timeout..." << std::endl;
    TaskMonitor m;
    m.record_task_start("t1", "type_c");
    m.record_task_timeout("t1");
    auto s = m.get_statistics("type_c");
    TEST_ASSERT(s.total_count == 1, "total_count should be 1");
    TEST_ASSERT(s.timeout_count == 1, "timeout_count should be 1");
    TEST_ASSERT(s.success_count == 0 && s.fail_count == 0,
                "success/fail should be 0");
    std::cout << "  record timeout: PASSED" << std::endl;
    return true;
}

static bool test_multiple_task_types() {
    std::cout << "Testing multiple task types..." << std::endl;
    TaskMonitor m;
    m.record_task_start("a1", "type_a");
    m.record_task_complete("a1", true, 10);
    m.record_task_start("a2", "type_a");
    m.record_task_complete("a2", true, 20);
    m.record_task_start("b1", "type_b");
    m.record_task_complete("b1", false, 30);

    auto sa = m.get_statistics("type_a");
    TEST_ASSERT(sa.total_count == 2 && sa.success_count == 2, "type_a stats");
    TEST_ASSERT(sa.total_execution_time_ns == 30, "type_a total time");
    TEST_ASSERT(sa.max_execution_time_ns == 20 && sa.min_execution_time_ns == 10,
                "type_a min/max");

    auto sb = m.get_statistics("type_b");
    TEST_ASSERT(sb.total_count == 1 && sb.fail_count == 1, "type_b stats");

    auto all = m.get_all_statistics();
    TEST_ASSERT(all.size() == 2, "get_all_statistics size");
    TEST_ASSERT(all.count("type_a") && all.count("type_b"), "all has both");
    std::cout << "  multiple task types: PASSED" << std::endl;
    return true;
}

static bool test_get_statistics_unknown_type() {
    std::cout << "Testing get_statistics unknown type..." << std::endl;
    TaskMonitor m;
    auto s = m.get_statistics("nonexistent");
    TEST_ASSERT(s.total_count == 0 && s.success_count == 0 && s.fail_count == 0,
                "unknown type returns zeros");
    std::cout << "  get_statistics unknown: PASSED" << std::endl;
    return true;
}

static bool test_complete_without_start_ignored() {
    std::cout << "Testing record_task_complete without start ignored..."
              << std::endl;
    TaskMonitor m;
    m.record_task_complete("no_start", true, 100);
    auto s = m.get_statistics("default");
    TEST_ASSERT(s.total_count == 0, "complete without start should not add");
    std::cout << "  complete without start ignored: PASSED" << std::endl;
    return true;
}

static bool test_enabled_disable() {
    std::cout << "Testing set_enabled / is_enabled..." << std::endl;
    TaskMonitor m;
    TEST_ASSERT(m.is_enabled(), "default enabled");
    m.set_enabled(false);
    TEST_ASSERT(!m.is_enabled(), "disabled");
    m.record_task_start("t1", "x");
    m.record_task_complete("t1", true, 1);
    auto s = m.get_statistics("x");
    TEST_ASSERT(s.total_count == 0, "no record when disabled");
    m.set_enabled(true);
    m.record_task_start("t2", "x");
    m.record_task_complete("t2", true, 2);
    s = m.get_statistics("x");
    TEST_ASSERT(s.total_count == 1, "record when re-enabled");
    std::cout << "  set_enabled / is_enabled: PASSED" << std::endl;
    return true;
}

static bool test_default_task_type() {
    std::cout << "Testing default task_type..." << std::endl;
    TaskMonitor m;
    m.record_task_start("t1");  // default "default"
    m.record_task_complete("t1", true, 42);
    auto s = m.get_statistics("default");
    TEST_ASSERT(s.total_count == 1 && s.total_execution_time_ns == 42,
                "default type");
    std::cout << "  default task_type: PASSED" << std::endl;
    return true;
}

static bool test_concurrent() {
    std::cout << "Testing concurrent record_* and get_*..." << std::endl;
    TaskMonitor m;
    std::vector<std::thread> writers;
    const int num_writers = 4;
    const int tasks_per_writer = 200;
    for (int w = 0; w < num_writers; ++w) {
        writers.emplace_back([&m, w, tasks_per_writer]() {
            for (int i = 0; i < tasks_per_writer; ++i) {
                std::string id =
                    "w" + std::to_string(w) + "_" + std::to_string(i);
                std::string type = "type_" + std::to_string(w % 2);
                m.record_task_start(id, type);
                m.record_task_complete(id, (i % 2) == 0, 100 + (i % 50));
            }
        });
    }
    for (auto& t : writers) t.join();
    auto all = m.get_all_statistics();
    TEST_ASSERT(all.size() <= 2, "at most type_0 and type_1");
    int64_t total = 0;
    for (const auto& kv : all) total += kv.second.total_count;
    TEST_ASSERT(total == num_writers * tasks_per_writer, "total concurrent");
    std::cout << "  concurrent: PASSED" << std::endl;
    return true;
}

int main() {
    std::cout << "=== TaskMonitor unit tests ===" << std::endl;
    bool ok = true;
    ok = test_record_start_complete_success() && ok;
    ok = test_record_start_complete_fail() && ok;
    ok = test_record_timeout() && ok;
    ok = test_multiple_task_types() && ok;
    ok = test_get_statistics_unknown_type() && ok;
    ok = test_complete_without_start_ignored() && ok;
    ok = test_enabled_disable() && ok;
    ok = test_default_task_type() && ok;
    ok = test_concurrent() && ok;
    std::cout << (ok ? "=== All TaskMonitor tests PASSED ==="
                     : "=== Some tests FAILED ===")
              << std::endl;
    return ok ? 0 : 1;
}
