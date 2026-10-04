#include <chrono>
#include <kairo/executor.hpp>
#include <future>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

using namespace kairo;

static void print_task_statistics(const std::string& label,
                                  const TaskStatistics& s) {
    double total_ms = 1e-6 * static_cast<double>(s.total_execution_time_ns);
    double max_ms = 1e-6 * static_cast<double>(s.max_execution_time_ns);
    double min_ms = 1e-6 * static_cast<double>(s.min_execution_time_ns);
    double avg_ms = (s.total_count > 0) ? (total_ms / static_cast<double>(s.total_count)) : 0.0;

    std::cout << "  " << label << ":\n";
    std::cout << "    total_count        = " << s.total_count << "\n";
    std::cout << "    success_count      = " << s.success_count << "\n";
    std::cout << "    fail_count         = " << s.fail_count << "\n";
    std::cout << "    timeout_count      = " << s.timeout_count << "\n";
    std::cout << "    total_execution_ms = " << std::fixed << std::setprecision(3)
              << total_ms << "\n";
    std::cout << "    avg_execution_ms   = " << avg_ms << "\n";
    std::cout << "    max_execution_ms   = " << max_ms << "\n";
    std::cout << "    min_execution_ms   = " << min_ms << "\n";
}

int main() {
    std::cout << "========================================\n";
    std::cout << "Executor 监控数据示例\n";
    std::cout << "========================================\n\n";

    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    config.enable_monitoring = true;

    Executor exec;
    if (!exec.initialize(config)) {
        std::cerr << "初始化失败\n";
        return 1;
    }
    std::cout << "Executor 已初始化，监控已开启\n\n";

    // 提交一批任务：部分立即返回，部分 sleep（若提交会抛异常的任务，fail_count 将增加）
    const int n_quick = 20;
    const int n_slow = 5;
    std::vector<std::future<void>> futures;

    for (int i = 0; i < n_quick; ++i) {
        futures.push_back(exec.submit([i]() { (void)i; }));
    }
    for (int i = 0; i < n_slow; ++i) {
        futures.push_back(exec.submit([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }));
    }

    for (auto& f : futures) f.get();
    (void)exec.wait_for_completion(std::chrono::seconds{300});

    std::cout << "任务已执行完毕，查询监控数据：\n\n";

    // ---------- get_task_statistics("default") ----------
    std::cout << "1. get_task_statistics(\"default\")  （按 task_type 聚合）\n";
    TaskStatistics stats_default = exec.get_task_statistics("default");
    print_task_statistics("default", stats_default);
    std::cout << "\n";

    // ---------- get_all_task_statistics() ----------
    std::cout << "2. get_all_task_statistics()  （全部 task_type）\n";
    auto all = exec.get_all_task_statistics();
    for (const auto& [task_type, s] : all) {
        print_task_statistics("task_type = \"" + task_type + "\"", s);
    }
    if (all.empty()) {
        std::cout << "  （无记录）\n";
    }
    std::cout << "\n";

    // ---------- 未知 task_type 返回全 0 ----------
    std::cout << "3. get_task_statistics(\"unknown\")  （不存在的 type）\n";
    TaskStatistics stats_unknown = exec.get_task_statistics("unknown");
    print_task_statistics("unknown", stats_unknown);
    std::cout << "\n";

    // ---------- 异步执行器状态（池级）对比 ----------
    std::cout << "4. get_async_executor_status()  （线程池状态，非按 type）\n";
    auto async_status = exec.get_async_executor_status();
    std::cout << "  name           = " << async_status.name << "\n";
    std::cout << "  is_running     = " << (async_status.is_running ? "true" : "false") << "\n";
    std::cout << "  active_tasks   = " << async_status.active_tasks << "\n";
    std::cout << "  completed_tasks= " << async_status.completed_tasks << "\n";
    std::cout << "  failed_tasks   = " << async_status.failed_tasks << "\n";
    std::cout << "  queue_size     = " << async_status.queue_size << "\n";
    std::cout << "  avg_task_time_ms = " << std::fixed << std::setprecision(3)
              << async_status.avg_task_time_ms << "\n";
    std::cout << "\n";

    exec.shutdown();
    std::cout << "========================================\n";
    std::cout << "示例结束\n";
    std::cout << "========================================\n";
    return 0;
}
