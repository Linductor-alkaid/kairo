#include <kairo/executor.hpp>
#include <iostream>
#include <vector>

#ifdef KAIRO_ENABLE_CUDA
#include <cuda_runtime.h>

__global__ void vector_scale_kernel(float* data, float scale, int n) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < n) {
        data[idx] *= scale;
    }
}

int main() {
    std::cout << "Unified Memory Example\n";
    std::cout << "======================\n\n";

    auto& executor = kairo::Executor::instance();

    // 配置启用统一内存
    kairo::gpu::GpuExecutorConfig config;
    config.name = "cuda0";
    config.backend = kairo::gpu::GpuBackend::CUDA;
    config.device_id = 0;
    config.enable_unified_memory = true;

    if (!executor.register_gpu_executor("cuda0", config)) {
        std::cerr << "Failed to register GPU executor\n";
        return 1;
    }

    auto* gpu_executor = executor.get_gpu_executor("cuda0");
    if (!gpu_executor) {
        std::cerr << "Failed to get GPU executor\n";
        return 1;
    }

    const int n = 1024;
    size_t size = n * sizeof(float);

    // 分配统一内存
    float* data = static_cast<float*>(gpu_executor->allocate_unified_memory(size));
    if (!data) {
        std::cerr << "Unified memory not supported or allocation failed\n";
        return 1;
    }

    std::cout << "Allocated " << size << " bytes of unified memory\n";

    // CPU 初始化数据
    std::cout << "Initializing data on CPU...\n";
    for (int i = 0; i < n; ++i) {
        data[i] = static_cast<float>(i);
    }

    // 预取到 GPU
    std::cout << "Prefetching to GPU...\n";
    if (!gpu_executor->prefetch_memory(data, size, 0, 0)) {
        std::cout << "Prefetch not supported, continuing anyway\n";
    }

    // GPU 处理
    std::cout << "Processing on GPU...\n";
    kairo::gpu::GpuTaskConfig task_config;
    task_config.grid_size[0] = (n + 255) / 256;
    task_config.block_size[0] = 256;

    auto future = executor.submit_gpu("cuda0",
        [data, n](void* stream) {
            cudaStream_t s = static_cast<cudaStream_t>(stream);
            vector_scale_kernel<<<(n + 255) / 256, 256, 0, s>>>(data, 2.0f, n);
        },
        task_config
    );

    future.wait();

    // 预取回 CPU
    std::cout << "Prefetching back to CPU...\n";
    gpu_executor->prefetch_memory(data, size, cudaCpuDeviceId, 0);
    gpu_executor->synchronize();

    // CPU 验证
    std::cout << "Verifying results on CPU...\n";
    bool success = true;
    for (int i = 0; i < n; ++i) {
        float expected = static_cast<float>(i) * 2.0f;
        if (data[i] != expected) {
            std::cerr << "Mismatch at " << i << ": " << data[i] << " != " << expected << "\n";
            success = false;
            break;
        }
    }

    if (success) {
        std::cout << "✓ All results correct!\n";
    }

    // 释放统一内存
    gpu_executor->free_unified_memory(data);

    std::cout << "\nUnified memory example completed successfully\n";
    return 0;
}

#else

int main() {
    std::cerr << "CUDA not enabled\n";
    return 1;
}

#endif
