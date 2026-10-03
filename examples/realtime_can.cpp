/**
 * @file realtime_can.cpp
 * @brief CAN 通信实时线程示例，展示 ICycleManager 周期管理器使用
 *
 * 本示例演示如何：
 * 1. 实现简单的 ICycleManager（SimpleCycleManager，基于 sleep_until）
 * 2. 将周期管理器注入 RealtimeThreadConfig
 * 3. 注册并运行实时任务（模拟 CAN 通道周期读写）
 * 4. 使用 Executor::push_realtime_task 向实时线程提交任务
 * 5. 查询周期统计、背压计数并停止任务
 */

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <kairo/executor.hpp>
#include <kairo/interfaces.hpp>
#include <kairo/types.hpp>

using namespace kairo;

// ========== SimpleCycleManager：基于 sleep_until 的 ICycleManager 实现 ==========

class SimpleCycleManager : public ICycleManager {
public:
    struct CycleInfo {
        std::string name;
        int64_t period_ns = 0;
        std::function<void()> callback;
    };

    bool register_cycle(const std::string& name, int64_t period_ns,
                       std::function<void()> callback) override {
        std::lock_guard<std::mutex> lock(mutex_);
        cycles_[name] = {name, period_ns, std::move(callback)};
        stop_requested_[name] = false;
        return true;
    }

    bool start_cycle(const std::string& name) override {
        CycleInfo info;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = cycles_.find(name);
            if (it == cycles_.end()) {
                return false;
            }
            info = it->second;
            stop_requested_[name] = false;
        }

        auto next_cycle_time = std::chrono::steady_clock::now();
        const auto period_ns = std::chrono::nanoseconds(info.period_ns);

        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stop_requested_[name]) {
                    break;
                }
            }

            if (info.callback) {
                info.callback();
            }

            next_cycle_time += period_ns;
            std::this_thread::sleep_until(next_cycle_time);
        }

        return true;
    }

    void stop_cycle(const std::string& name) override {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_[name] = true;
    }

    CycleStatistics get_statistics(const std::string& name) const override {
        CycleStatistics stats;
        stats.name = name;
        return stats;
    }

private:
    std::unordered_map<std::string, CycleInfo> cycles_;
    std::unordered_map<std::string, bool> stop_requested_;
    mutable std::mutex mutex_;
};

// ========== 主示例 ==========

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Realtime CAN Example (ICycleManager)" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    // 初始化 Executor
    ExecutorConfig exec_config;
    exec_config.min_threads = 2;
    exec_config.max_threads = 4;
    exec_config.queue_capacity = 100;

    auto& exec = Executor::instance();
    auto init = exec.initialize(exec_config);
    if (!init) {
        std::cerr << "Failed to initialize executor: " << init.message << std::endl;
        return 1;
    }

    exec.set_failure_callback([](const ExecutorFailureEvent& event) {
        std::cerr << "[failure] " << event.message << std::endl;
    });

    std::cout << "Executor initialized" << std::endl;

    // 创建周期管理器（用户管理生命周期）
    SimpleCycleManager cycle_manager;

    // 模拟 CAN 通道：周期计数器
    std::atomic<int> can_cycle_count{0};

    // 配置实时任务（使用 ICycleManager）
    RealtimeThreadConfig rt_config;
    rt_config.thread_name = "can_channel_0";
    rt_config.cycle_period_ns = 2000000;  // 2 ms (500 Hz)，模拟 CAN 典型周期
    rt_config.thread_priority = 0;        // 示例环境使用普通优先级
    rt_config.cycle_manager = &cycle_manager;
    rt_config.cycle_callback = [&can_cycle_count]() {
        // 模拟 CAN 周期：读反馈、发指令
        can_cycle_count.fetch_add(1, std::memory_order_relaxed);
    };

    std::cout << "Registering realtime task with ICycleManager (2 ms cycle)..." << std::endl;

    if (!exec.register_realtime_task("can_channel_0", rt_config)) {
        std::cerr << "Failed to register realtime task" << std::endl;
        exec.shutdown();
        return 1;
    }

    std::cout << "Starting realtime task..." << std::endl;
    if (!exec.start_realtime_task("can_channel_0")) {
        std::cerr << "Failed to start realtime task" << std::endl;
        exec.shutdown();
        return 1;
    }

    // 运行一段时间
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 示例：通过 facade 向实时线程推送任务（在周期回调中执行）
    if (!exec.push_realtime_task("can_channel_0", []() {
            std::cout << "  [push_realtime_task] Task executed in realtime cycle" << std::endl;
        })) {
        std::cerr << "Failed to push realtime task" << std::endl;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 查询状态与统计
    auto status = exec.get_realtime_executor_status("can_channel_0");
    std::cout << "Realtime executor status:" << std::endl;
    std::cout << "  Name: " << status.name << std::endl;
    std::cout << "  Running: " << (status.is_running ? "yes" : "no") << std::endl;
    std::cout << "  Cycle period: " << status.cycle_period_ns << " ns" << std::endl;
    std::cout << "  Cycle count: " << status.cycle_count << std::endl;
    std::cout << "  Avg cycle time: " << status.avg_cycle_time_ns << " ns" << std::endl;
    std::cout << "  Dropped tasks: " << status.dropped_task_count << std::endl;
    std::cout << "  Queue full drops: " << status.queue_full_count << std::endl;
    std::cout << "  Pool exhausted drops: " << status.pool_exhausted_count << std::endl;
    std::cout << "  Callback invocations (can_cycle_count): " << can_cycle_count.load() << std::endl;

    // 停止实时任务（内部会调用 cycle_manager->stop_cycle）
    std::cout << "Stopping realtime task..." << std::endl;
    exec.stop_realtime_task("can_channel_0");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    status = exec.get_realtime_executor_status("can_channel_0");
    std::cout << "After stop - running: " << (status.is_running ? "yes" : "no") << std::endl;

    exec.shutdown();
    std::cout << "Executor shut down" << std::endl;

    std::cout << std::endl;
    
    // ========== 多通道演示（可选） ==========
    std::cout << "========================================" << std::endl;
    std::cout << "Multi-Channel Demo (Optional)" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    // 创建多个 CAN 通道的实时任务
    const int num_channels = 4;
    std::vector<std::atomic<int>> channel_cycles(num_channels);
    for (auto& cycles : channel_cycles) {
        cycles.store(0);
    }
    
    SimpleCycleManager multi_cycle_manager;
    
    std::cout << "Registering " << num_channels << " CAN channels..." << std::endl;
    
    for (int i = 0; i < num_channels; ++i) {
        RealtimeThreadConfig channel_config;
        channel_config.thread_name = "can_channel_" + std::to_string(i);
        channel_config.cycle_period_ns = 2000000 + i * 500000;  // 不同周期：2ms, 2.5ms, 3ms, 3.5ms
        channel_config.thread_priority = 0;
        channel_config.cycle_manager = &multi_cycle_manager;
        channel_config.cycle_callback = [i, &channel_cycles]() {
            channel_cycles[i].fetch_add(1, std::memory_order_relaxed);
        };
        
        if (exec.register_realtime_task("can_channel_" + std::to_string(i), channel_config)) {
            std::cout << "  Registered channel " << i << " (period: " 
                      << (channel_config.cycle_period_ns / 1000000.0) << " ms)" << std::endl;
        }
    }
    
    std::cout << "Starting all channels..." << std::endl;
    for (int i = 0; i < num_channels; ++i) {
        exec.start_realtime_task("can_channel_" + std::to_string(i));
    }
    
    // 运行一段时间
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // 查询所有通道的状态
    std::cout << std::endl;
    std::cout << "Channel status after 500ms:" << std::endl;
    for (int i = 0; i < num_channels; ++i) {
        auto ch_status = exec.get_realtime_executor_status("can_channel_" + std::to_string(i));
        std::cout << "  Channel " << i << ": cycles=" << channel_cycles[i].load()
                  << ", running=" << (ch_status.is_running ? "yes" : "no") << std::endl;
    }
    
    // 停止所有通道
    std::cout << std::endl;
    std::cout << "Stopping all channels..." << std::endl;
    for (int i = 0; i < num_channels; ++i) {
        exec.stop_realtime_task("can_channel_" + std::to_string(i));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Realtime CAN example completed." << std::endl;
    std::cout << "========================================" << std::endl;

    exec.shutdown();
    return 0;
}
