/**
 * @file gpu_basic.cpp
 * @brief GPU 执行器基础示例：演示执行器注册、任务提交、内存管理与多 GPU 使用
 *
 * 需使用 KAIRO_ENABLE_GPU=ON 且 KAIRO_ENABLE_CUDA=ON 构建。
 * 实际 kernel 启动由用户在 submit_gpu 的 lambda 内编写。
 * 多 GPU：自动注册所有可用设备（cuda0, cuda1, ...），示例 4 演示向多块 GPU 并行提交任务。
 */

#include <iostream>
#include <string>
#include <vector>
#include <kairo/executor.hpp>

using namespace kairo;

#ifdef KAIRO_ENABLE_GPU

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "GPU Executor Basic Example" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    // ---------- 1. 初始化 Executor ----------
    ExecutorConfig config;
    config.min_threads = 2;
    config.max_threads = 4;
    config.queue_capacity = 1000;

    auto& exec = Executor::instance();
    if (!exec.initialize(config)) {
        std::cerr << "Failed to initialize executor" << std::endl;
        return 1;
    }
    std::cout << "Executor initialized successfully" << std::endl;
    std::cout << std::endl;

    // ---------- 2. GPU 执行器注册（支持多 GPU）----------
    // 配置 CUDA 执行器：名称、后端、设备 ID、队列与流数量等
    // 循环注册多块 GPU（cuda0, cuda1, ...），直到无可用设备或注册失败
    int gpu_count = 0;
    for (int device_id = 0; ; ++device_id) {
        gpu::GpuExecutorConfig gpu_config;
        gpu_config.name = "cuda" + std::to_string(device_id);
        gpu_config.backend = gpu::GpuBackend::CUDA;
        gpu_config.device_id = device_id;
        gpu_config.max_queue_size = 1000;
        gpu_config.default_stream_count = 1;

        if (!exec.register_gpu_executor(gpu_config.name, gpu_config)) {
            break;
        }
        ++gpu_count;
    }

    if (gpu_count == 0) {
        std::cout << "GPU not available or registration failed. Skipping GPU examples." << std::endl;
        exec.shutdown();
        return 0;
    }
    std::cout << "Registered " << gpu_count << " GPU executor(s): ";
    auto names = exec.get_gpu_executor_names();
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << "\"" << names[i] << "\"";
    }
    std::cout << std::endl << std::endl;

    // ---------- 示例一：基本 GPU 任务提交 ----------
    std::cout << "Example 1: Basic GPU task submission" << std::endl;
    {
        gpu::GpuTaskConfig task_config;
        task_config.grid_size[0] = 1;
        task_config.block_size[0] = 1;
        task_config.async = false;

        // 提交无参 callable；在实际应用中，此处应填入在 GPU 上执行的 kernel 调用逻辑
        auto future = exec.submit_gpu("cuda0",
            []() {
                // 占位：实际使用时在此调用 CUDA kernel，如
                //   my_kernel<<<grid, block, 0, stream>>>(...);
            },
            task_config);

        future.wait();
        try {
            future.get();
        } catch (const std::exception& e) {
            std::cerr << "  GPU task exception: " << e.what() << std::endl;
        }
        std::cout << "  GPU task completed" << std::endl;
    }
    std::cout << std::endl;

    // ---------- 示例二：GPU 内存管理 ----------
    std::cout << "Example 2: GPU memory management" << std::endl;
    {
        auto* gpu_exec = exec.get_gpu_executor("cuda0");
        if (!gpu_exec) {
            std::cerr << "  GPU executor not found" << std::endl;
            exec.shutdown();
            return 1;
        }

        const size_t n = 1024;
        const size_t size_bytes = n * sizeof(float);
        std::vector<float> host_src(n, 1.0f);
        std::vector<float> host_dst(n, 0.0f);

        // 分配设备内存
        void* d_ptr = gpu_exec->allocate_device_memory(size_bytes);
        if (!d_ptr) {
            std::cerr << "  Failed to allocate device memory" << std::endl;
            exec.shutdown();
            return 1;
        }

        // 主机 -> 设备（同步复制）
        if (!gpu_exec->copy_to_device(d_ptr, host_src.data(), size_bytes, false)) {
            std::cerr << "  copy_to_device failed" << std::endl;
            gpu_exec->free_device_memory(d_ptr);
            exec.shutdown();
            return 1;
        }

        // 提交“内核”（此处为占位，实际应在此执行 GPU kernel）
        gpu::GpuTaskConfig task_config;
        task_config.grid_size[0] = 1;
        task_config.block_size[0] = 1;
        task_config.async = false;

        auto future = exec.submit_gpu("cuda0",
            [d_ptr, size_bytes, gpu_exec]() {
                (void)d_ptr;
                (void)size_bytes;
                (void)gpu_exec;
                // 占位：在实际应用中在此启动 kernel 处理 d_ptr 指向的设备数据
            },
            task_config);

        future.wait();
        try {
            future.get();
        } catch (const std::exception& e) {
            std::cerr << "  GPU task exception: " << e.what() << std::endl;
            gpu_exec->free_device_memory(d_ptr);
            exec.shutdown();
            return 1;
        }

        // 设备 -> 主机
        if (!gpu_exec->copy_to_host(host_dst.data(), d_ptr, size_bytes, false)) {
            std::cerr << "  copy_to_host failed" << std::endl;
            gpu_exec->free_device_memory(d_ptr);
            exec.shutdown();
            return 1;
        }

        gpu_exec->free_device_memory(d_ptr);

        std::cout << "  Allocate -> copy_to_device -> submit_gpu -> copy_to_host -> free done"
                  << std::endl;
        std::cout << "  host_dst[0] = " << host_dst[0] << " (expected 1.0)" << std::endl;
    }
    std::cout << std::endl;

    // ---------- 2b. 异步内存传输与指定流（重叠 copy 与 kernel） ----------
    std::cout << "Example 2b: Async memory transfer with stream" << std::endl;
    {
        auto* gpu_exec = exec.get_gpu_executor("cuda0");
        if (!gpu_exec) {
            std::cerr << "  GPU executor not found" << std::endl;
        } else {
            int stream_id = gpu_exec->create_stream();
            if (stream_id < 0) {
                std::cout << "  create_stream failed, skip async demo." << std::endl;
            } else {
                const size_t n = 256;
                const size_t size_bytes = n * sizeof(float);
                std::vector<float> host_src(n, 2.0f);
                std::vector<float> host_dst(n, 0.0f);
                void* d_ptr = gpu_exec->allocate_device_memory(size_bytes);
                if (d_ptr) {
                    // 异步 copy 到设备（指定流）
                    if (gpu_exec->copy_to_device(d_ptr, host_src.data(), size_bytes, true, stream_id)) {
                        gpu::GpuTaskConfig task_config;
                        task_config.grid_size[0] = 1;
                        task_config.block_size[0] = 1;
                        task_config.stream_id = stream_id;
                        auto future = exec.submit_gpu("cuda0", [d_ptr]() { (void)d_ptr; }, task_config);
                        future.wait();
                        gpu_exec->synchronize_stream(stream_id);
                        // 再复制回主机（可同步或异步）
                        if (gpu_exec->copy_to_host(host_dst.data(), d_ptr, size_bytes, false, stream_id)) {
                            std::cout << "  Async copy (stream " << stream_id
                                      << ") -> kernel -> sync -> copy_to_host done." << std::endl;
                            std::cout << "  host_dst[0] = " << host_dst[0] << " (expected 2.0)" << std::endl;
                        }
                    }
                    gpu_exec->free_device_memory(d_ptr);
                }
                gpu_exec->destroy_stream(stream_id);
            }
        }
    }
    std::cout << std::endl;

    // ---------- 3. 状态查询 ----------
    std::cout << "Example 3: GPU executor status" << std::endl;
    {
        auto status = exec.get_gpu_executor_status("cuda0");
        std::cout << "  Name: " << status.name << std::endl;
        std::cout << "  Is running: " << (status.is_running ? "true" : "false") << std::endl;
        std::cout << "  Device ID: " << status.device_id << std::endl;
        std::cout << "  Queue size: " << status.queue_size << std::endl;
        std::cout << "  Memory used: " << status.memory_used_bytes << " B" << std::endl;
        std::cout << "  Memory total: " << status.memory_total_bytes << " B" << std::endl;
    }
    std::cout << std::endl;

    // ---------- 4. 多 GPU 示例 ----------
    std::cout << "Example 4: Multi-GPU task submission" << std::endl;
    {
        auto gpu_names = exec.get_gpu_executor_names();
        if (gpu_names.size() < 2) {
            std::cout << "  Only one GPU registered, skip multi-GPU demo." << std::endl;
        } else {
            gpu::GpuTaskConfig task_config;
            task_config.grid_size[0] = 1;
            task_config.block_size[0] = 1;
            task_config.async = false;

            std::vector<std::future<void>> futures;
            for (const auto& name : gpu_names) {
                std::string executor_name = name;  // 按值捕获，避免异步执行时悬垂引用
                auto future = exec.submit_gpu(name,
                    [executor_name]() {
                        // 占位：在实际应用中，此处可针对不同 device 执行不同 kernel
                        (void)executor_name;
                    },
                    task_config);
                futures.push_back(std::move(future));
            }

            for (size_t i = 0; i < futures.size(); ++i) {
                try {
                    futures[i].get();
                } catch (const std::exception& e) {
                    std::cerr << "  GPU task on \"" << gpu_names[i] << "\" exception: " << e.what() << std::endl;
                }
            }
            std::cout << "  Submitted and completed " << futures.size() << " tasks (one per GPU)." << std::endl;

            std::cout << "  Status of all GPUs:" << std::endl;
            for (const auto& name : gpu_names) {
                auto status = exec.get_gpu_executor_status(name);
                std::cout << "    " << status.name << " (device_id=" << status.device_id
                          << ", memory_used=" << status.memory_used_bytes << " B)" << std::endl;
            }
        }
    }
    std::cout << std::endl;

    exec.shutdown();
    std::cout << "Example completed successfully." << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}

#else

int main() {
    std::cout << "GPU support not enabled. Build with KAIRO_ENABLE_GPU=ON and "
                 "KAIRO_ENABLE_CUDA=ON." << std::endl;
    return 0;
}

#endif
