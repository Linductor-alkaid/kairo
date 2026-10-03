#include <kairo/executor.hpp>
#include <iostream>
#include <vector>

int main() {
    std::cout << "OpenCL Executor Example\n";
    std::cout << "======================\n\n";

    auto& exec = kairo::Executor::instance();

    // 注册 OpenCL 执行器
    kairo::gpu::GpuExecutorConfig config;
    config.name = "opencl0";
    config.backend = kairo::gpu::GpuBackend::OPENCL;
    config.device_id = 0;
    config.default_stream_count = 2;

    if (!exec.register_gpu_executor("opencl0", config)) {
        std::cerr << "Failed to register OpenCL executor. OpenCL may not be available.\n";
        return 1;
    }

    std::cout << "OpenCL executor registered successfully.\n\n";

    // 获取设备信息
    auto device_info = exec.get_gpu_executor("opencl0")->get_device_info();
    std::cout << "Device Info:\n";
    std::cout << "  Name: " << device_info.name << "\n";
    std::cout << "  Backend: OpenCL\n";
    std::cout << "  Device ID: " << device_info.device_id << "\n";
    std::cout << "  Total Memory: " << (device_info.total_memory_bytes / 1024 / 1024) << " MB\n\n";

    // 分配设备内存
    const size_t data_size = 1024 * sizeof(float);
    auto* gpu_exec = exec.get_gpu_executor("opencl0");

    void* device_buffer = gpu_exec->allocate_device_memory(data_size);
    if (!device_buffer) {
        std::cerr << "Failed to allocate device memory.\n";
        return 1;
    }
    std::cout << "Allocated " << data_size << " bytes on device.\n";

    // 准备主机数据
    std::vector<float> host_data(1024);
    for (size_t i = 0; i < host_data.size(); ++i) {
        host_data[i] = static_cast<float>(i);
    }

    // 复制到设备
    if (!gpu_exec->copy_to_device(device_buffer, host_data.data(), data_size)) {
        std::cerr << "Failed to copy data to device.\n";
        gpu_exec->free_device_memory(device_buffer);
        return 1;
    }
    std::cout << "Copied data to device.\n";

    // 提交 OpenCL kernel 任务
    kairo::gpu::GpuTaskConfig task_config;
    task_config.stream_id = 0;

    auto future = exec.submit_gpu("opencl0",
        [](void* queue) {
            // 这里应该是 OpenCL kernel 调用
            // 示例中仅做占位
            std::cout << "Kernel executed on OpenCL device.\n";
        },
        task_config
    );

    future.wait();
    std::cout << "Kernel execution completed.\n";

    // 复制回主机
    std::vector<float> result_data(1024);
    if (!gpu_exec->copy_to_host(result_data.data(), device_buffer, data_size)) {
        std::cerr << "Failed to copy data from device.\n";
        gpu_exec->free_device_memory(device_buffer);
        return 1;
    }
    std::cout << "Copied data from device.\n";

    // 释放设备内存
    gpu_exec->free_device_memory(device_buffer);
    std::cout << "Device memory freed.\n\n";

    // 获取执行器状态
    auto status = exec.get_gpu_executor_status("opencl0");
    std::cout << "Executor Status:\n";
    std::cout << "  Active kernels: " << status.active_kernels << "\n";
    std::cout << "  Completed kernels: " << status.completed_kernels << "\n";
    std::cout << "  Failed kernels: " << status.failed_kernels << "\n";

    std::cout << "\nOpenCL example completed successfully.\n";
    return 0;
}
