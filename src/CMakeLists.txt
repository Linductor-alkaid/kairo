# src/CMakeLists.txt
# 配置 kairo 库的构建

# 收集源文件
# 注意：当前阶段还没有源文件，后续阶段会添加
file(GLOB_RECURSE KAIRO_SOURCES CONFIGURE_DEPENDS
    "kairo/*.cpp"
    "kairo/*.c"
)

# 如果未启用任何GPU后端，排除GPU相关源文件
if(NOT KAIRO_CUDA_ENABLED AND NOT KAIRO_OPENCL_ENABLED)
    list(FILTER KAIRO_SOURCES EXCLUDE REGEX ".*gpu.*")
    # gpu_scheduler 不依赖 GPU 后端，始终编译
    list(APPEND KAIRO_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/kairo/gpu/gpu_scheduler.cpp")
    # 性能优化器不依赖 GPU 后端，始终编译
    list(APPEND KAIRO_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/kairo/gpu/kernel_launch_optimizer.cpp")
    list(APPEND KAIRO_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/kairo/gpu/transfer_optimizer.cpp")
    list(APPEND KAIRO_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/kairo/gpu/task_scheduler_optimizer.cpp")
else()
    # 如果未启用CUDA，排除CUDA特定文件
    if(NOT KAIRO_CUDA_ENABLED)
        list(FILTER KAIRO_SOURCES EXCLUDE REGEX ".*cuda.*")
    endif()
    # 如果未启用OpenCL，排除OpenCL特定文件
    if(NOT KAIRO_OPENCL_ENABLED)
        list(FILTER KAIRO_SOURCES EXCLUDE REGEX ".*opencl.*")
    endif()
endif()

# 收集头文件（用于 IDE 显示）
file(GLOB_RECURSE KAIRO_HEADERS CONFIGURE_DEPENDS
    "kairo/*.hpp"
    "kairo/*.h"
)

# 供 tests/ 的 KAIRO_STOP_TOKEN_FORCE_FALLBACK 实例化目标复用同一份
# 源列表（fallback 必须整库重编，避免 StopToken 类型跨 TU 的 ODR 冲突）。
set(KAIRO_LIB_SOURCES "${KAIRO_SOURCES}" CACHE INTERNAL "kairo library sources")

# 确定库类型
if(KAIRO_BUILD_SHARED)
    # 库尚未声明任何 dllexport/可见性注解：MSVC 下 DLL 不会导出符号，
    # 消费者链接必然失败。在配置期显式拒绝，而不是构建出一个不可用的 DLL。
    # CR-086: 守卫覆盖 MSVC 与非 MinGW 的 WIN32 工具链（clang-cl 会同时命中
    # 两者；clang-gnu-on-Windows 只命中后者）——MinGW 默认全符号导出，不在
    # 拒绝之列。
    if(MSVC OR (WIN32 AND NOT MINGW))
        message(FATAL_ERROR
            "KAIRO_BUILD_SHARED=ON is not supported with this Windows toolchain "
            "(MSVC/clang on Windows): the library does "
            "not declare dllexport annotations, so the DLL would export no symbols "
            "and any consumer would fail to link. Use the default static build "
            "(KAIRO_BUILD_SHARED=OFF).")
    endif()
    set(LIBRARY_TYPE SHARED)
else()
    set(LIBRARY_TYPE STATIC)
endif()

# 如果没有源文件，创建一个空的源文件占位符
if(NOT KAIRO_SOURCES)
    message(STATUS "No source files found. Creating placeholder library.")
    # 创建一个空的源文件
    set(PLACEHOLDER_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/kairo_placeholder.cpp")
    file(WRITE ${PLACEHOLDER_SOURCE} "// Placeholder source file\n// This will be replaced in later stages\nnamespace kairo {\n    void placeholder() {}\n}\n")
    list(APPEND KAIRO_SOURCES ${PLACEHOLDER_SOURCE})
endif()

# 创建库目标
add_library(kairo ${LIBRARY_TYPE}
    ${KAIRO_SOURCES}
    ${KAIRO_HEADERS}
)
add_library(kairo::kairo ALIAS kairo)
kairo_enable_warnings(kairo)

# 设置包含目录
# PUBLIC: 使用此库的项目也会获得这些包含目录
# PRIVATE: 仅此库内部使用
target_include_directories(kairo
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
        # CR-085: 生成的 kairo/version.hpp 位于根二进制目录
        $<BUILD_INTERFACE:${KAIRO_GENERATED_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# GPU 支持宏定义
if(KAIRO_ENABLE_GPU)
    target_compile_definitions(kairo PUBLIC KAIRO_ENABLE_GPU)
endif()

# CUDA 支持（如果启用）
# 注意：使用动态加载，不链接CUDA库，只包含头文件用于类型定义
if(KAIRO_CUDA_ENABLED)
    # 添加 CUDA 包含目录（仅用于类型定义，不链接库）
    if(CUDA_INCLUDE_DIRS)
        target_include_directories(kairo
            SYSTEM PRIVATE
                ${CUDA_INCLUDE_DIRS}
        )
    elseif(CUDAToolkit_INCLUDE_DIRS)
        target_include_directories(kairo
            SYSTEM PRIVATE
                ${CUDAToolkit_INCLUDE_DIRS}
        )
    endif()

    # 不链接CUDA库，使用动态加载
    # CudaLoader会在运行时动态搜索和加载CUDA DLL
endif()

# OpenCL 支持（如果启用）
# 注意：使用动态加载，不链接OpenCL库，只包含头文件用于类型定义
if(KAIRO_OPENCL_ENABLED)
    # 添加 OpenCL 包含目录（仅用于类型定义，不链接库）
    if(OpenCL_INCLUDE_DIRS)
        target_include_directories(kairo
            SYSTEM PRIVATE
                ${OpenCL_INCLUDE_DIRS}
        )
    endif()

    # 不链接OpenCL库，使用动态加载
    # OpenCLLoader会在运行时动态搜索和加载OpenCL库
endif()

# 平台特定的链接库
if(UNIX AND NOT APPLE)
    # Linux/Android: pthread 用于 thread_utils 中的线程调度函数
    find_package(Threads REQUIRED)
    target_link_libraries(kairo PUBLIC Threads::Threads)
    if(ANDROID)
        # bionic 已把 pthread / rt / sched 函数合并进 libc；Android 没有 librt，
        # arm64 等 ABI 也不需要把 libatomic 作为公共依赖导出。
    else()
        # Clang may lower std::atomic<T>::is_lock_free() to the libatomic runtime.
        # kairo is commonly static, so expose this as a transitive dependency
        # for examples and downstream consumers as well.
        target_link_libraries(kairo PUBLIC atomic)
        # 链接rt库（用于某些Linux系统上的实时调度函数）
        target_link_libraries(kairo PRIVATE rt)
    endif()
elseif(WIN32)
    # Windows Multimedia API（timeBeginPeriod / timeEndPeriod）
    target_link_libraries(kairo PRIVATE winmm)
endif()

# 设置 C++ 标准（确保与根 CMakeLists.txt 一致）
target_compile_features(kairo PUBLIC cxx_std_20)

if(KAIRO_BUILD_TESTS)
    # NN-01 修复后该宏不再影响 ThreadPool 类布局（测试钩子无条件存在），
    # 仅为兼容既有测试 TU 保留定义；BUILD_INTERFACE genexp 保证宏不进入
    # 安装导出的 kairoTargets.cmake，下游不会被注入该定义。
    target_compile_definitions(kairo PUBLIC
        $<BUILD_INTERFACE:KAIRO_THREAD_POOL_TEST_HOOKS>)
endif()

# MSVC: 设置源文件编码为UTF-8，避免C4819警告
if(MSVC)
    target_compile_options(kairo PRIVATE /utf-8)
endif()

# 设置输出名称和版本
if(WIN32)
    # Windows: 动态库需要设置输出名称和导入库名称
    if(KAIRO_BUILD_SHARED)
        set_target_properties(kairo PROPERTIES
            OUTPUT_NAME kairo
            VERSION ${PROJECT_VERSION}
        )
        # 对于多配置生成器（如 Visual Studio），需要为每个配置设置输出目录
        foreach(config Release Debug RelWithDebInfo MinSizeRel)
            string(TOUPPER ${config} config_upper)
            set_target_properties(kairo PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY_${config_upper} ${CMAKE_BINARY_DIR}/bin/${config}
                LIBRARY_OUTPUT_DIRECTORY_${config_upper} ${CMAKE_BINARY_DIR}/lib/${config}
                ARCHIVE_OUTPUT_DIRECTORY_${config_upper} ${CMAKE_BINARY_DIR}/lib/${config}
            )
        endforeach()
    else()
        # 静态库
        set_target_properties(kairo PROPERTIES
            OUTPUT_NAME kairo
        )
        # 对于多配置生成器，为每个配置设置输出目录
        foreach(config Release Debug RelWithDebInfo MinSizeRel)
            string(TOUPPER ${config} config_upper)
            set_target_properties(kairo PROPERTIES
                ARCHIVE_OUTPUT_DIRECTORY_${config_upper} ${CMAKE_BINARY_DIR}/lib/${config}
            )
        endforeach()
    endif()
else()
    # Linux/Unix
    set_target_properties(kairo PROPERTIES
        OUTPUT_NAME kairo
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )
endif()

# 安装目标
install(TARGETS kairo
    EXPORT kairoTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# 安装导出文件（用于 find_package）
install(EXPORT kairoTargets
    FILE kairoTargets.cmake
    NAMESPACE kairo::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/kairo
)

# 安装 src/kairo/ 下的公开头文件（CudaExecutor / OpenCLExecutor 等公开 API）
# 这些头文件 src 内部依赖 include/ 下的核心头，用户安装后通过 find_package(kairo)
# 即可获得完整 include path（CudaExecutor 的 include path = ${INSTALL_INCLUDEDIR}）
install(DIRECTORY kairo/gpu/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/kairo/gpu
    FILES_MATCHING PATTERN "*.hpp"
    PATTERN "*.cpp" EXCLUDE
)

# CR-084: gpu 头文件的依赖闭包必须一并安装——cuda_executor.hpp / opencl_executor.hpp
# 相对包含 ../util/exception_handler.hpp，缺失时下游包含 gpu 头直接编译失败。
# （util 头仅依赖标准库，闭包到此为止；CI/本地由 install_headers_smoke 测试兜底。）
install(DIRECTORY kairo/util/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/kairo/util
    FILES_MATCHING PATTERN "*.hpp"
    PATTERN "*.cpp" EXCLUDE
)

# CR-080: 库的覆盖率插桩。Coverage.cmake 在 add_subdirectory(src) 之前 include，
# 旧版 include 期 if(TARGET kairo) 恒假导致库从未被插桩；改为目标创建后显式调用。
if(COMMAND kairo_apply_coverage_to_target)
    kairo_apply_coverage_to_target(kairo)
    # 库插桩后，消费方可执行文件（tests/examples/find_package 下游）链接时也必须
    # 引入 gcov 运行时，否则 undefined reference to __gcov_init/__gcov_exit/
    # __gcov_merge_add（静态库的插桩符号在最终链接落地）。INTERFACE 让所有
    # 链接 kairo 的目标自动继承，无需逐个补 flags。
    if(KAIRO_ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_link_options(kairo INTERFACE --coverage)
    endif()
endif()
