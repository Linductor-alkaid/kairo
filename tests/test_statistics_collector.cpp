#include <iostream>
#include <string>
#include "kairo/monitor/statistics_collector.hpp"

using namespace kairo::monitor;

#define TEST_ASSERT(condition, message)                                      \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":"  \
                      << __LINE__ << std::endl;                              \
            return false;                                                    \
        }                                                                    \
    } while (0)

static bool test_delegation_to_task_monitor() {
    std::cout << "Testing get_task_statistics / get_all_task_statistics..."
              << std::endl;
    StatisticsCollector sc;
    TaskMonitor& tm = sc.get_task_monitor();
    tm.record_task_start("t1", "A");
    tm.record_task_complete("t1", true, 100);
    tm.record_task_start("t2", "B");
    tm.record_task_complete("t2", false, 200);

    auto sa = sc.get_task_statistics("A");
    TEST_ASSERT(sa.total_count == 1 && sa.success_count == 1, "A stats");
    TEST_ASSERT(sa.total_execution_time_ns == 100, "A time");

    auto sb = sc.get_task_statistics("B");
    TEST_ASSERT(sb.total_count == 1 && sb.fail_count == 1, "B stats");

    auto all = sc.get_all_task_statistics();
    TEST_ASSERT(all.size() == 2 && all.count("A") && all.count("B"),
                "get_all");
    TEST_ASSERT(all.at("A").total_count == 1 && all.at("B").total_count == 1,
                "all counts");
    std::cout << "  delegation: PASSED" << std::endl;
    return true;
}

static bool test_unknown_type_returns_zeros() {
    std::cout << "Testing unknown task_type returns zeros..." << std::endl;
    StatisticsCollector sc;
    auto s = sc.get_task_statistics("nonexistent");
    TEST_ASSERT(s.total_count == 0 && s.success_count == 0 && s.fail_count == 0,
                "unknown zeros");
    auto all = sc.get_all_task_statistics();
    TEST_ASSERT(all.empty(), "all empty when no records");
    std::cout << "  unknown zeros: PASSED" << std::endl;
    return true;
}

int main() {
    std::cout << "=== StatisticsCollector unit tests ===" << std::endl;
    bool ok = test_delegation_to_task_monitor() && test_unknown_type_returns_zeros();
    std::cout << (ok ? "=== All StatisticsCollector tests PASSED ==="
                     : "=== Some tests FAILED ===")
              << std::endl;
    return ok ? 0 : 1;
}
