#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <kairo/executor.hpp>

using namespace kairo;

#define TEST_ASSERT(condition, message)                                      \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::cerr << "FAILED: " << message << " at " << __FILE__ << ":"  \
                      << __LINE__ << std::endl;                              \
            return false;                                                    \
        }                                                                    \
    } while (0)

static bool test_monitoring_enabled_submit_and_query() {
    std::cout << "Testing enable_monitoring(true), submit, get_task_statistics..."
              << std::endl;
    Executor exec;
    ExecutorConfig cfg;
    cfg.min_threads = 2;
    cfg.max_threads = 4;
    cfg.enable_monitoring = true;
    TEST_ASSERT(exec.initialize(cfg), "initialize");

    exec.enable_monitoring(true);
    const int n = 50;
    std::vector<std::future<void>> futures;
    futures.reserve(n);
    for (int i = 0; i < n; ++i) {
        futures.push_back(exec.submit([i]() { (void)i; }));
    }
    for (auto& f : futures) f.get();
    (void)exec.wait_for_completion(std::chrono::seconds{300});

    TaskStatistics s = exec.get_task_statistics("default");
    TEST_ASSERT(s.total_count >= static_cast<int64_t>(n),
                "default total_count >= n");
    TEST_ASSERT(s.success_count >= static_cast<int64_t>(n), "success_count >= n");

    auto all = exec.get_all_task_statistics();
    TEST_ASSERT(all.count("default"), "all has default");
    TEST_ASSERT(all.at("default").total_count >= static_cast<int64_t>(n),
                "all default count");

    exec.shutdown();
    std::cout << "  monitoring enabled + query: PASSED" << std::endl;
    return true;
}

static bool test_monitoring_disabled_no_count() {
    std::cout << "Testing enable_monitoring(false), submit, no new counts..."
              << std::endl;
    Executor exec;
    ExecutorConfig cfg;
    cfg.min_threads = 2;
    cfg.max_threads = 4;
    TEST_ASSERT(exec.initialize(cfg), "initialize");

    exec.enable_monitoring(true);
    exec.submit([]() {}).get();
    (void)exec.wait_for_completion(std::chrono::seconds{300});
    TaskStatistics before = exec.get_task_statistics("default");
    int64_t count_before = before.total_count;
    TEST_ASSERT(count_before >= 1, "at least one task when enabled");

    exec.enable_monitoring(false);
    std::vector<std::future<void>> fs;
    for (int i = 0; i < 20; ++i) {
        fs.push_back(exec.submit([]() {}));
    }
    for (auto& f : fs) f.get();
    (void)exec.wait_for_completion(std::chrono::seconds{300});
    TaskStatistics after = exec.get_task_statistics("default");
    TEST_ASSERT(after.total_count == count_before,
                "total_count unchanged when monitoring disabled");

    exec.shutdown();
    std::cout << "  monitoring disabled no count: PASSED" << std::endl;
    return true;
}

static bool test_config_enable_monitoring_initial() {
    std::cout << "Testing ExecutorConfig::enable_monitoring initial..."
              << std::endl;
    Executor exec;
    ExecutorConfig cfg;
    cfg.min_threads = 2;
    cfg.max_threads = 4;
    cfg.enable_monitoring = false;
    TEST_ASSERT(exec.initialize(cfg), "initialize");

    {
        std::vector<std::future<void>> fs;
        for (int i = 0; i < 10; ++i) {
            fs.push_back(exec.submit([]() {}));
        }
        for (auto& f : fs) f.get();
    }
    (void)exec.wait_for_completion(std::chrono::seconds{300});
    TaskStatistics s = exec.get_task_statistics("default");
    TEST_ASSERT(s.total_count == 0,
                "no counts when config enable_monitoring=false");

    exec.enable_monitoring(true);
    exec.submit([]() {}).get();
    (void)exec.wait_for_completion(std::chrono::seconds{300});
    s = exec.get_task_statistics("default");
    TEST_ASSERT(s.total_count >= 1, "counts after enable_monitoring(true)");

    exec.shutdown();
    std::cout << "  config enable_monitoring initial: PASSED" << std::endl;
    return true;
}

static bool test_unknown_task_type_zeros() {
    std::cout << "Testing get_task_statistics unknown type..." << std::endl;
    Executor exec;
    ExecutorConfig cfg;
    cfg.min_threads = 2;
    cfg.max_threads = 4;
    TEST_ASSERT(exec.initialize(cfg), "initialize");

    TaskStatistics s = exec.get_task_statistics("nonexistent");
    TEST_ASSERT(s.total_count == 0 && s.success_count == 0 && s.fail_count == 0,
                "unknown type zeros");

    exec.shutdown();
    std::cout << "  unknown task type zeros: PASSED" << std::endl;
    return true;
}

int main() {
    std::cout << "=== Monitor integration tests ===" << std::endl;
    bool ok = true;
    ok = test_monitoring_enabled_submit_and_query() && ok;
    ok = test_monitoring_disabled_no_count() && ok;
    ok = test_config_enable_monitoring_initial() && ok;
    ok = test_unknown_task_type_zeros() && ok;
    std::cout << (ok ? "=== All monitor integration tests PASSED ==="
                     : "=== Some tests FAILED ===")
              << std::endl;
    return ok ? 0 : 1;
}
