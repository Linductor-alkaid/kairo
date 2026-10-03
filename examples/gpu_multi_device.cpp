/**
 * @file gpu_multi_device.cpp
 * @brief 多 GPU 设备示例：多设备注册、并行提交、P2P 设备间数据同步
 *
 * 需使用 KAIRO_ENABLE_GPU=ON 且 KAIRO_ENABLE_CUDA=ON 构建。
 * 演示：多 GPU 注册、多 GPU 并行任务提交、设备间 P2P 拷贝与校验。
 */

#include <iostream>
#include <string>
#include <vector>
#include <kairo/executor.hpp>

using namespace kairo;

#ifdef KAIRO_ENABLE_GPU

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "GPU Multi-Device Example" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

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

    // ---------- 1. 多 GPU 设备注册 ----------
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
        std::cout << "GPU not available or registration failed. Exiting." << std::endl;
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

    // ---------- 2. 多 GPU 并行任务提交 ----------
    std::cout << "Example: Multi-GPU parallel task submission" << std::endl;
    {
        gpu::GpuTaskConfig task_config;
        task_config.grid_size[0] = 1;
        task_config.block_size[0] = 1;
        task_config.async = false;

        std::vector<std::future<void>> futures;
        for (const auto& name : names) {
            std::string executor_name = name;
            auto future = exec.submit_gpu(name,
                [executor_name]() {
                    (void)executor_name;
                    // 占位：实际应用中可针对不同 device 执行不同 kernel
                },
                task_config);
            futures.push_back(std::move(future));
        }

        for (size_t i = 0; i < futures.size(); ++i) {
            try {
                futures[i].get();
            } catch (const std::exception& e) {
                std::cerr << "  GPU task on \"" << names[i] << "\" exception: " << e.what() << std::endl;
            }
        }
        std::cout << "  Submitted and completed " << futures.size() << " tasks (one per GPU)." << std::endl;
    }
    std::cout << std::endl;

    // ---------- 3. 设备间数据同步（P2P） ----------
    if (gpu_count < 2) {
        std::cout << "Single GPU, skipping P2P demo." << std::endl;
    } else {
        std::cout << "Example: P2P device-to-device copy (cuda0 -> cuda1)" << std::endl;
        auto* cuda0 = exec.get_gpu_executor("cuda0");
        auto* cuda1 = exec.get_gpu_executor("cuda1");
        if (!cuda0 || !cuda1) {
            std::cerr << "  Missing cuda0 or cuda1 executor." << std::endl;
        } else {
            const size_t n = 256;
            const size_t size_bytes = n * sizeof(float);
            std::vector<float> host_src(n);
            for (size_t i = 0; i < n; ++i) {
                host_src[i] = static_cast<float>(i);
            }
            std::vector<float> host_dst(n, 0.0f);

            void* d_src = cuda0->allocate_device_memory(size_bytes);
            void* d_dst = cuda1->allocate_device_memory(size_bytes);
            if (!d_src || !d_dst) {
                std::cerr << "  P2P: allocation failed." << std::endl;
                if (d_src) cuda0->free_device_memory(d_src);
                if (d_dst) cuda1->free_device_memory(d_dst);
            } else {
                if (!cuda0->copy_to_device(d_src, host_src.data(), size_bytes, false)) {
                    std::cerr << "  P2P: copy_to_device (cuda0) failed." << std::endl;
                } else {
                    bool used_p2p = cuda1->copy_from_peer(cuda0, d_src, d_dst, size_bytes, false);
                    bool transfer_ok = used_p2p;
                    if (!used_p2p) {
                        std::vector<float> host_tmp(n);
                        if (cuda0->copy_to_host(host_tmp.data(), d_src, size_bytes, false) &&
                            cuda1->copy_to_device(d_dst, host_tmp.data(), size_bytes, false)) {
                            transfer_ok = true;
                        } else {
                            std::cerr << "  Fallback: host-mediated copy (cuda0 -> host -> cuda1) failed." << std::endl;
                        }
                    }
                    if (transfer_ok) {
                        gpu::GpuTaskConfig task_config;
                        task_config.grid_size[0] = 1;
                        task_config.block_size[0] = 1;
                        task_config.async = false;
                        auto future = exec.submit_gpu("cuda1", [d_dst]() { (void)d_dst; }, task_config);
                        future.wait();
                        try {
                            future.get();
                        } catch (const std::exception& e) {
                            std::cerr << "  Kernel exception: " << e.what() << std::endl;
                        }
                        if (!cuda1->copy_to_host(host_dst.data(), d_dst, size_bytes, false)) {
                            std::cerr << "  copy_to_host failed." << std::endl;
                        } else {
                            bool ok = true;
                            for (size_t i = 0; i < n && ok; ++i) {
                                if (host_dst[i] != host_src[i]) ok = false;
                            }
                            if (ok) {
                                if (used_p2p) {
                                    std::cout << "  P2P copy verified (cuda0 -> cuda1, " << n << " floats)." << std::endl;
                                } else {
                                    std::cout << "  P2P unavailable; host-mediated copy (cuda0 -> host -> cuda1) verified (" << n << " floats)." << std::endl;
                                }
                            } else {
                                std::cerr << "  Verification failed." << std::endl;
                            }
                        }
                    }
                }
                cuda0->free_device_memory(d_src);
                cuda1->free_device_memory(d_dst);
            }
        }
    }
    std::cout << std::endl;

    // ---------- 4. 状态与设备信息 ----------
    std::cout << "Status of all GPUs:" << std::endl;
    for (const auto& name : names) {
        auto status = exec.get_gpu_executor_status(name);
        std::cout << "  " << status.name << " (device_id=" << status.device_id
                  << ", memory_used=" << status.memory_used_bytes << " B)" << std::endl;
    }
    std::cout << std::endl;

    exec.shutdown();
    std::cout << "Multi-device example completed." << std::endl;
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
