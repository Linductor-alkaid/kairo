#include <iostream>
#include <string>

#include <kairo/executor.hpp>

int main() {
    kairo::Executor executor;
    kairo::ExecutorConfig config;
    config.min_threads = 1;
    config.max_threads = 1;

    if (!executor.initialize_ex(config)) {
        return 1;
    }

    auto analysis = executor.submit_priority(0, [] { return std::string{"analysis"}; });
    auto control = executor.submit_priority(2, [] { return std::string{"control"}; });
    const auto analysis_result = analysis.get();
    const auto control_result = control.get();

    std::cout << "priority tasks=" << analysis_result << "," << control_result << '\n';
    executor.shutdown();
    return analysis_result == "analysis" && control_result == "control" ? 0 : 1;
}
