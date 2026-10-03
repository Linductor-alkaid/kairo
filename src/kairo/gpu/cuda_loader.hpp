#pragma once

#include <string>
#include <memory>
#include <mutex>
#include <atomic>
#include <functional>

#include "loaded_library_lease.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#ifdef KAIRO_ENABLE_CUDA
// 需要CUDA类型定义
#include <cuda_runtime.h>
#endif

namespace kairo {
namespace gpu {

// CUDA类型定义（即使没有CUDA头文件也需要）
#ifdef KAIRO_ENABLE_CUDA
// 使用CUDA头文件中的定义
#else
// 如果没有CUDA，定义基本类型
enum cudaError_t {
    cudaSuccess = 0,
    cudaErrorInvalidValue = 1,
    cudaErrorMemoryAllocation = 2,
    cudaErrorInitializationError = 3
};
typedef void* cudaStream_t;
struct cudaDeviceProp {
    char name[256];
    int major;
    int minor;
    int maxThreadsPerBlock;
    int maxGridSize[3];
};
enum cudaMemcpyKind {
    cudaMemcpyHostToDevice = 1,
    cudaMemcpyDeviceToHost = 2,
    cudaMemcpyDeviceToDevice = 3
};
#endif

/**
 * @brief CUDA函数指针类型定义
 */
using CudaFreeFunc = cudaError_t (*)(void*);
using CudaGetDeviceCountFunc = cudaError_t (*)(int*);
using CudaSetDeviceFunc = cudaError_t (*)(int);
using CudaGetDevicePropertiesFunc = cudaError_t (*)(cudaDeviceProp*, int);
using CudaMallocFunc = cudaError_t (*)(void**, size_t);
using CudaMemcpyFunc = cudaError_t (*)(void*, const void*, size_t, cudaMemcpyKind);
using CudaMemcpyAsyncFunc = cudaError_t (*)(void*, const void*, size_t, cudaMemcpyKind, cudaStream_t);
using CudaDeviceSynchronizeFunc = cudaError_t (*)();
using CudaStreamCreateFunc = cudaError_t (*)(cudaStream_t*);
using CudaStreamSynchronizeFunc = cudaError_t (*)(cudaStream_t);
using CudaStreamDestroyFunc = cudaError_t (*)(cudaStream_t);
using CudaMemGetInfoFunc = cudaError_t (*)(size_t*, size_t*);
using CudaLaunchHostFuncFunc = cudaError_t (*)(cudaStream_t, void (*)(void*), void*);
using CudaMemcpyPeerFunc = cudaError_t (*)(void*, int, const void*, int, size_t);
using CudaMemcpyPeerAsyncFunc = cudaError_t (*)(void*, int, const void*, int, size_t, cudaStream_t);
using CudaDeviceCanAccessPeerFunc = cudaError_t (*)(int*, int, int);
using CudaDeviceEnablePeerAccessFunc = cudaError_t (*)(int, unsigned int);
using CudaGetLastErrorFunc = cudaError_t (*)();
using CudaGetErrorStringFunc = const char* (*)(cudaError_t);
using CudaMallocManagedFunc = cudaError_t (*)(void**, size_t, unsigned int);
using CudaMemPrefetchAsyncFunc = cudaError_t (*)(const void*, size_t, int, cudaStream_t);

/**
 * @brief CUDA函数指针集合
 */
struct CudaFunctionPointers {
    LoadedLibraryLease library_lease;
    CudaFreeFunc cudaFree = nullptr;
    CudaGetDeviceCountFunc cudaGetDeviceCount = nullptr;
    CudaSetDeviceFunc cudaSetDevice = nullptr;
    CudaGetDevicePropertiesFunc cudaGetDeviceProperties = nullptr;
    CudaMallocFunc cudaMalloc = nullptr;
    CudaMemcpyFunc cudaMemcpy = nullptr;
    CudaMemcpyAsyncFunc cudaMemcpyAsync = nullptr;
    CudaDeviceSynchronizeFunc cudaDeviceSynchronize = nullptr;
    CudaStreamCreateFunc cudaStreamCreate = nullptr;
    CudaStreamSynchronizeFunc cudaStreamSynchronize = nullptr;
    CudaStreamDestroyFunc cudaStreamDestroy = nullptr;
    CudaMemGetInfoFunc cudaMemGetInfo = nullptr;
    CudaLaunchHostFuncFunc cudaLaunchHostFunc = nullptr;  // 可选，CUDA 10+
    CudaMemcpyPeerFunc cudaMemcpyPeer = nullptr;          // P2P 可选
    CudaMemcpyPeerAsyncFunc cudaMemcpyPeerAsync = nullptr;
    CudaDeviceCanAccessPeerFunc cudaDeviceCanAccessPeer = nullptr;
    CudaDeviceEnablePeerAccessFunc cudaDeviceEnablePeerAccess = nullptr;
    CudaGetLastErrorFunc cudaGetLastError = nullptr;
    CudaGetErrorStringFunc cudaGetErrorString = nullptr;
    CudaMallocManagedFunc cudaMallocManaged = nullptr;        // Unified Memory (可选)
    CudaMemPrefetchAsyncFunc cudaMemPrefetchAsync = nullptr;  // Unified Memory (可选)

    /**
     * @brief 检查所有函数指针是否已加载
     * @note cudaLaunchHostFunc、P2P 相关为可选，不参与 is_complete 判定
     */
    bool is_complete() const {
        return cudaFree != nullptr &&
               cudaGetDeviceCount != nullptr &&
               cudaSetDevice != nullptr &&
               cudaGetDeviceProperties != nullptr &&
               cudaMalloc != nullptr &&
               cudaMemcpy != nullptr &&
               cudaMemcpyAsync != nullptr &&
               cudaDeviceSynchronize != nullptr &&
               cudaStreamCreate != nullptr &&
               cudaStreamSynchronize != nullptr &&
               cudaStreamDestroy != nullptr &&
               cudaMemGetInfo != nullptr;
    }

    /**
     * @brief 检查 P2P 相关函数是否已加载
     */
    bool is_p2p_available() const {
        return cudaMemcpyPeer != nullptr &&
               cudaMemcpyPeerAsync != nullptr &&
               cudaDeviceCanAccessPeer != nullptr &&
               cudaDeviceEnablePeerAccess != nullptr;
    }

    /**
     * @brief 检查统一内存相关函数是否已加载
     */
    bool is_unified_memory_available() const {
        return cudaMallocManaged != nullptr &&
               cudaMemPrefetchAsync != nullptr;
    }
};

/**
 * @brief CUDA动态加载器
 * 
 * 负责动态搜索和加载CUDA DLL，获取CUDA函数指针。
 * 支持自动搜索常见安装路径，即使CUDA不在PATH中也能工作。
 */
class CudaLoader {
public:
    /**
     * @brief 获取单例实例
     */
    static CudaLoader& instance();

    /**
     * @brief 析构函数
     */
    ~CudaLoader();

    // 禁止拷贝和移动
    CudaLoader(const CudaLoader&) = delete;
    CudaLoader& operator=(const CudaLoader&) = delete;
    CudaLoader(CudaLoader&&) = delete;
    CudaLoader& operator=(CudaLoader&&) = delete;

    /**
     * @brief 加载CUDA DLL
     * 
     * 自动搜索CUDA DLL并加载，获取所有函数指针。
     * 
     * @return 是否加载成功
     */
    bool load();

    /**
     * @brief 放弃加载器持有的 CUDA DLL 引用
     *
     * 已获取的函数表仍持有库租约，因此不会因此调用悬空函数指针。
     */
    void unload();

    /**
     * @brief 放弃加载器引用，并报告 DLL 是否已实际卸载
     *
     * @return true 表示没有函数表租约保留 DLL；false 表示仍有调用方租约
     */
    bool unload_if_idle();

    /**
     * @brief 检查CUDA是否可用
     * 
     * @return 是否已成功加载
     */
    bool is_available() const;

    /**
     * @brief 获取带 DLL 租约的 CUDA 函数指针集合
     *
     * 返回值同时复制函数指针并持有 DLL 租约。即便其他线程调用 unload()
     * 或 unload_if_idle()，函数指针也会在返回值存活期间保持有效。
     *
     * 拷贝成本: CudaFunctionPointers 内全部为函数指针(几十个),
     *         sizeof 在百字节级,远小于一次 CUDA 调用。
     *
     * @return 带 DLL 租约的函数指针集合；未加载时所有指针为 nullptr
     */
    CudaFunctionPointers get_functions() const;

    /**
     * @brief 获取已加载的DLL路径
     * 
     * @return DLL路径，如果未加载则返回空字符串
     */
    std::string get_dll_path() const;

private:
    /**
     * @brief 私有构造函数（单例模式）
     */
    CudaLoader();

    /**
     * @brief 搜索CUDA DLL
     * 
     * 按优先级搜索：
     * 1. 环境变量CUDA_PATH
     * 2. 常见安装路径（Windows: Program Files, Linux: /usr/local/cuda）
     * 3. 系统PATH/LD_LIBRARY_PATH
     * 
     * @return DLL路径，如果未找到则返回空字符串
     */
    std::string search_cuda_dll();

    /**
     * @brief 尝试从指定路径加载DLL
     * 
     * @param dll_path DLL完整路径
     * @return 是否加载成功
     */
    bool try_load_dll(const std::string& dll_path);

    /**
     * @brief 加载所有CUDA函数指针
     * 
     * @return 是否所有函数都加载成功
     */
    bool load_functions();

    void unload_locked();

    /**
     * @brief 获取函数指针（平台无关）
     * 
     * @param function_name 函数名称
     * @return 函数指针，失败返回nullptr
     */
    void* get_function_pointer(const char* function_name);

    /**
     * @brief 搜索Windows常见安装路径
     */
#ifdef _WIN32
    std::string search_windows_paths();
#else
    std::string search_linux_paths();
#endif

private:
    mutable std::mutex mutex_;                    // 保护加载状态
    bool is_loaded_;                              // 是否已加载
    std::string dll_path_;                       // DLL路径
    
#ifdef _WIN32
    HMODULE dll_handle_;                          // Windows DLL句柄
#else
    void* dll_handle_;                           // Linux共享库句柄
#endif

    CudaFunctionPointers functions_;              // CUDA函数指针集合
    std::shared_ptr<LoadedLibraryLease::Library> library_;
    std::weak_ptr<LoadedLibraryLease::Library> last_library_;
    std::function<void*(const char*)> function_resolver_;
};

} // namespace gpu
} // namespace kairo
