#include "kairo/gpu/device_query.hpp"
#include "kairo/gpu/cuda_loader.hpp"
#include "kairo/gpu/opencl_loader.hpp"
#include <algorithm>

namespace kairo::gpu {

std::vector<GpuDeviceInfo> enumerate_cuda_devices() {
    std::vector<GpuDeviceInfo> devices;

#ifdef KAIRO_ENABLE_CUDA
    auto& loader = CudaLoader::instance();
    if (!loader.load()) {
        return devices;
    }

    auto funcs = loader.get_functions();
    int device_count = 0;
    if (funcs.cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
        return devices;
    }

    for (int i = 0; i < device_count; ++i) {
        cudaDeviceProp prop;
        if (funcs.cudaGetDeviceProperties(&prop, i) != cudaSuccess) {
            continue;
        }

        GpuDeviceInfo info;
        info.device_id = i;
        info.backend = GpuBackend::CUDA;
        info.name = prop.name;
        info.vendor = "NVIDIA";
        info.compute_capability_major = prop.major;
        info.compute_capability_minor = prop.minor;
        info.max_threads_per_block = static_cast<size_t>(prop.maxThreadsPerBlock);
        info.max_blocks_per_grid[0] = static_cast<size_t>(prop.maxGridSize[0]);
        info.max_blocks_per_grid[1] = static_cast<size_t>(prop.maxGridSize[1]);
        info.max_blocks_per_grid[2] = static_cast<size_t>(prop.maxGridSize[2]);

        // 获取内存信息
        size_t free_mem = 0, total_mem = 0;
        if (funcs.cudaMemGetInfo && funcs.cudaSetDevice) {
            if (funcs.cudaSetDevice(i) == cudaSuccess) {
                if (funcs.cudaMemGetInfo(&free_mem, &total_mem) == cudaSuccess) {
                    info.total_memory_bytes = total_mem;
                    info.free_memory_bytes = free_mem;
                }
            }
        }

        devices.push_back(info);
    }
#endif

    return devices;
}

std::vector<GpuDeviceInfo> enumerate_opencl_devices() {
    std::vector<GpuDeviceInfo> devices;

#ifdef KAIRO_ENABLE_OPENCL
    auto& loader = OpenCLLoader::instance();
    if (!loader.load()) {
        return devices;
    }

    auto funcs = loader.get_functions();

    // 获取平台数量
    cl_uint num_platforms = 0;
    if (funcs.clGetPlatformIDs(0, nullptr, &num_platforms) != 0 || num_platforms == 0) {
        return devices;
    }

    std::vector<cl_platform_id> platforms(num_platforms);
    if (funcs.clGetPlatformIDs(num_platforms, platforms.data(), nullptr) != 0) {
        return devices;
    }

    int device_index = 0;
    for (cl_platform_id platform : platforms) {
        // 获取GPU设备数量
        cl_uint num_devices = 0;
        if (funcs.clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 0, nullptr, &num_devices) != 0 || num_devices == 0) {
            continue;
        }

        std::vector<cl_device_id> cl_devices(num_devices);
        if (funcs.clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, num_devices, cl_devices.data(), nullptr) != 0) {
            continue;
        }

        for (cl_device_id device : cl_devices) {
            GpuDeviceInfo info;
            info.device_id = device_index++;
            info.backend = GpuBackend::OPENCL;

            // 获取设备名称
            char name[256] = {0};
            if (funcs.clGetDeviceInfo(device, CL_DEVICE_NAME, sizeof(name), name, nullptr) == 0) {
                info.name = name;
            }

            // 获取总内存
            cl_ulong mem_size = 0;
            if (funcs.clGetDeviceInfo(device, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(mem_size), &mem_size, nullptr) == 0) {
                info.total_memory_bytes = static_cast<size_t>(mem_size);
            }

            // 获取厂商
            char vendor[256] = {0};
            if (funcs.clGetDeviceInfo(device, CL_DEVICE_VENDOR, sizeof(vendor), vendor, nullptr) == 0) {
                info.vendor = vendor;
            }

            info.compute_capability_major = 0;
            info.compute_capability_minor = 0;

            devices.push_back(info);
        }
    }
#endif

    return devices;
}

std::vector<GpuDeviceInfo> enumerate_all_devices() {
    std::vector<GpuDeviceInfo> all_devices;

    auto cuda_devices = enumerate_cuda_devices();
    auto opencl_devices = enumerate_opencl_devices();

    all_devices.insert(all_devices.end(), cuda_devices.begin(), cuda_devices.end());
    all_devices.insert(all_devices.end(), opencl_devices.begin(), opencl_devices.end());

    return all_devices;
}

GpuBackend get_recommended_backend(int device_id) {
    // 优先尝试CUDA（NVIDIA GPU性能更好）
    auto cuda_devices = enumerate_cuda_devices();
    if (!cuda_devices.empty() && device_id < static_cast<int>(cuda_devices.size())) {
        return GpuBackend::CUDA;
    }

    // 其次尝试OpenCL
    auto opencl_devices = enumerate_opencl_devices();
    if (!opencl_devices.empty()) {
        return GpuBackend::OPENCL;
    }

    // 默认返回CUDA
    return GpuBackend::CUDA;
}

} // namespace kairo::gpu
