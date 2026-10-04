#include <gtest/gtest.h>

#include <chrono>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <string>
#include <thread>

#define private public
#include "kairo/gpu/cuda_loader.hpp"
#include "kairo/gpu/opencl_loader.hpp"
#undef private

using kairo::gpu::CudaLoader;
using kairo::gpu::OpenCLLoader;

namespace {

#ifdef KAIRO_ENABLE_CUDA
cudaError_t cuda_test_symbol() {
    return cudaSuccess;
}
#endif

#ifdef KAIRO_ENABLE_OPENCL
cl_int opencl_test_symbol(cl_command_queue) {
    return CL_SUCCESS;
}
#endif

class LoaderLibraryPathScope {
public:
    LoaderLibraryPathScope() {
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        directory_ = std::filesystem::temp_directory_path() /
            ("executor_loader_failure_" + std::to_string(timestamp));
        std::filesystem::create_directories(directory_ / "lib64");
        std::filesystem::create_directories(directory_ / "lib");
        std::filesystem::create_symlink("/lib/x86_64-linux-gnu/libc.so.6",
                                        directory_ / "lib64/libcudart.so");
        std::filesystem::create_symlink("/lib/x86_64-linux-gnu/libc.so.6",
                                        directory_ / "lib/libOpenCL.so");
        setenv("CUDA_PATH", directory_.c_str(), 1);
        setenv("OPENCL_PATH", directory_.c_str(), 1);
    }

    ~LoaderLibraryPathScope() {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

private:
    std::filesystem::path directory_;
};

template <typename Loader>
void verify_failed_load_can_retry(Loader& loader) {
    loader.unload();
    LoaderLibraryPathScope library_path;
    loader.function_resolver_ = [](const char*) { return nullptr; };

    auto failed_load = std::async(std::launch::async, [&loader] { return loader.load(); });
    ASSERT_EQ(failed_load.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_FALSE(failed_load.get());
    EXPECT_EQ(loader.dll_handle_, nullptr);
    EXPECT_FALSE(loader.get_functions().is_complete());

    loader.function_resolver_ = [](const char*) { return reinterpret_cast<void*>(&std::rand); };
    EXPECT_TRUE(loader.load());
    loader.unload();
    loader.function_resolver_ = {};
}

template <typename Loader, typename InvokeTestSymbol>
void verify_function_table_lease_survives_concurrent_unload(
    Loader& loader, InvokeTestSymbol invoke_test_symbol) {
#if !defined(KAIRO_ENABLE_CUDA) && !defined(KAIRO_ENABLE_OPENCL)
    GTEST_SKIP() << "CUDA and OpenCL are disabled";
#else
    loader.unload();
    LoaderLibraryPathScope library_path;
    loader.function_resolver_ = [](const char* function_name) {
#ifdef KAIRO_ENABLE_CUDA
        if (std::string(function_name) == "cudaGetLastError") {
            return reinterpret_cast<void*>(&cuda_test_symbol);
        }
#endif
#ifdef KAIRO_ENABLE_OPENCL
        if (std::string(function_name) == "clFinish") {
            return reinterpret_cast<void*>(&opencl_test_symbol);
        }
#endif
        return reinterpret_cast<void*>(&std::rand);
    };
    ASSERT_TRUE(loader.load());

    auto functions = loader.get_functions();
    ASSERT_TRUE(functions.library_lease.valid());

    std::atomic<bool> saw_deferred_unload{false};
    std::thread unloader([&] {
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (!loader.unload_if_idle()) {
                saw_deferred_unload.store(true, std::memory_order_relaxed);
            }
        }
    });
    unloader.join();

    EXPECT_TRUE(saw_deferred_unload.load(std::memory_order_relaxed));
    invoke_test_symbol(functions);

    functions = {};
    EXPECT_TRUE(loader.unload_if_idle());
    EXPECT_EQ(loader.dll_handle_, nullptr);
    loader.function_resolver_ = {};
#endif
}

}  // namespace

TEST(CudaLoaderTest, LoaderLoadFunctionsFailureDoesNotDeadlock) {
    verify_failed_load_can_retry(CudaLoader::instance());
}

TEST(OpenCLLoaderTest, LoaderLoadFunctionsFailureDoesNotDeadlock) {
    verify_failed_load_can_retry(OpenCLLoader::instance());
}

#ifdef KAIRO_ENABLE_CUDA
TEST(CudaLoaderTest, LoaderFunctionTableLeaseSurvivesConcurrentUnload) {
    verify_function_table_lease_survives_concurrent_unload(
        CudaLoader::instance(), [](const auto& functions) {
            ASSERT_NE(functions.cudaGetLastError, nullptr);
            EXPECT_EQ(functions.cudaGetLastError(), cudaSuccess);
        });
}
#endif

#ifdef KAIRO_ENABLE_OPENCL
TEST(OpenCLLoaderTest, LoaderFunctionTableLeaseSurvivesConcurrentUnload) {
    verify_function_table_lease_survives_concurrent_unload(
        OpenCLLoader::instance(), [](const auto& functions) {
            ASSERT_NE(functions.clFinish, nullptr);
            EXPECT_EQ(functions.clFinish(nullptr), CL_SUCCESS);
        });
}
#endif
