#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <kairo/executor.hpp>

int main() {
    kairo::Executor executor;
    kairo::ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 2;
    config.queue_capacity = 32;
    if (!executor.initialize_ex(config)) {
        return 1;
    }

    std::atomic<int> failure_callbacks{0};
    executor.set_failure_callback([&](const kairo::ExecutorFailureEvent& event) noexcept {
        if (event.kind == kairo::FailureKind::TaskException) {
            ++failure_callbacks;
        }
    });

    auto schema = executor.submit_with_handle([] { return std::string{"schema-v1"}; });
    auto destination = executor.submit_with_handle([] { return std::string{"orders"}; });
    const auto prerequisites = executor.when_all({schema.handle, destination.handle});
    auto prepared = executor.submit_after(prerequisites, []() noexcept { return true; });

    const std::string schema_name = schema.future.get();
    const std::string table_name = destination.future.get();
    const bool is_prepared = prepared.get();

    const std::vector<std::string> rows{
        "order-1001", "order-1002", "", "order-1004"};
    std::atomic<int> imported{0};
    std::vector<std::function<void()>> imports;
    imports.reserve(rows.size());
    for (const auto& row : rows) {
        imports.push_back([row, &imported] {
            if (row.empty()) {
                throw std::invalid_argument("missing order id");
            }
            ++imported;
        });
    }

    auto futures = executor.submit_batch(imports);
    int rejected = 0;
    for (auto& future : futures) {
        try {
            future.get();
        } catch (const std::exception&) {
            ++rejected;
        }
    }

    const auto drained =
        executor.wait_for_completion_ex(std::chrono::seconds{1});
    std::cout << "prepared=" << (is_prepared ? "yes" : "no")
              << ", schema=" << schema_name
              << ", table=" << table_name << '\n';
    std::cout << "imported=" << imported.load()
              << ", rejected=" << rejected
              << ", callbacks=" << failure_callbacks.load()
              << ", drained=" << (drained.completed ? "yes" : "no") << '\n';

    executor.shutdown();
    return is_prepared && schema_name == "schema-v1" && table_name == "orders" &&
                   imported == 3 && rejected == 1 && failure_callbacks == 1 &&
                   drained.completed
               ? 0
               : 1;
}
