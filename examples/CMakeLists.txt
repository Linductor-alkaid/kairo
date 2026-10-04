# examples/CMakeLists.txt
# 配置示例可执行文件

# 收集示例源文件
file(GLOB EXAMPLE_SOURCES "*.cpp")

# 从通用列表中排除 GPU 示例（仅在启用 GPU 时单独添加）
list(FILTER EXAMPLE_SOURCES EXCLUDE REGEX "gpu_.*\\.cpp$")

# 如果没有示例文件，创建一个占位符说明
if(NOT EXAMPLE_SOURCES)
    message(STATUS "No example files found. Examples will be added in later stages.")
    return()
endif()

# 为每个示例文件创建可执行文件
foreach(EXAMPLE_SOURCE ${EXAMPLE_SOURCES})
    get_filename_component(EXAMPLE_NAME ${EXAMPLE_SOURCE} NAME_WE)
    
    add_executable(${EXAMPLE_NAME} ${EXAMPLE_SOURCE})
    
    # 链接 kairo 库
    target_link_libraries(${EXAMPLE_NAME} PRIVATE kairo)
endforeach()

# 生命周期快照示例只使用 CPU 后端，作为发布文档中的可执行 smoke test。
if(TARGET lifecycle_snapshot AND BUILD_TESTING)
    add_test(NAME lifecycle_snapshot_example COMMAND lifecycle_snapshot)
    set_tests_properties(lifecycle_snapshot_example PROPERTIES
        LABELS "example;monitoring;smoke"
        TIMEOUT 15
    )
endif()

# 事件循环互操作示例：外部事件循环（asio strand 等）互操作指南的可编译
# 伴随示例（docs/external_event_loop_interop.md），作为指南模式的回归 smoke。
if(TARGET event_loop_interop AND BUILD_TESTING)
    add_test(NAME event_loop_interop_example COMMAND event_loop_interop)
    set_tests_properties(event_loop_interop_example PROPERTIES
        LABELS "example;interop;smoke"
        TIMEOUT 15
    )
endif()

add_subdirectory(tutorial)
if(TARGET topic_subscriptions AND BUILD_TESTING)
    add_test(NAME topic_subscriptions_example COMMAND topic_subscriptions)
    set_tests_properties(topic_subscriptions_example PROPERTIES
        LABELS "example;communication;smoke"
        TIMEOUT 15
    )
endif()

if(TARGET cycle_manager_example)
    add_dependencies(tutorial_examples cycle_manager_example)

    if(BUILD_TESTING)
        add_test(NAME cycle_manager_example COMMAND cycle_manager_example)
        set_tests_properties(cycle_manager_example PROPERTIES
            LABELS "tutorial;smoke"
            TIMEOUT 15
        )
    endif()
endif()

# GPU 示例：仅在启用 GPU 时构建
if(KAIRO_ENABLE_GPU)
    # 设备查询示例（CUDA或OpenCL任一可用即可）
    if(KAIRO_CUDA_ENABLED OR KAIRO_OPENCL_ENABLED)
        add_executable(gpu_device_query gpu_device_query.cpp)
        target_link_libraries(gpu_device_query PRIVATE kairo)
    endif()

    # CUDA 特定示例
    if(KAIRO_CUDA_ENABLED)
        set(_cuda_inc_dirs ${CUDA_INCLUDE_DIRS})
        if(NOT _cuda_inc_dirs AND CUDAToolkit_INCLUDE_DIRS)
            set(_cuda_inc_dirs ${CUDAToolkit_INCLUDE_DIRS})
        endif()

        add_executable(gpu_basic gpu_basic.cpp)
        target_link_libraries(gpu_basic PRIVATE kairo)
        target_include_directories(gpu_basic PRIVATE ${_cuda_inc_dirs})

        add_executable(gpu_multi_device gpu_multi_device.cpp)
        target_link_libraries(gpu_multi_device PRIVATE kairo)
        target_include_directories(gpu_multi_device PRIVATE ${_cuda_inc_dirs})

        # gpu_unified_memory 包含 CUDA kernel 语法，必须用 nvcc 编译
        # Ninja/Makefile 生成器下可用 enable_language(CUDA)；VS 生成器需要 CUDA toolset，跳过
        if(NOT CMAKE_GENERATOR MATCHES "Visual Studio")
            enable_language(CUDA)
            add_executable(gpu_unified_memory gpu_unified_memory.cpp)
            target_link_libraries(gpu_unified_memory PRIVATE kairo)
            set_source_files_properties(gpu_unified_memory.cpp PROPERTIES LANGUAGE CUDA)
        else()
            message(STATUS "Skipping gpu_unified_memory: Visual Studio generator requires CUDA toolset integration")
        endif()
    endif()

    # OpenCL 特定示例
    if(KAIRO_OPENCL_ENABLED)
        add_executable(gpu_opencl gpu_opencl.cpp)
        target_link_libraries(gpu_opencl PRIVATE kairo)
    endif()
endif()
