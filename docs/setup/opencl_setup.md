# OpenCL 测试环境搭建指南

本文档说明如何搭建 OpenCL 测试环境，以便测试 executor 项目的 OpenCL 执行器功能。

---

## 1. OpenCL 运行时安装

OpenCL 是跨平台的异构计算标准，支持 Intel、AMD、NVIDIA 等多家厂商的 GPU。

### 1.1 Linux 环境

#### Intel GPU (集成显卡)

```bash
# Ubuntu/Debian
sudo apt-get update
sudo apt-get install intel-opencl-icd

# 或者安装完整的 Intel Compute Runtime
wget https://github.com/intel/compute-runtime/releases/download/23.30.26918.9/intel-level-zero-gpu_1.3.26918.9_amd64.deb
wget https://github.com/intel/compute-runtime/releases/download/23.30.26918.9/intel-opencl-icd_23.30.26918.9_amd64.deb
sudo dpkg -i *.deb
```

#### NVIDIA GPU

```bash
# 安装 NVIDIA 驱动（如果未安装）
sudo apt-get install nvidia-driver-535

# NVIDIA 驱动自带 OpenCL 支持，无需额外安装
```

#### AMD GPU

```bash
# 安装 ROCm (AMD GPU 计算平台)
wget https://repo.radeon.com/amdgpu-install/latest/ubuntu/focal/amdgpu-install_5.4.50403-1_all.deb
sudo dpkg -i amdgpu-install_5.4.50403-1_all.deb
sudo amdgpu-install --usecase=opencl
```

#### 通用 OpenCL ICD Loader

```bash
# 安装 OpenCL ICD Loader (所有厂商通用)
sudo apt-get install ocl-icd-libopencl1
sudo apt-get install opencl-headers
sudo apt-get install clinfo

# 验证安装
clinfo
```

### 1.2 Windows 环境

#### Intel GPU

1. 下载 Intel Graphics Driver: https://www.intel.com/content/www/us/en/download-center/home.html
2. 安装驱动，OpenCL 运行时会自动安装

#### NVIDIA GPU

1. 下载 NVIDIA 驱动: https://www.nvidia.com/Download/index.aspx
2. 安装驱动，OpenCL 运行时会自动安装

#### AMD GPU

1. 下载 AMD Adrenalin 驱动: https://www.amd.com/en/support
2. 安装驱动，OpenCL 运行时会自动安装

#### OpenCL SDK (开发用)

```powershell
# 下载 OpenCL SDK
# https://github.com/KhronosGroup/OpenCL-SDK/releases

# 或使用 vcpkg 安装
vcpkg install opencl
```

---

## 2. 验证 OpenCL 安装

### 2.1 使用 clinfo 工具

```bash
# Linux
clinfo

# Windows (需要下载 clinfo.exe)
clinfo.exe
```

预期输出应包含：
- 平台信息 (Platform)
- 设备信息 (Device)
- 设备类型 (GPU/CPU)
- OpenCL 版本

### 2.2 检查 OpenCL 库文件

#### Linux

```bash
# 检查 libOpenCL.so
ls -l /usr/lib/x86_64-linux-gnu/libOpenCL.so*
ls -l /usr/lib64/libOpenCL.so*

# 检查 ICD 配置
ls -l /etc/OpenCL/vendors/
```

#### Windows

```powershell
# 检查 OpenCL.dll
dir C:\Windows\System32\OpenCL.dll
```

---

## 3. 构建 executor 项目（启用 OpenCL）

### 3.1 配置构建

```bash
cd executor
mkdir build && cd build

# 启用 OpenCL 支持
cmake .. -DKAIRO_ENABLE_GPU=ON \
         -DKAIRO_ENABLE_OPENCL=ON \
         -DKAIRO_BUILD_TESTS=ON \
         -DKAIRO_BUILD_EXAMPLES=ON

# 构建
cmake --build . -j$(nproc)
```

### 3.2 检查构建输出

构建成功后，应该看到：

```
-- OpenCL found: Version X.X
-- Configuring done
-- Generating done
```

如果 OpenCL 未找到：

```
-- OpenCL support requested but OpenCL not found. Disabling OpenCL support.
```

---

## 4. 运行测试

### 4.1 运行 OpenCL 单元测试

```bash
cd build
./tests/test_opencl_executor

# 或使用 ctest
ctest -R opencl -V
```

### 4.2 运行 OpenCL 示例

```bash
./examples/gpu_opencl
```

预期输出：

```
OpenCL Executor Example
======================

OpenCL executor registered successfully.

Device Info:
  Name: Intel(R) UHD Graphics 620
  Backend: OpenCL
  Device ID: 0
  Total Memory: 6656 MB

Allocated 4096 bytes on device.
Copied data to device.
Kernel executed on OpenCL device.
Kernel execution completed.
Copied data from device.
Device memory freed.

Executor Status:
  Active kernels: 0
  Completed kernels: 1
  Failed kernels: 0

OpenCL example completed successfully.
```

---

## 5. 常见问题排查

### 5.1 找不到 OpenCL 库

**问题**: CMake 报告 "OpenCL not found"

**解决方案**:

```bash
# Linux: 设置环境变量
export OPENCL_PATH=/usr/local/opencl

# 或在 CMake 中指定路径
cmake .. -DOpenCL_INCLUDE_DIR=/usr/include \
         -DOpenCL_LIBRARY=/usr/lib/x86_64-linux-gnu/libOpenCL.so
```

### 5.2 运行时找不到设备

**问题**: `clinfo` 显示 "Number of platforms: 0"

**解决方案**:

```bash
# 检查 ICD Loader 配置
ls /etc/OpenCL/vendors/

# 如果为空，手动创建 ICD 文件
# Intel 示例:
echo "/usr/lib/x86_64-linux-gnu/intel-opencl/libigdrcl.so" | \
    sudo tee /etc/OpenCL/vendors/intel.icd
```

### 5.3 权限问题

**问题**: "Permission denied" 访问 GPU 设备

**解决方案**:

```bash
# 将用户添加到 video 和 render 组
sudo usermod -a -G video $USER
sudo usermod -a -G render $USER

# 重新登录生效
```

### 5.4 动态加载失败

**问题**: executor 运行时报告 "OpenCL not available"

**解决方案**:

executor 使用动态加载，会自动搜索以下路径：

**Linux**:
- `/usr/lib/x86_64-linux-gnu/libOpenCL.so.1`
- `/usr/lib64/libOpenCL.so.1`
- `/opt/intel/opencl/lib64/libOpenCL.so`

**Windows**:
- `C:\Windows\System32\OpenCL.dll`

如果库不在这些路径，设置环境变量：

```bash
export OPENCL_PATH=/path/to/opencl
```

---

## 6. 性能测试

### 6.1 基准测试

```bash
# 运行性能测试
./tests/test_gpu_performance --gtest_filter=*OpenCL*
```

### 6.2 对比 CPU vs OpenCL

```bash
# 运行混合计算示例
./examples/gpu_hybrid_compute
```

---

## 7. 多设备配置

如果系统有多个 OpenCL 设备（如集成显卡 + 独立显卡）：

```cpp
// 注册多个 OpenCL 执行器
kairo::gpu::GpuExecutorConfig config0;
config0.device_id = 0;  // Intel 集成显卡
executor.register_gpu_executor("opencl0", config0);

kairo::gpu::GpuExecutorConfig config1;
config1.device_id = 1;  // NVIDIA 独立显卡
executor.register_gpu_executor("opencl1", config1);
```

---

## 8. 参考资源

- **OpenCL 官方文档**: https://www.khronos.org/opencl/
- **Intel OpenCL**: https://www.intel.com/content/www/us/en/developer/tools/opencl-sdk/overview.html
- **AMD ROCm**: https://rocmdocs.amd.com/
- **NVIDIA OpenCL**: https://developer.nvidia.com/opencl
- **clinfo 工具**: https://github.com/Oblomov/clinfo

---

## 9. 最小测试环境

如果只是想快速测试，最简单的方式：

### Linux (Intel CPU 集成显卡)

```bash
sudo apt-get install intel-opencl-icd ocl-icd-libopencl1 clinfo
clinfo
```

### Windows (任意 GPU)

1. 确保显卡驱动已安装
2. 检查 `C:\Windows\System32\OpenCL.dll` 存在
3. 直接构建和运行 executor 项目

---

## 10. Docker 测试环境 (可选)

```dockerfile
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    build-essential cmake git \
    intel-opencl-icd ocl-icd-libopencl1 \
    opencl-headers clinfo

# 构建 executor
COPY . /executor
WORKDIR /executor/build
RUN cmake .. -DKAIRO_ENABLE_OPENCL=ON && make -j

CMD ["./tests/test_opencl_executor"]
```

注意：Docker 容器需要 `--device /dev/dri` 访问 GPU。
