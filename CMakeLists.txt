cmake_minimum_required(VERSION 3.16)
project(kairo LANGUAGES CXX VERSION 0.7.0)

# CR-085: 版本单一来源。project(VERSION) 是唯一事实来源；这里生成代码侧可
# include 的 kairo/version.hpp（构建树经 kairo 目标的 BUILD_INTERFACE
# include 路径暴露，安装树经下方 install(FILES) 落到 include/kairo/），
# 打包脚本也从 CMakeLists.txt 提取默认版本，不再各自硬编码。
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/kairo/version.hpp"
    @ONLY
)
# 供 src/CMakeLists.txt 挂到 kairo 目标的 BUILD_INTERFACE include 路径
set(KAIRO_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}")

# C++ 标准设置
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 构建选项
option(KAIRO_BUILD_TESTS "Build tests" ON)
option(KAIRO_BUILD_EXAMPLES "Build examples" OFF)
option(KAIRO_BUILD_SHARED "Build shared library" OFF)
option(KAIRO_ENABLE_REALTIME_ALLOCATION_GUARD
       "Enable Linux diagnostic allocation tracking for explicitly guarded realtime paths" OFF)

# GPU 支持选项
# Android 一期默认关闭 GPU；用户显式传入 -DKAIRO_ENABLE_GPU=ON 时仍可覆盖。
if(ANDROID AND NOT DEFINED KAIRO_ENABLE_GPU)
    set(_kairo_enable_gpu_default OFF)
else()
    set(_kairo_enable_gpu_default ON)
endif()
if(ANDROID AND NOT DEFINED KAIRO_ENABLE_CUDA)
    set(_kairo_enable_cuda_default OFF)
else()
    set(_kairo_enable_cuda_default ON)
endif()
option(KAIRO_ENABLE_GPU "Enable GPU support" ${_kairo_enable_gpu_default})
option(KAIRO_ENABLE_CUDA "Enable CUDA support" ${_kairo_enable_cuda_default})
option(KAIRO_ENABLE_OPENCL "Enable OpenCL support" OFF)

# 无锁工作线程队列（需配套支持 TaskDispatcher 模板化的双模式构建）
option(KAIRO_LOCKFREE_QUEUE "Use LockFreeWorkerQueue as the worker local queue implementation" OFF)
if(KAIRO_LOCKFREE_QUEUE)
    add_definitions(-DUSE_LOCKFREE_WORKER_QUEUE)
endif()

if(KAIRO_ENABLE_REALTIME_ALLOCATION_GUARD)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_compile_definitions(KAIRO_ENABLE_REALTIME_ALLOCATION_GUARD)
    else()
        message(WARNING "Realtime allocation guard is only supported on Linux")
    endif()
endif()

# MSVC: define NOMINMAX to prevent macro conflicts with std::min/std::max
if(MSVC)
    add_compile_definitions(NOMINMAX)
    add_compile_options(/W3 /wd4324 /wd4267 /wd4244)
endif()

# 包含 CMake 模块
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

# Sanitizers 无条件包含（CR-083）：TSAN 顶层开关不依赖 Debug 构建类型（CI 的
# Release+TSAN job 依赖此路径）；Debug 建议由模块内部告警表达，而非在根文件
# 用构建类型门槛静默吞掉 Release 下的 sanitizer 请求。
include(Sanitizers)

# 可选：包含 Coverage（代码覆盖率）
include(Coverage)

# GPU/CUDA 支持检测
if(KAIRO_ENABLE_GPU)
    if(KAIRO_ENABLE_CUDA)
        # 查找 CUDA（优先使用现代方法）
        # CMake 3.18+ 支持 CUDAToolkit
        if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.18")
            find_package(CUDAToolkit QUIET)
            if(CUDAToolkit_FOUND)
                set(CUDA_FOUND TRUE)
                set(CUDA_VERSION ${CUDAToolkit_VERSION})
                set(CUDA_TOOLKIT_ROOT_DIR ${CUDAToolkit_ROOT_DIR})
                set(CUDA_LIBRARIES ${CUDAToolkit_LIBRARIES})

                # 派生 CUDA include dirs: cmake 4.x 的 CUDAToolkit 模块把 include dirs
                # 标为 INTERNAL(_cmake_CUDAToolkit_include_directories),外部变量
                # CUDAToolkit_INCLUDE_DIRS 取不到,target_link_libraries 也没接
                # CUDA::cudart 时 include 不会自动加。CUDAToolkit_BIN_DIR 已知,
                # include path = <bin>/../targets/<arch>/include (CUDA 11+ 布局)
                if(NOT CUDA_INCLUDE_DIRS AND CUDAToolkit_BIN_DIR)
                    # 从 <cuda_root>/bin 推导 <cuda_root>
                    get_filename_component(_cuda_root "${CUDAToolkit_BIN_DIR}" DIRECTORY)
                    # 探测当前构建的 target arch (x86_64-linux / aarch64-linux / ...)
                    set(_cuda_inc_candidates
                        "${_cuda_root}/targets/x86_64-linux/include"
                        "${_cuda_root}/targets/aarch64-linux/include"
                        "${_cuda_root}/include")
                    foreach(_c IN LISTS _cuda_inc_candidates)
                        if(EXISTS "${_c}/cuda_runtime.h")
                            set(CUDA_INCLUDE_DIRS "${_c}")
                            break()
                        endif()
                    endforeach()
                endif()
                # CUDAToolkit 模块的 INTERNAL include 仍可能可用,加上作为兜底
                if(_cmake_CUDAToolkit_include_directories)
                    if(CUDA_INCLUDE_DIRS)
                        list(APPEND CUDA_INCLUDE_DIRS ${_cmake_CUDAToolkit_include_directories})
                    else()
                        set(CUDA_INCLUDE_DIRS ${_cmake_CUDAToolkit_include_directories})
                    endif()
                endif()
            endif()
        endif()
        
        # 如果现代方法失败，尝试传统方法
        if(NOT CUDA_FOUND)
            find_package(CUDA QUIET)
        endif()
        
        if(CUDA_FOUND)
            message(STATUS "CUDA found: Version ${CUDA_VERSION}")
            if(CUDA_TOOLKIT_ROOT_DIR)
                message(STATUS "CUDA Toolkit Root: ${CUDA_TOOLKIT_ROOT_DIR}")
            endif()
            set(KAIRO_CUDA_ENABLED ON)
            add_definitions(-DKAIRO_ENABLE_CUDA)
        else()
            message(WARNING "CUDA support requested but CUDA not found. Disabling CUDA support.")
            set(KAIRO_CUDA_ENABLED OFF)
            set(KAIRO_ENABLE_CUDA OFF)
        endif()
    else()
        set(KAIRO_CUDA_ENABLED OFF)
    endif()

    # OpenCL 支持检测
    if(KAIRO_ENABLE_OPENCL)
        find_package(OpenCL QUIET)
        if(OpenCL_FOUND)
            message(STATUS "OpenCL found: Version ${OpenCL_VERSION_STRING}")
            set(KAIRO_OPENCL_ENABLED ON)
            add_definitions(-DKAIRO_ENABLE_OPENCL)
        else()
            message(WARNING "OpenCL support requested but OpenCL not found. Disabling OpenCL support.")
            set(KAIRO_OPENCL_ENABLED OFF)
            set(KAIRO_ENABLE_OPENCL OFF)
        endif()
    else()
        set(KAIRO_OPENCL_ENABLED OFF)
    endif()

    # 如果启用了 GPU 支持（即使 CUDA/OpenCL 未找到，也定义宏以支持其他后端）
    if(KAIRO_ENABLE_GPU)
        add_definitions(-DKAIRO_ENABLE_GPU)
    endif()
else()
    set(KAIRO_CUDA_ENABLED OFF)
    set(KAIRO_ENABLE_CUDA OFF)
endif()

# 安装目录（须在 add_subdirectory(src) 之前，src 中 install(EXPORT) 使用 CMAKE_INSTALL_LIBDIR）
include(GNUInstallDirs)

# 添加子目录
add_subdirectory(src)

if(KAIRO_BUILD_TESTS)
    enable_testing()
    include(CTest)
    add_subdirectory(tests)
endif()

if(KAIRO_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# 安装配置（kairo 目标已由 src/CMakeLists 安装并导出）
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# CR-085: 安装 configure_file 生成的版本头（源码树没有对应文件，不能并入
# install(DIRECTORY include/)）
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/kairo/version.hpp"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/kairo)

# find_package(kairo) 支持：生成并安装 kairoConfig.cmake、kairoConfigVersion.cmake
include(CMakePackageConfigHelpers)
set(KAIRO_CMAKE_CONFIG_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/kairo")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/kairoConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    cmake/kairoConfig.cmake
    "${CMAKE_CURRENT_BINARY_DIR}/kairoConfigVersion.cmake"
    DESTINATION ${KAIRO_CMAKE_CONFIG_DIR}
)
