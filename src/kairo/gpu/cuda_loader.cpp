#include "cuda_loader.hpp"
#include <algorithm>
#include <vector>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#endif

namespace kairo {
namespace gpu {

// 单例实例
static CudaLoader* g_instance = nullptr;
static std::once_flag g_once_flag;

CudaLoader& CudaLoader::instance() {
    std::call_once(g_once_flag, []() {
        g_instance = new CudaLoader();
    });
    return *g_instance;
}

CudaLoader::CudaLoader()
    : is_loaded_(false)
#ifdef _WIN32
    , dll_handle_(nullptr)
#else
    , dll_handle_(nullptr)
#endif
{
}

CudaLoader::~CudaLoader() {
    unload();
}

bool CudaLoader::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (is_loaded_) {
        return true;  // 已经加载
    }

    // 搜索CUDA DLL（search_cuda_dll内部会实际加载DLL）
    std::string dll_path = search_cuda_dll();
    if (dll_path.empty() || dll_handle_ == nullptr) {
        return false;  // 未找到DLL或加载失败
    }

    // 加载函数指针
    if (!load_functions()) {
        unload_locked();
        return false;  // 函数加载失败
    }

    is_loaded_ = true;
    dll_path_ = dll_path;
    library_ = std::make_shared<LoadedLibraryLease::Library>(dll_handle_);
    last_library_ = library_;
    return true;
}

void CudaLoader::unload() {
    (void)unload_if_idle();
}

bool CudaLoader::unload_if_idle() {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool can_unload = library_ ? library_.use_count() == 1 : last_library_.expired();
    unload_locked();
    return can_unload;
}

void CudaLoader::unload_locked() {
    // 清空函数指针
    functions_ = CudaFunctionPointers{};

    // 放弃加载器持有的引用；函数表租约会延迟实际卸载。
    if (library_) {
        library_.reset();
        dll_handle_ = nullptr;
    } else {
        // 仅处理 load_functions() 失败时尚未创建租约的句柄。
#ifdef _WIN32
        if (dll_handle_ != nullptr) {
            FreeLibrary(dll_handle_);
            dll_handle_ = nullptr;
        }
#else
        if (dll_handle_ != nullptr) {
            dlclose(dll_handle_);
            dll_handle_ = nullptr;
        }
#endif
    }

    is_loaded_ = false;
    dll_path_.clear();
}

bool CudaLoader::is_available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_loaded_ && functions_.is_complete();
}

CudaFunctionPointers CudaLoader::get_functions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto functions = functions_;
    functions.library_lease = LoadedLibraryLease(library_);
    return functions;
}

std::string CudaLoader::get_dll_path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dll_path_;
}

std::string CudaLoader::search_cuda_dll() {
    // 1. 检查环境变量CUDA_PATH
#ifdef _WIN32
    const char* cuda_path = std::getenv("CUDA_PATH");
    if (cuda_path != nullptr) {
        // 尝试不同版本的DLL
        for (int version = 12; version >= 9; --version) {
            std::string dll_path = std::string(cuda_path) + "\\bin\\cudart64_" + 
                                  std::to_string(version) + ".dll";
            if (try_load_dll(dll_path)) {
                // 实际加载DLL
                dll_handle_ = LoadLibraryA(dll_path.c_str());
                if (dll_handle_ != nullptr) {
                    return dll_path;
                }
            }
        }
    }
#else
    const char* cuda_path = std::getenv("CUDA_PATH");
    if (cuda_path != nullptr) {
        // 尝试版本化库
        for (int version = 12; version >= 9; --version) {
            std::string lib_path = std::string(cuda_path) + "/lib64/libcudart.so." + 
                                  std::to_string(version);
            if (try_load_dll(lib_path)) {
                dll_handle_ = dlopen(lib_path.c_str(), RTLD_LAZY | RTLD_LOCAL);
                if (dll_handle_ != nullptr) {
                    return lib_path;
                }
            }
        }
        // 尝试非版本化库
        std::string lib_path = std::string(cuda_path) + "/lib64/libcudart.so";
        if (try_load_dll(lib_path)) {
            dll_handle_ = dlopen(lib_path.c_str(), RTLD_LAZY | RTLD_LOCAL);
            if (dll_handle_ != nullptr) {
                return lib_path;
            }
        }
    }
#endif

    // 2. 搜索常见安装路径
#ifdef _WIN32
    std::string found_path = search_windows_paths();
#else
    std::string found_path = search_linux_paths();
#endif
    if (!found_path.empty()) {
        return found_path;
    }

    // 3. 从系统PATH/LD_LIBRARY_PATH搜索
#ifdef _WIN32
    const char* cuda_dll_names[] = {
        "cudart64_12.dll",
        "cudart64_11.dll",
        "cudart64_10.dll",
        "cudart64_9.dll"
    };
    for (const char* dll_name : cuda_dll_names) {
        if (try_load_dll(dll_name)) {
            dll_handle_ = LoadLibraryA(dll_name);
            if (dll_handle_ != nullptr) {
                return dll_name;  // 从PATH加载
            }
        }
    }
#else
    const char* lib_names[] = {
        "libcudart.so.12",
        "libcudart.so.11",
        "libcudart.so.10",
        "libcudart.so.9",
        "libcudart.so"
    };
    for (const char* lib_name : lib_names) {
        if (try_load_dll(lib_name)) {
            dll_handle_ = dlopen(lib_name, RTLD_LAZY | RTLD_LOCAL);
            if (dll_handle_ != nullptr) {
                return lib_name;  // 从LD_LIBRARY_PATH加载
            }
        }
    }
#endif

    return std::string();  // 未找到
}

bool CudaLoader::try_load_dll(const std::string& dll_path) {
    // 检查文件是否存在（仅测试，不实际加载）
    // 注意：在search_cuda_dll中会实际加载DLL
#ifdef _WIN32
    // Windows: 使用GetFileAttributes检查文件是否存在
    DWORD attrs = GetFileAttributesA(dll_path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return true;  // 文件存在
    }
#else
    // Linux: 使用access检查文件是否存在
    if (access(dll_path.c_str(), R_OK) == 0) {
        return true;  // 文件存在且可读
    }
#endif
    return false;
}

bool CudaLoader::load_functions() {
    // 加载所有CUDA函数指针
    functions_.cudaFree = reinterpret_cast<CudaFreeFunc>(
        get_function_pointer("cudaFree"));
    functions_.cudaGetDeviceCount = reinterpret_cast<CudaGetDeviceCountFunc>(
        get_function_pointer("cudaGetDeviceCount"));
    functions_.cudaSetDevice = reinterpret_cast<CudaSetDeviceFunc>(
        get_function_pointer("cudaSetDevice"));
    functions_.cudaGetDeviceProperties = reinterpret_cast<CudaGetDevicePropertiesFunc>(
        get_function_pointer("cudaGetDeviceProperties"));
    functions_.cudaMalloc = reinterpret_cast<CudaMallocFunc>(
        get_function_pointer("cudaMalloc"));
    functions_.cudaMemcpy = reinterpret_cast<CudaMemcpyFunc>(
        get_function_pointer("cudaMemcpy"));
    functions_.cudaMemcpyAsync = reinterpret_cast<CudaMemcpyAsyncFunc>(
        get_function_pointer("cudaMemcpyAsync"));
    functions_.cudaDeviceSynchronize = reinterpret_cast<CudaDeviceSynchronizeFunc>(
        get_function_pointer("cudaDeviceSynchronize"));
    functions_.cudaStreamCreate = reinterpret_cast<CudaStreamCreateFunc>(
        get_function_pointer("cudaStreamCreate"));
    functions_.cudaStreamSynchronize = reinterpret_cast<CudaStreamSynchronizeFunc>(
        get_function_pointer("cudaStreamSynchronize"));
    functions_.cudaStreamDestroy = reinterpret_cast<CudaStreamDestroyFunc>(
        get_function_pointer("cudaStreamDestroy"));
    functions_.cudaMemGetInfo = reinterpret_cast<CudaMemGetInfoFunc>(
        get_function_pointer("cudaMemGetInfo"));
    // 可选：CUDA 10+ 提供，用于流回调
    functions_.cudaLaunchHostFunc = reinterpret_cast<CudaLaunchHostFuncFunc>(
        get_function_pointer("cudaLaunchHostFunc"));

    // P2P 可选：设备间拷贝
    functions_.cudaMemcpyPeer = reinterpret_cast<CudaMemcpyPeerFunc>(
        get_function_pointer("cudaMemcpyPeer"));
    functions_.cudaMemcpyPeerAsync = reinterpret_cast<CudaMemcpyPeerAsyncFunc>(
        get_function_pointer("cudaMemcpyPeerAsync"));
    functions_.cudaDeviceCanAccessPeer = reinterpret_cast<CudaDeviceCanAccessPeerFunc>(
        get_function_pointer("cudaDeviceCanAccessPeer"));
    functions_.cudaDeviceEnablePeerAccess = reinterpret_cast<CudaDeviceEnablePeerAccessFunc>(
        get_function_pointer("cudaDeviceEnablePeerAccess"));
    functions_.cudaGetLastError = reinterpret_cast<CudaGetLastErrorFunc>(
        get_function_pointer("cudaGetLastError"));
    functions_.cudaGetErrorString = reinterpret_cast<CudaGetErrorStringFunc>(
        get_function_pointer("cudaGetErrorString"));

    // Unified Memory 可选：CUDA 6.0+
    functions_.cudaMallocManaged = reinterpret_cast<CudaMallocManagedFunc>(
        get_function_pointer("cudaMallocManaged"));
    functions_.cudaMemPrefetchAsync = reinterpret_cast<CudaMemPrefetchAsyncFunc>(
        get_function_pointer("cudaMemPrefetchAsync"));

    return functions_.is_complete();
}

void* CudaLoader::get_function_pointer(const char* function_name) {
    if (dll_handle_ == nullptr) {
        return nullptr;
    }

    if (function_resolver_) {
        return function_resolver_(function_name);
    }

#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(dll_handle_, function_name));
#else
    return dlsym(dll_handle_, function_name);
#endif
}

#ifdef _WIN32
std::string CudaLoader::search_windows_paths() {
    // 搜索 Program Files\NVIDIA GPU Computing Toolkit\CUDA\v*\bin 目录
    std::vector<std::string> base_paths = {
        "C:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA",
        "C:\\Program Files (x86)\\NVIDIA GPU Computing Toolkit\\CUDA"
    };

    // 常见的CUDA版本目录（按版本号从高到低）
    std::vector<std::string> versions = {
        "v12.6", "v12.5", "v12.4", "v12.3", "v12.2", "v12.1", "v12.0",
        "v11.8", "v11.7", "v11.6", "v11.5", "v11.4", "v11.3", "v11.2", "v11.1", "v11.0",
        "v10.2", "v10.1", "v10.0",
        "v9.2", "v9.1", "v9.0"
    };

    for (const std::string& base_path : base_paths) {
        for (const std::string& version : versions) {
            // 从版本字符串中提取主版本号（例如 "v12.3" -> 12）
            int major_version = 12;  // 默认
            if (version.length() >= 3 && version[0] == 'v') {
                try {
                    major_version = std::stoi(version.substr(1));
                } catch (...) {
                    major_version = 12;  // 默认值
                }
            }
            
            // 尝试该版本对应的DLL
            std::string dll_path = base_path + "\\" + version + "\\bin\\cudart64_" + 
                                  std::to_string(major_version) + ".dll";
            if (try_load_dll(dll_path)) {
                // 实际加载DLL
                dll_handle_ = LoadLibraryA(dll_path.c_str());
                if (dll_handle_ != nullptr) {
                    return dll_path;
                }
            }
        }
    }

    return std::string();
}
#else
std::string CudaLoader::search_linux_paths() {
    // 搜索 /usr/local/cuda*/lib64/
    std::vector<std::string> search_paths = {
        "/usr/local/cuda",
        "/usr/local/cuda-12",
        "/usr/local/cuda-11",
        "/usr/local/cuda-10",
        "/usr/local/cuda-9"
    };

    for (const std::string& base_path : search_paths) {
        std::string lib_path = base_path + "/lib64/libcudart.so";
        if (try_load_dll(lib_path)) {
            dll_handle_ = dlopen(lib_path.c_str(), RTLD_LAZY | RTLD_LOCAL);
            if (dll_handle_ != nullptr) {
                return lib_path;
            }
        }
        // 尝试版本化库
        for (int version = 12; version >= 9; --version) {
            lib_path = base_path + "/lib64/libcudart.so." + std::to_string(version);
            if (try_load_dll(lib_path)) {
                dll_handle_ = dlopen(lib_path.c_str(), RTLD_LAZY | RTLD_LOCAL);
                if (dll_handle_ != nullptr) {
                    return lib_path;
                }
            }
        }
    }

    // 搜索 /usr/lib/x86_64-linux-gnu/
    std::string system_path = "/usr/lib/x86_64-linux-gnu/libcudart.so";
    if (try_load_dll(system_path)) {
        dll_handle_ = dlopen(system_path.c_str(), RTLD_LAZY | RTLD_LOCAL);
        if (dll_handle_ != nullptr) {
            return system_path;
        }
    }

    return std::string();
}
#endif

} // namespace gpu
} // namespace kairo
