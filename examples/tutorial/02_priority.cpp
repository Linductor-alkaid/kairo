// 0.7.2：优先级是调度策略维度，经 submit_auto(TaskBuilder) 的 priority()
// 表达（submit_priority 已弃用）。见 docs/MIGRATION.md 0.7.2 节。
#include <iostream>
#include <string>

#include <kairo/executor.hpp>
#include <kairo/task_options.hpp>

int main() {
    kairo::Executor executor;
    kairo::ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;

    if (!executor.initialize(config)) {
        return 1;
    }

    auto analysis = executor.submit_auto(
        kairo::TaskBuilder([] { return std::string{"analysis"}; })
            .priority(kairo::TaskPriority::LOW));
    auto control = executor.submit_auto(
        kairo::TaskBuilder([] { return std::string{"control"}; })
            .priority(kairo::TaskPriority::HIGH));
    const auto analysis_result = analysis.get();
    const auto control_result = control.get();

    std::cout << "priority tasks=" << analysis_result << "," << control_result << '\n';
    executor.shutdown();
    return analysis_result == "analysis" && control_result == "control" ? 0 : 1;
}
