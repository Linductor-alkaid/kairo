#include <kairo/gpu/device_query.hpp>
#include <iostream>

int main() {
    std::cout << "GPU Device Query Example\n";
    std::cout << "========================\n\n";

    // 查询所有可用设备
    auto devices = kairo::gpu::enumerate_all_devices();

    if (devices.empty()) {
        std::cout << "No GPU devices found.\n";
        return 1;
    }

    std::cout << "Found " << devices.size() << " GPU device(s):\n\n";

    for (const auto& dev : devices) {
        std::cout << "Device " << dev.device_id << ":\n";
        std::cout << "  Name: " << dev.name << "\n";
        std::cout << "  Backend: " << (dev.backend == kairo::gpu::GpuBackend::CUDA ? "CUDA" : "OpenCL") << "\n";
        std::cout << "  Vendor: " << dev.vendor << "\n";
        std::cout << "  Memory: " << (dev.total_memory_bytes / 1024 / 1024) << " MB\n";
        if (dev.backend == kairo::gpu::GpuBackend::CUDA) {
            std::cout << "  Compute Capability: " << dev.compute_capability_major
                      << "." << dev.compute_capability_minor << "\n";
        }
        std::cout << "\n";
    }

    // 获取推荐后端
    auto recommended = kairo::gpu::get_recommended_backend(0);
    std::cout << "Recommended backend for device 0: "
              << (recommended == kairo::gpu::GpuBackend::CUDA ? "CUDA" : "OpenCL") << "\n";

    return 0;
}
