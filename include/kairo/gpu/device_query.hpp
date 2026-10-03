#pragma once

#include "kairo/types.hpp"
#include <vector>
#include <string>

namespace kairo::gpu {

/**
 * @brief 枚举所有可用的 CUDA 设备
 *
 * @return CUDA 设备列表，如果 CUDA 不可用则返回空列表
 */
std::vector<GpuDeviceInfo> enumerate_cuda_devices();

/**
 * @brief 枚举所有可用的 OpenCL 设备
 *
 * @return OpenCL 设备列表，如果 OpenCL 不可用则返回空列表
 */
std::vector<GpuDeviceInfo> enumerate_opencl_devices();

/**
 * @brief 枚举所有可用的 GPU 设备（所有后端）
 *
 * @return 所有 GPU 设备列表
 */
std::vector<GpuDeviceInfo> enumerate_all_devices();

/**
 * @brief 获取推荐的后端类型
 *
 * 根据系统环境自动选择最佳后端：
 * - NVIDIA GPU 优先 CUDA
 * - AMD/Intel GPU 使用 OpenCL
 *
 * @param device_id 设备ID（默认0）
 * @return 推荐的后端类型，如果无可用设备则返回 CUDA
 */
GpuBackend get_recommended_backend(int device_id = 0);

} // namespace kairo::gpu
