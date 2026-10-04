#include "opencl_loader.hpp"
#include <vector>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace kairo {
namespace gpu {

static OpenCLLoader* g_instance = nullptr;
static std::once_flag g_once_flag;

OpenCLLoader& OpenCLLoader::instance() {
    std::call_once(g_once_flag, []() {
        g_instance = new OpenCLLoader();
    });
    return *g_instance;
}

OpenCLLoader::OpenCLLoader()
    : is_loaded_(false), dll_handle_(nullptr) {
}

OpenCLLoader::~OpenCLLoader() {
    unload();
}

bool OpenCLLoader::load() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (is_loaded_) {
        return true;
    }

    std::string dll_path = search_opencl_dll();
    if (dll_path.empty() || dll_handle_ == nullptr) {
        return false;
    }

    if (!load_functions()) {
        unload_locked();
        return false;
    }

    is_loaded_ = true;
    dll_path_ = dll_path;
    library_ = std::make_shared<LoadedLibraryLease::Library>(dll_handle_);
    last_library_ = library_;
    return true;
}

void OpenCLLoader::unload() {
    (void)unload_if_idle();
}

bool OpenCLLoader::unload_if_idle() {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool can_unload = library_ ? library_.use_count() == 1 : last_library_.expired();
    unload_locked();
    return can_unload;
}

void OpenCLLoader::unload_locked() {
    functions_ = OpenCLFunctionPointers{};

    if (library_) {
        library_.reset();
        dll_handle_ = nullptr;
    } else {
#ifdef _WIN32
        if (dll_handle_) {
            FreeLibrary(dll_handle_);
            dll_handle_ = nullptr;
        }
#else
        if (dll_handle_) {
            dlclose(dll_handle_);
            dll_handle_ = nullptr;
        }
#endif
    }

    is_loaded_ = false;
    dll_path_.clear();
}

bool OpenCLLoader::is_available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_loaded_;
}

OpenCLFunctionPointers OpenCLLoader::get_functions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto functions = functions_;
    functions.library_lease = LoadedLibraryLease(library_);
    return functions;
}

std::string OpenCLLoader::get_dll_path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dll_path_;
}

std::string OpenCLLoader::search_opencl_dll() {
#ifdef _WIN32
    return search_windows_paths();
#else
    return search_linux_paths();
#endif
}

bool OpenCLLoader::try_load_dll(const std::string& dll_path) {
#ifdef _WIN32
    dll_handle_ = LoadLibraryA(dll_path.c_str());
    return dll_handle_ != nullptr;
#else
    dll_handle_ = dlopen(dll_path.c_str(), RTLD_LAZY);
    return dll_handle_ != nullptr;
#endif
}

void* OpenCLLoader::get_function_pointer(const char* function_name) {
    if (!dll_handle_) {
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

bool OpenCLLoader::load_functions() {
    functions_.clGetPlatformIDs = reinterpret_cast<clGetPlatformIDsFunc>(get_function_pointer("clGetPlatformIDs"));
    functions_.clGetDeviceIDs = reinterpret_cast<clGetDeviceIDsFunc>(get_function_pointer("clGetDeviceIDs"));
    functions_.clGetDeviceInfo = reinterpret_cast<clGetDeviceInfoFunc>(get_function_pointer("clGetDeviceInfo"));
    functions_.clCreateContext = reinterpret_cast<clCreateContextFunc>(get_function_pointer("clCreateContext"));
    functions_.clReleaseContext = reinterpret_cast<clReleaseContextFunc>(get_function_pointer("clReleaseContext"));
    functions_.clCreateCommandQueue = reinterpret_cast<clCreateCommandQueueFunc>(get_function_pointer("clCreateCommandQueue"));
    functions_.clReleaseCommandQueue = reinterpret_cast<clReleaseCommandQueueFunc>(get_function_pointer("clReleaseCommandQueue"));
    functions_.clCreateBuffer = reinterpret_cast<clCreateBufferFunc>(get_function_pointer("clCreateBuffer"));
    functions_.clReleaseMemObject = reinterpret_cast<clReleaseMemObjectFunc>(get_function_pointer("clReleaseMemObject"));
    functions_.clEnqueueReadBuffer = reinterpret_cast<clEnqueueReadBufferFunc>(get_function_pointer("clEnqueueReadBuffer"));
    functions_.clEnqueueWriteBuffer = reinterpret_cast<clEnqueueWriteBufferFunc>(get_function_pointer("clEnqueueWriteBuffer"));
    functions_.clEnqueueCopyBuffer = reinterpret_cast<clEnqueueCopyBufferFunc>(get_function_pointer("clEnqueueCopyBuffer"));
    functions_.clFinish = reinterpret_cast<clFinishFunc>(get_function_pointer("clFinish"));
    functions_.clFlush = reinterpret_cast<clFlushFunc>(get_function_pointer("clFlush"));
    functions_.clCreateProgramWithSource = reinterpret_cast<clCreateProgramWithSourceFunc>(get_function_pointer("clCreateProgramWithSource"));
    functions_.clBuildProgram = reinterpret_cast<clBuildProgramFunc>(get_function_pointer("clBuildProgram"));
    functions_.clReleaseProgram = reinterpret_cast<clReleaseProgramFunc>(get_function_pointer("clReleaseProgram"));
    functions_.clCreateKernel = reinterpret_cast<clCreateKernelFunc>(get_function_pointer("clCreateKernel"));
    functions_.clReleaseKernel = reinterpret_cast<clReleaseKernelFunc>(get_function_pointer("clReleaseKernel"));
    functions_.clSetKernelArg = reinterpret_cast<clSetKernelArgFunc>(get_function_pointer("clSetKernelArg"));
    functions_.clEnqueueNDRangeKernel = reinterpret_cast<clEnqueueNDRangeKernelFunc>(get_function_pointer("clEnqueueNDRangeKernel"));
    functions_.clWaitForEvents = reinterpret_cast<clWaitForEventsFunc>(get_function_pointer("clWaitForEvents"));
    functions_.clReleaseEvent = reinterpret_cast<clReleaseEventFunc>(get_function_pointer("clReleaseEvent"));

    return functions_.is_complete();
}

#ifdef _WIN32
std::string OpenCLLoader::search_windows_paths() {
    std::vector<std::string> search_paths;

    // 环境变量
    if (const char* opencl_path = std::getenv("OPENCL_PATH")) {
        search_paths.push_back(std::string(opencl_path) + "\\bin\\OpenCL.dll");
    }

    // 常见安装路径
    search_paths.push_back("C:\\Windows\\System32\\OpenCL.dll");

    // Intel OpenCL
    if (const char* intel_path = std::getenv("INTELOCLSDKROOT")) {
        search_paths.push_back(std::string(intel_path) + "\\bin\\x64\\OpenCL.dll");
    }

    // AMD OpenCL
    if (const char* amd_path = std::getenv("AMDAPPSDKROOT")) {
        search_paths.push_back(std::string(amd_path) + "\\bin\\x86_64\\OpenCL.dll");
    }

    // 系统PATH中搜索
    search_paths.push_back("OpenCL.dll");

    for (const auto& path : search_paths) {
        if (try_load_dll(path)) {
            return path;
        }
    }

    return "";
}
#else
std::string OpenCLLoader::search_linux_paths() {
    std::vector<std::string> search_paths;

    // 环境变量
    if (const char* opencl_path = std::getenv("OPENCL_PATH")) {
        search_paths.push_back(std::string(opencl_path) + "/lib/libOpenCL.so");
    }

    // 常见安装路径
    search_paths.push_back("/usr/lib/x86_64-linux-gnu/libOpenCL.so.1");
    search_paths.push_back("/usr/lib/x86_64-linux-gnu/libOpenCL.so");
    search_paths.push_back("/usr/lib64/libOpenCL.so.1");
    search_paths.push_back("/usr/lib64/libOpenCL.so");
    search_paths.push_back("/usr/lib/libOpenCL.so.1");
    search_paths.push_back("/usr/lib/libOpenCL.so");

    // Intel OpenCL
    search_paths.push_back("/opt/intel/opencl/lib64/libOpenCL.so");

    // 系统LD_LIBRARY_PATH中搜索
    search_paths.push_back("libOpenCL.so.1");
    search_paths.push_back("libOpenCL.so");

    for (const auto& path : search_paths) {
        if (try_load_dll(path)) {
            return path;
        }
    }

    return "";
}
#endif

} // namespace gpu
} // namespace kairo
