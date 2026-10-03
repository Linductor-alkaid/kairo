/**
 * Task-graph scheduling path benchmark (PA-6 acceptance of the 2026-09
 * performance audit, dependency-driven scheduling PR-3).
 *
 * Measures, against the Executor facade (tracked/task-graph paths):
 *   1. tracked submit+complete throughput at 1/2/4/8 producer threads
 *      (submit_with_handle; graph bookkeeping included).
 *   2. parked fan-out release latency: W submit_after dependents parked on
 *      one gate dependency, wall time from gate release to all completions
 *      (directed O(1) enqueue replaces the former notify_all thundering herd).
 *   3. dependency chain completion latency: K-link submit_after chain,
 *      per-hop scheduling cost.
 * Also prints closure_graveyard_size() (expected 0 after clean runs).
 *
 * Config: env KAIRO_BENCHMARK_* + CLI (--json, --tasks, --producers).
 * Output: human-readable text (default) or JSON lines (--json).
 */

#include <kairo/executor.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using kairo::Executor;
using kairo::ExecutorConfig;
using kairo::TaskHandle;

namespace {

constexpr size_t kDefaultTasksPerProducer = 20000;
constexpr size_t kDefaultFanout = 256;
constexpr size_t kDefaultChainLength = 256;

struct Config {
    size_t tasks_per_producer = kDefaultTasksPerProducer;
    size_t fanout = kDefaultFanout;
    size_t chain_length = kDefaultChainLength;
    bool json_output = false;
};

size_t parse_size_t(const char* s, size_t default_val) {
    if (!s || !*s) return default_val;
    try {
        unsigned long v = std::stoul(s);
        return static_cast<size_t>(v);
    } catch (...) {
        return default_val;
    }
}

void apply_env(Config& c) {
    if (const char* t = std::getenv("KAIRO_BENCHMARK_TASKS")) {
        c.tasks_per_producer = parse_size_t(t, c.tasks_per_producer);
    }
    if (const char* t = std::getenv("KAIRO_BENCHMARK_FANOUT")) {
        c.fanout = parse_size_t(t, c.fanout);
    }
    if (const char* t = std::getenv("KAIRO_BENCHMARK_CHAIN")) {
        c.chain_length = parse_size_t(t, c.chain_length);
    }
    if (const char* t = std::getenv("KAIRO_BENCHMARK_JSON")) {
        if (t[0] == '1' || t[0] == 't' || t[0] == 'T') c.json_output = true;
    }
}

void parse_args(int argc, char* argv[], Config& c) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--json") {
            c.json_output = true;
        } else if (a == "--tasks" && i + 1 < argc) {
            c.tasks_per_producer = parse_size_t(argv[++i], c.tasks_per_producer);
        } else if (a == "--fanout" && i + 1 < argc) {
            c.fanout = parse_size_t(argv[++i], c.fanout);
        } else if (a == "--chain" && i + 1 < argc) {
            c.chain_length = parse_size_t(argv[++i], c.chain_length);
        }
    }
}

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now() - start)
        .count();
}

ExecutorConfig make_config(unsigned workers) {
    ExecutorConfig ec;
    ec.min_threads = workers;
    ec.max_threads = workers;
    return ec;
}

// 场景 1：tracked 提交+完成吞吐（含图簿记）。P 生产者各提交 N 个
// submit_with_handle 任务，等待全部完成。
double benchmark_tracked_throughput(unsigned producers, size_t per_producer) {
    Executor executor;
    if (!executor.initialize(make_config(producers))) {
        return 0.0;
    }
    const size_t total = static_cast<size_t>(producers) * per_producer;
    std::vector<std::future<int>> futures;
    futures.reserve(total);
    const auto start = std::chrono::steady_clock::now();
    for (unsigned p = 0; p < producers; ++p) {
        for (size_t i = 0; i < per_producer; ++i) {
            futures.push_back(
                executor.submit_with_handle([]() noexcept { return 1; }).future);
        }
    }
    for (auto& f : futures) {
        if (f.wait_for(std::chrono::seconds(120)) != std::future_status::ready) {
            return 0.0;
        }
    }
    const double elapsed = seconds_since(start);
    executor.shutdown();
    return (elapsed > 0.0) ? static_cast<double>(total) / elapsed : 0.0;
}

// 场景 2：parked fan-out 释放——W 个 submit_after 依赖停驻在同一门任务上，
// 释放门后量测到全部完成的时间（定向 O(1) 入队替代 notify_all 惊群）。
double benchmark_parked_fanout(unsigned workers, size_t fanout) {
    Executor executor;
    if (!executor.initialize(make_config(workers))) {
        return 0.0;
    }
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    auto gate = executor.submit_with_handle([&]() noexcept {
        entered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return 1;
    });
    while (!entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::vector<std::future<int>> dependents;
    dependents.reserve(fanout);
    for (size_t i = 0; i < fanout; ++i) {
        dependents.push_back(
            executor.submit_after(gate.handle, []() noexcept { return 2; }));
    }

    const auto start = std::chrono::steady_clock::now();
    release.store(true, std::memory_order_release);
    for (auto& f : dependents) {
        if (f.wait_for(std::chrono::seconds(120)) != std::future_status::ready) {
            return 0.0;
        }
    }
    if (gate.future.wait_for(std::chrono::seconds(120)) !=
        std::future_status::ready) {
        return 0.0;
    }
    const double elapsed = seconds_since(start);
    executor.shutdown();
    return elapsed * 1000.0;  // ms
}

// 场景 3：K 链完成延迟——K 段 submit_after 链的总耗时与每跳成本。
double benchmark_dependency_chain(unsigned workers, size_t chain_length,
                                  double& per_hop_us) {
    Executor executor;
    if (!executor.initialize(make_config(workers))) {
        return 0.0;
    }
    auto first = executor.submit_with_handle([]() noexcept { return 0; });
    TaskHandle prev = first.handle;
    std::vector<std::future<int>> links;
    links.reserve(chain_length);
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < chain_length; ++i) {
        auto link = executor.submit_after_with_handle(
            prev, []() noexcept { return 1; });
        prev = link.handle;
        links.push_back(std::move(link.future));
    }
    for (auto& f : links) {
        if (f.wait_for(std::chrono::seconds(120)) != std::future_status::ready) {
            return 0.0;
        }
    }
    const double elapsed = seconds_since(start);
    per_hop_us = (elapsed * 1e6) / static_cast<double>(chain_length);
    executor.shutdown();
    return elapsed * 1000.0;  // ms
}

}  // namespace

int main(int argc, char* argv[]) {
    Config config;
    apply_env(config);
    parse_args(argc, argv, config);

    std::cout << "task-graph scheduling path benchmark (PA-6)\n";
    std::cout << "tasks/producer=" << config.tasks_per_producer
              << " fanout=" << config.fanout
              << " chain=" << config.chain_length << "\n\n";

    if (!config.json_output) {
        std::cout << std::fixed << std::setprecision(1);
        for (unsigned producers : {1u, 2u, 4u, 8u}) {
            const double ops = benchmark_tracked_throughput(
                producers, config.tasks_per_producer);
            std::cout << "tracked_submit_throughput producers=" << producers
                      << ": " << ops << " ops/s\n";
        }
        std::cout << "\n";
        for (size_t fanout : {size_t{64}, config.fanout}) {
            const double ms = benchmark_parked_fanout(4, fanout);
            std::cout << "parked_fanout_release fanout=" << fanout << ": " << ms
                      << " ms (" << (ms * 1000.0 / static_cast<double>(fanout))
                      << " us/dependent)\n";
        }
        std::cout << "\n";
        for (size_t chain : {size_t{64}, config.chain_length}) {
            double per_hop_us = 0;
            const double ms = benchmark_dependency_chain(2, chain, per_hop_us);
            std::cout << "dependency_chain links=" << chain << ": " << ms
                      << " ms (" << per_hop_us << " us/hop)\n";
        }
        std::cout << "\n";
    } else {
        std::cout << "{\"type\":\"task_graph_paths\",\"sections\":[\n";
        for (unsigned producers : {1u, 2u, 4u, 8u}) {
            const double ops = benchmark_tracked_throughput(
                producers, config.tasks_per_producer);
            std::cout << "{\"section\":\"tracked_submit_throughput\","
                      << "\"producers\":" << producers << ",\"ops_per_s\":"
                      << ops << "},\n";
        }
        for (size_t fanout : {size_t{64}, config.fanout}) {
            const double ms = benchmark_parked_fanout(4, fanout);
            std::cout << "{\"section\":\"parked_fanout_release\","
                      << "\"fanout\":" << fanout << ",\"ms\":" << ms << "},\n";
        }
        for (size_t chain : {size_t{64}, config.chain_length}) {
            double per_hop_us = 0;
            const double ms = benchmark_dependency_chain(2, chain, per_hop_us);
            std::cout << "{\"section\":\"dependency_chain\",\"links\":" << chain
                      << ",\"ms\":" << ms << ",\"per_hop_us\":" << per_hop_us
                      << "},\n";
        }
        std::cout << "]}\n";
    }

    // 墓地观测：无超时竞争的干净运行应为 0。
    {
        Executor executor;
        if (executor.initialize(make_config(2))) {
            std::cout << "closure_graveyard_size: "
                      << executor.closure_graveyard_size() << " (expected 0)\n";
            executor.shutdown();
            std::cout << "closure_graveyard_size after shutdown: "
                      << executor.closure_graveyard_size() << " (expected 0)\n";
        }
    }
    return 0;
}
