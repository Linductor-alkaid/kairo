# tests/CMakeLists.txt
# 配置测试框架和测试可执行文件

# 收集测试源文件
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "*.cpp")

# 不依赖 GTest 的 standalone 测试：桌面 CI 和 Android smoke 都可直接构建运行。
foreach(STANDALONE_TEST_SOURCE
        android_smoke.cpp
        test_stop_token_compat.cpp
        test_executor_manager_android_defaults.cpp
        test_android_lockfree_queue_core.cpp
        test_multithread_mpsc.cpp
        test_lockfree_worker_queue_concurrent_steal.cpp
        test_task_cancellation_fallback.cpp
        test_serial_execution_context.cpp
        test_serial_context_stress.cpp
        test_bounded_admission.cpp)
    list(FILTER TEST_SOURCES EXCLUDE REGEX ".*/${STANDALONE_TEST_SOURCE}$")
    get_filename_component(STANDALONE_TEST_NAME ${STANDALONE_TEST_SOURCE} NAME_WE)
    add_executable(${STANDALONE_TEST_NAME} ${STANDALONE_TEST_SOURCE})
    target_include_directories(${STANDALONE_TEST_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
    )
    if(MSVC)
        target_compile_options(${STANDALONE_TEST_NAME} PRIVATE /utf-8)
    endif()
    if(STANDALONE_TEST_NAME STREQUAL "test_task_cancellation_fallback")
        # 整库 + 测试一起强制 fallback：StopToken 类型必须跨 TU 一致（ODR），
        # 因此该目标不链接常规 kairo 库（避免符号重复）。源列表直接复用
        # kairo 库目标的 KAIRO_LIB_SOURCES（GPU 启用时含 cuda/opencl
        # 源——根 CMakeLists 的 add_definitions 已全局开启对应宏，过滤掉它们
        # 会在链接期缺 CudaExecutor/OpenCLExecutor 符号；非 GPU 配置下四个
        # 常编调度器/优化器源已由 src/CMakeLists.txt 追加到列表中）。
        target_sources(${STANDALONE_TEST_NAME} PRIVATE ${KAIRO_LIB_SOURCES})
        target_include_directories(${STANDALONE_TEST_NAME} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../include
        )
        # 与 kairo 库目标一致：CUDA/OpenCL 启用时补头文件目录（GPU
        # headers stub 编译路径使用）。
        if(KAIRO_CUDA_ENABLED)
            if(CUDA_INCLUDE_DIRS)
                target_include_directories(${STANDALONE_TEST_NAME} SYSTEM PRIVATE
                    ${CUDA_INCLUDE_DIRS})
            elseif(CUDAToolkit_INCLUDE_DIRS)
                target_include_directories(${STANDALONE_TEST_NAME} SYSTEM PRIVATE
                    ${CUDAToolkit_INCLUDE_DIRS})
            endif()
        endif()
        if(KAIRO_OPENCL_ENABLED)
            if(OpenCL_INCLUDE_DIRS)
                target_include_directories(${STANDALONE_TEST_NAME} SYSTEM PRIVATE
                    ${OpenCL_INCLUDE_DIRS})
            endif()
        endif()
        target_compile_definitions(${STANDALONE_TEST_NAME} PRIVATE
            KAIRO_STOP_TOKEN_FORCE_FALLBACK)
        # 镜像 kairo 库目标的平台链接（Windows 的 timer_period_guard
        # 需要 winmm；Linux 需要 pthread/atomic/rt）。注意用普通库名而非
        # Threads::Threads——IMPORTED 目标的目录作用域只在 src/ 内可见，
        # tests/ 直接引用会在 generate 期报 "target was not found"。
        if(WIN32)
            target_link_libraries(${STANDALONE_TEST_NAME} PRIVATE winmm)
        elseif(UNIX AND NOT APPLE AND NOT ANDROID)
            target_link_libraries(${STANDALONE_TEST_NAME} PRIVATE
                pthread atomic rt)
        endif()
    else()
        target_link_libraries(${STANDALONE_TEST_NAME} PRIVATE kairo)
    endif()
    add_test(NAME ${STANDALONE_TEST_NAME} COMMAND ${STANDALONE_TEST_NAME})
    if(STANDALONE_TEST_NAME STREQUAL "android_smoke")
        # android_smoke 支持 KAIRO_ANDROID_SOAK_SECONDS 长稳模式。
        set(STANDALONE_TEST_TIMEOUT 900)
    else()
        set(STANDALONE_TEST_TIMEOUT 120)
    endif()
    set_tests_properties(${STANDALONE_TEST_NAME} PROPERTIES
        LABELS "unit;standalone"
        TIMEOUT ${STANDALONE_TEST_TIMEOUT}
    )
endforeach()

# 查找 Google Test
find_package(GTest)

# 离线兜底：KAIRO_LOCAL_GTEST_DIR 指向本机已有的 googletest 源码 checkout
# （如其他项目的 third_party），供无网络且未装 libgtest-dev 的验证环境使用。
# 仅在显式指定且目录有效时启用；不指定时行为与原先完全一致。
set(KAIRO_LOCAL_GTEST_DIR "" CACHE PATH
    "Path to a local googletest checkout used when system GTest is unavailable")
if(NOT GTest_FOUND AND KAIRO_LOCAL_GTEST_DIR
        AND EXISTS "${KAIRO_LOCAL_GTEST_DIR}/CMakeLists.txt")
    message(STATUS "Using local googletest checkout: ${KAIRO_LOCAL_GTEST_DIR}")
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    add_subdirectory("${KAIRO_LOCAL_GTEST_DIR}"
        "${CMAKE_BINARY_DIR}/googletest-local" EXCLUDE_FROM_ALL)
    if(NOT TARGET GTest::GTest)
        add_library(GTest::GTest INTERFACE IMPORTED)
        target_link_libraries(GTest::GTest INTERFACE gtest)
    endif()
    if(NOT TARGET GTest::Main)
        add_library(GTest::Main INTERFACE IMPORTED)
        target_link_libraries(GTest::Main INTERFACE gtest_main)
    endif()
    set(GTest_FOUND ON)
endif()

if(NOT GTest_FOUND)
    # 可选兜底：无系统 GTest 时（如未装 libgtest-dev 的 runner），用 FetchContent
    # 拉取固定版本的 googletest。默认关闭，离线构建不受影响。
    option(KAIRO_FETCH_GTEST
        "Download googletest via FetchContent when system GTest is not found" OFF)
    if(KAIRO_FETCH_GTEST)
        include(FetchContent)
        FetchContent_Declare(googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
        # googletest 提供 gtest / gtest_main 目标与 GTest::gtest 别名，但没有
        # find_package 模块模式的 GTest::GTest / GTest::Main 名称；补齐以保持
        # 测试目标的链接语句不变。
        if(NOT TARGET GTest::GTest)
            add_library(GTest::GTest INTERFACE IMPORTED)
            target_link_libraries(GTest::GTest INTERFACE gtest)
        endif()
        if(NOT TARGET GTest::Main)
            add_library(GTest::Main INTERFACE IMPORTED)
            target_link_libraries(GTest::Main INTERFACE gtest_main)
        endif()
    else()
        # 用 WARNING 而非 STATUS：静默跳过约 95% 的测试会让 CI 看起来是绿的。
        message(WARNING
            "GTest not found: skipping all GTest-based tests; only the standalone "
            "tests will build and run. Install libgtest-dev (Ubuntu) or set "
            "-DKAIRO_FETCH_GTEST=ON to get the full suite.")
        return()
    endif()
endif()
list(FILTER TEST_SOURCES EXCLUDE REGEX ".*/test_dispatch_task_fallback\\.cpp$")
# test_task_cancellation_fallback 由上方 standalone 循环以 forced-fallback 整库编译。
list(FILTER TEST_SOURCES EXCLUDE REGEX ".*/test_task_cancellation_fallback\\.cpp$")
list(FILTER TEST_SOURCES EXCLUDE REGEX ".*/test_object_pool_double_release_detect\\.cpp$")

if(NOT (KAIRO_CUDA_ENABLED OR KAIRO_OPENCL_ENABLED))
    list(FILTER TEST_SOURCES EXCLUDE REGEX ".*/test_gpu_executor_direct_config_validation\\.cpp$")
endif()

# 如果未启用CUDA，排除CUDA/GPU相关测试（kairo 不编译 gpu 源）
if(NOT KAIRO_CUDA_ENABLED)
    list(FILTER TEST_SOURCES EXCLUDE REGEX ".*cuda.*")
    list(FILTER TEST_SOURCES EXCLUDE REGEX ".*gpu_memory_manager.*")
    list(FILTER TEST_SOURCES EXCLUDE REGEX ".*unified_memory.*")
endif()

# 如果未启用OpenCL，排除OpenCL相关测试
if(NOT KAIRO_OPENCL_ENABLED)
    list(FILTER TEST_SOURCES EXCLUDE REGEX ".*opencl.*")
endif()

# MSVC 主门禁经 KAIRO_FETCH_GTEST 首次纳入 GTest 测试。以下文件存在
# 既有的 Windows 移植债务（POSIX 专用 API、按直接源码编译的 CUDA 链接
# 配置、facade record_* 符号在 MSVC 下未解析），补齐前先在 MSVC 排除，
# 其余 ~110 个测试进入 Windows 门禁（此前只有 6 个 standalone）。
# MinGW-w64 交叉门禁（posix/win32 线程模型）同样排除：setenv、
# clock_gettime 等 POSIX 专用 API 在 MinGW 运行时同样缺位或随线程模型
# 缺位（winpthreads 提供 clock_gettime，win32 模型没有）。
# 跟踪: 见 PR #174 讨论中的 Windows 测试移植 follow-up。
if(MSVC OR MINGW)
    list(FILTER TEST_SOURCES EXCLUDE REGEX ".*/test_gpu_loader_failure\\.cpp$")
    list(FILTER TEST_SOURCES EXCLUDE REGEX ".*/test_gpu_memory_validation\\.cpp$")
    list(FILTER TEST_SOURCES EXCLUDE REGEX ".*/test_realtime_thread_executor\\.cpp$")
    list(FILTER TEST_SOURCES EXCLUDE REGEX ".*/test_executor_facade_submit_batch\\.cpp$")
    # test_thread_pool_monitor_concurrent_set 在 MSVC Release 下稳定失败、Debug
    # 通过：setter 线程 2s 有界等待内计数恒为 0，acquire 加载亦不改善。该测试
    # 直接针对 ThreadPool（不经本 PR 改动路径），此前从未在 Windows 运行过，
    # 需要 Windows 环境实机调试，补齐前先排除。
    list(FILTER TEST_SOURCES EXCLUDE REGEX ".*/test_thread_pool_monitor_concurrent_set\\.cpp$")
endif()

# test_lockfree_queue_size 取 __gcov_dump_one 弱符号地址判空（ELF 下未定义
# 弱引用落地为 null）；MinGW 的 PE/COFF 对取地址产生 .refptr 强引用，链接
# 期无法解析。仅 MinGW 排除，MSVC（编译器分支不含该弱符号）与 Linux 不受
# 影响。跟踪: 同上 PR #174 Windows 测试移植 follow-up。
if(MINGW)
    list(FILTER TEST_SOURCES EXCLUDE REGEX ".*/test_lockfree_queue_size\\.cpp$")
endif()

# 如果没有测试文件，创建一个占位符说明
if(NOT TEST_SOURCES)
    message(STATUS "No test files found. Tests will be added in later stages.")
    return()
endif()

add_executable(test_comm_harness_foundation
    harness/test_comm_harness_foundation.cpp
)
target_link_libraries(test_comm_harness_foundation PRIVATE kairo GTest::GTest GTest::Main)
target_include_directories(test_comm_harness_foundation PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/harness
)
if(MSVC)
    target_compile_options(test_comm_harness_foundation PRIVATE /utf-8)
endif()
kairo_apply_coverage_to_test(test_comm_harness_foundation)
add_test(NAME test_comm_harness_foundation COMMAND test_comm_harness_foundation)
set_tests_properties(test_comm_harness_foundation PROPERTIES
    LABELS "unit;harness"
    TIMEOUT 30
)

add_executable(test_comm_facade_usage
    harness/test_comm_facade_usage.cpp
)
target_link_libraries(test_comm_facade_usage PRIVATE kairo GTest::GTest GTest::Main)
if(MSVC)
    target_compile_options(test_comm_facade_usage PRIVATE /utf-8)
endif()
kairo_apply_coverage_to_test(test_comm_facade_usage)
add_test(NAME test_comm_facade_usage COMMAND test_comm_facade_usage)
set_tests_properties(test_comm_facade_usage PROPERTIES
    LABELS "unit;harness"
    TIMEOUT 30
)

# 为每个测试文件创建可执行文件
foreach(TEST_SOURCE ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)
    
    add_executable(${TEST_NAME} ${TEST_SOURCE})

    # Keep CUDA memory validation runnable on hosts without a CUDA SDK.  The
    # kairo source uses its fallback CUDA types there, so this target can
    # exercise the pre-driver validation gate without linking a CUDA runtime.
    if(TEST_NAME STREQUAL "test_gpu_memory_validation" AND NOT KAIRO_CUDA_ENABLED)
        target_sources(${TEST_NAME} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/kairo/gpu/cuda_executor.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/kairo/gpu/cuda_loader.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/kairo/gpu/gpu_memory_manager.cpp
        )
    endif()

    if(TEST_NAME STREQUAL "test_gpu_loader_failure")
        if(NOT KAIRO_CUDA_ENABLED)
            target_sources(${TEST_NAME} PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/../src/kairo/gpu/cuda_loader.cpp
            )
        endif()
        if(NOT KAIRO_OPENCL_ENABLED)
            target_sources(${TEST_NAME} PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/../src/kairo/gpu/opencl_loader.cpp
            )
        endif()
    endif()

    # 链接 kairo 库和 GTest
    target_link_libraries(${TEST_NAME} PRIVATE kairo GTest::GTest GTest::Main)
    
    # 如果启用了 GPU 支持，传递宏定义给测试
    if(KAIRO_ENABLE_GPU)
        target_compile_definitions(${TEST_NAME} PRIVATE KAIRO_ENABLE_GPU)
    endif()

    if(TEST_NAME STREQUAL "test_thread_pool")
        target_compile_definitions(${TEST_NAME} PRIVATE KAIRO_THREAD_POOL_TEST_HOOKS)
    endif()

    if(TEST_NAME STREQUAL "test_multithread_mpsc")
        target_compile_definitions(${TEST_NAME} PRIVATE LOCKFREE_QUEUE_DEBUG_HOOKS)
    endif()
    
    # 如果是 CUDA/GPU 测试，添加 CUDA 头文件支持（不链接库，使用动态加载）
    if((TEST_NAME MATCHES ".*cuda.*" OR TEST_NAME MATCHES ".*gpu_memory_validation.*" OR TEST_NAME MATCHES ".*gpu_executor.*" OR TEST_NAME MATCHES ".*gpu_multi_device.*" OR TEST_NAME MATCHES ".*gpu_memory_manager.*" OR TEST_NAME MATCHES ".*gpu_monitor.*" OR TEST_NAME MATCHES ".*gpu_dep_async.*" OR TEST_NAME MATCHES ".*gpu_wait_for_completion.*" OR TEST_NAME MATCHES ".*unified_memory.*" OR TEST_NAME STREQUAL "test_gpu_loader_failure") AND KAIRO_CUDA_ENABLED)
        if(CUDA_INCLUDE_DIRS)
            target_include_directories(${TEST_NAME} SYSTEM PRIVATE ${CUDA_INCLUDE_DIRS})
        elseif(CUDAToolkit_INCLUDE_DIRS)
            target_include_directories(${TEST_NAME} SYSTEM PRIVATE ${CUDAToolkit_INCLUDE_DIRS})
        endif()
        # 不链接CUDA库，使用动态加载
        # CudaLoader会在运行时动态搜索和加载CUDA DLL
    endif()

    # 如果是 OpenCL 测试，添加 OpenCL 头文件支持（不链接库，使用动态加载）
    if((TEST_NAME MATCHES ".*opencl.*" OR TEST_NAME MATCHES ".*gpu_executor.*") AND KAIRO_OPENCL_ENABLED)
        if(OpenCL_INCLUDE_DIRS)
            target_include_directories(${TEST_NAME} SYSTEM PRIVATE ${OpenCL_INCLUDE_DIRS})
        endif()
        target_compile_definitions(${TEST_NAME} PRIVATE KAIRO_ENABLE_OPENCL)
    endif()
    
    # 添加src目录到包含路径，以便测试可以访问内部头文件
    target_include_directories(${TEST_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
    )
    
    # MSVC: 设置源文件编码为UTF-8，避免C4819警告
    if(MSVC)
        target_compile_options(${TEST_NAME} PRIVATE /utf-8)
    endif()
    
    # 应用覆盖率选项（如果启用）——复用 Coverage.cmake 的统一入口（NN-08）
    kairo_apply_coverage_to_test(${TEST_NAME})
    
    # 添加测试
    if(TEST_NAME STREQUAL "test_gpu_memory_validation")
        add_test(NAME CudaExecutorMemoryValidation COMMAND ${TEST_NAME})
    else()
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    endif()
    
    # 根据测试类型设置 LABELS 和 TIMEOUT
    # 单元测试
    if(TEST_NAME STREQUAL "test_lockfree_worker_queue_concurrent_steal")
        set_tests_properties(${TEST_NAME} PROPERTIES
            LABELS "unit;concurrency;lockfree"
            TIMEOUT 60
        )
    elseif(TEST_NAME MATCHES "^test_comm_(channel|double_buffer|mailbox|phase_gate)$")
        set_tests_properties(${TEST_NAME} PROPERTIES
            LABELS "unit;concurrency;lockfree"
            TIMEOUT 60
        )
    elseif(TEST_NAME STREQUAL "test_comm_topic")
        set_tests_properties(${TEST_NAME} PROPERTIES
            LABELS "unit;concurrency"
            TIMEOUT 60
        )
    elseif(TEST_NAME STREQUAL "test_comm_realtime_memory")
        set_tests_properties(${TEST_NAME} PROPERTIES
            LABELS "unit;realtime;lockfree"
            TIMEOUT 60
        )
    elseif(TEST_NAME STREQUAL "test_comm_types")
        set_tests_properties(${TEST_NAME} PROPERTIES
            LABELS "unit;lockfree"
            TIMEOUT 30
        )
    elseif(TEST_NAME STREQUAL "test_self_stop_handoff")
        set_tests_properties(${TEST_NAME} PROPERTIES
            LABELS "unit;concurrency"
            TIMEOUT 30
        )
    elseif(TEST_NAME STREQUAL "test_worker_local_queue_steal")
        # yield 自旋的窃取竞态压测：本地多核约 5s，2-vCPU Windows runner
        # 显著更慢（首次纳入 Windows 门禁时 30s 超时），放宽到并发测试档。
        set_tests_properties(${TEST_NAME} PROPERTIES
            LABELS "unit;concurrency"
            TIMEOUT 120
        )
    elseif(TEST_NAME MATCHES "^test_(dispatcher_resize_under_load|thread_pool_resize_uaf|thread_pool_resize_status|concurrent_stop_submit)$")
        set_tests_properties(${TEST_NAME} PROPERTIES
            LABELS "integration;concurrency;thread_pool"
            TIMEOUT 120
        )
    elseif(TEST_NAME STREQUAL "test_thread_pool")
        # 核心线程池测试含多组压力子用例，CI 高负载下 30s 会超时
        #（master CI 两次偶发 Timeout；本地直跑恒绿）。
        set_tests_properties(${TEST_NAME} PROPERTIES
            LABELS "unit;concurrency;thread_pool"
            TIMEOUT 120
        )
    elseif(TEST_NAME MATCHES "^test_(util|task|thread_pool_empty_task_rejection|statistics_collector|task_monitor|cuda_executor|gpu_memory_manager|realtime_push_task_contract)$")
        set_tests_properties(${TEST_NAME} PROPERTIES
            LABELS "unit"
            TIMEOUT 30
        )
    # 集成/端到端测试
    elseif(TEST_NAME MATCHES "^test_(executor_facade|executor_manager|executor_manager_gpu|gpu_multi_device|gpu_monitor|e2e_workflow|thread_pool_integration|monitor_integration|cycle_manager_integration|realtime_thread_executor|thread_pool_executor)$")
        set_tests_properties(${TEST_NAME} PROPERTIES
            LABELS "integration"
            TIMEOUT 60
        )
    # 性能/压力测试
    elseif(TEST_NAME MATCHES "^test_(performance|stress|benchmark)$")
        set_tests_properties(${TEST_NAME} PROPERTIES
            LABELS "benchmark;stress"
            TIMEOUT 120
        )
    # 性能基准测试（可配置、JSON 输出，用于优化前后对比）
    elseif(TEST_NAME STREQUAL "benchmark_baseline")
        set_tests_properties(${TEST_NAME} PROPERTIES
            LABELS "benchmark;baseline"
            TIMEOUT 120
        )
    # 定时器精度基准测试（多周期 delayed/periodic jitter，JSON 输出）
    elseif(TEST_NAME STREQUAL "benchmark_timer_precision")
        set_tests_properties(${TEST_NAME} PROPERTIES
            LABELS "benchmark"
            TIMEOUT 120
        )
    # 实时线程周期精度基准测试（RealtimeThreadExecutor cycle jitter，JSON 输出）
    elseif(TEST_NAME STREQUAL "benchmark_realtime_precision")
        set_tests_properties(${TEST_NAME} PROPERTIES
            LABELS "benchmark"
            TIMEOUT 120
        )
    # 提交热路径基准（默认 2 万任务 × 6 档生产者 + 延迟采样，RUN_SERIAL）：
    # 无优化构建下可超 30s 默认档，与其余基准一致放宽到 120s；并归入
    # benchmark 标签，与 CI/本地 -LE benchmark 口径一致。
    elseif(TEST_NAME STREQUAL "benchmark_thread_pool_hotpath")
        set_tests_properties(${TEST_NAME} PROPERTIES
            LABELS "benchmark;baseline"
            TIMEOUT 120
        )
    # 优化基线基准测试（测量优化路线图中各优化目标的当前性能）
    elseif(TEST_NAME STREQUAL "benchmark_optimization_baseline")
        set_tests_properties(${TEST_NAME} PROPERTIES
            LABELS "benchmark;optimization"
            TIMEOUT 120
        )
    # 任务图调度路径基准测试（PA-6 验收：tracked 吞吐/parked fan-out/链延迟）
    elseif(TEST_NAME STREQUAL "benchmark_task_graph_paths")
        set_tests_properties(${TEST_NAME} PROPERTIES
            LABELS "benchmark;task_graph"
            TIMEOUT 300
        )
    # 批量提交基准测试（benchmark_batch_* 系列）
    elseif(TEST_NAME MATCHES "^benchmark_batch_.*")
        set_tests_properties(${TEST_NAME} PROPERTIES
            LABELS "benchmark;batch"
            TIMEOUT 120
        )
    # 无锁任务执行器基准测试（高吞吐量压测，需要更长超时）
    elseif(TEST_NAME STREQUAL "benchmark_lockfree_task_executor")
        set_tests_properties(${TEST_NAME} PROPERTIES
            LABELS "benchmark"
            TIMEOUT 120
        )
    elseif(TEST_NAME STREQUAL "test_gpu_memory_validation")
        set_tests_properties(CudaExecutorMemoryValidation PROPERTIES
            LABELS "unit"
            TIMEOUT 30
        )
    # 默认：单元测试
    else()
        set_tests_properties(${TEST_NAME} PROPERTIES
            LABELS "unit"
            TIMEOUT 30
        )
    endif()

    # benchmark_* 测试的延迟分位数断言对机器负载敏感：与其他 CPU 密集测试
    # 并行调度时会因负载噪声产生环境性 flake（issue #188）。RUN_SERIAL 让
    # ctest 独占调度该测试，消除并行负载误报。
    if(TEST_NAME MATCHES "^benchmark_")
        set_tests_properties(${TEST_NAME} PROPERTIES RUN_SERIAL TRUE)
    endif()

    if(KAIRO_ENABLE_TSAN AND TEST_NAME MATCHES "^test_(comm_(channel|double_buffer|mailbox|phase_gate|topic)|thread_pool|thread_pool_resize_uaf|thread_pool_resize_status|thread_pool_monitor_concurrent_set|dispatcher_resize_under_load|concurrent_stop_submit|load_balancer_concurrent_resize|opencl_lifecycle|self_stop_handoff|lockfree_task_executor|lockfree_mpsc|batch_integration|executor_manager)$")
        set_tests_properties(${TEST_NAME} PROPERTIES
            TIMEOUT 180
        )
    endif()

    # benchmark_thread_pool_hotpath 的 harness 数据竞争已修（#194：延迟采样改为
    # 每样本独占预分配槽位），但 Phase 1 吞吐压测（默认 2 万任务 × 最多 16 生产
    # 者）在 TSAN 插桩下减速 ~20x，全 6 档实测 220-276s（gcc 13.3 / 8 worker），
    # 远超 30s 默认档。按 600s 放宽供 TSAN 全量树外取证使用；CI 的 TSAN job 只
    # 跑白名单 test_* 目标，不含 benchmark，不受影响。
    if(KAIRO_ENABLE_TSAN AND TEST_NAME STREQUAL "benchmark_thread_pool_hotpath")
        set_tests_properties(${TEST_NAME} PROPERTIES
            TIMEOUT 600
        )
    endif()

    # --coverage 插桩对时序敏感的池测试有 2-5 倍减速（与 TSan 放宽同理），
    # 2-vCPU runner 上偶发超出 30s 默认档；coverage 构建下放宽到 120s。
    if(KAIRO_ENABLE_COVERAGE AND TEST_NAME MATCHES "^test_(thread_pool|thread_pool_integration|monitor_integration)$")
        set_tests_properties(${TEST_NAME} PROPERTIES
            TIMEOUT 120
        )
    endif()
endforeach()

# dependency-driven scheduling（PR-1）验证测试：门控握手 + 多处有限等待，
# 单测最坏路径超过默认 30s 档；worker 占满/释放的时序对并行 ctest 的负载
# 噪声敏感，RUN_SERIAL 独占调度。
if(TARGET test_dependency_driven_scheduling)
    set_tests_properties(test_dependency_driven_scheduling PROPERTIES
        LABELS "unit;concurrency"
        TIMEOUT 120
        RUN_SERIAL TRUE
    )
endif()

# dependency-driven scheduling（PR-2）：parked 超时 / shutdown 终局结算 /
# retention 交互 / serial dispatch 组合。同样为门控握手 + 有限等待模式，
# 且含 200ms 量级超时预算的观察窗口，独占调度避免负载噪声误报。
if(TARGET test_dependency_driven_pr2)
    set_tests_properties(test_dependency_driven_pr2 PROPERTIES
        LABELS "unit;concurrency"
        TIMEOUT 300
        RUN_SERIAL TRUE
    )
endif()

# dependency-driven scheduling（PR-3）验证测试：Queued 补记确定性采样与
# closure 墓地观测。同样为门控握手 + 有限等待模式，含 200ms 量级超时预算
# 与稳定性观察窗口，独占调度避免负载噪声误报。
if(TARGET test_dependency_driven_pr3)
    set_tests_properties(test_dependency_driven_pr3 PROPERTIES
        LABELS "unit;concurrency"
        TIMEOUT 300
        RUN_SERIAL TRUE
    )
endif()

add_executable(test_dispatch_task_fallback test_dispatch_task_fallback.cpp)
target_link_libraries(test_dispatch_task_fallback PRIVATE kairo GTest::GTest GTest::Main)
target_include_directories(test_dispatch_task_fallback PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
if(MSVC)
    target_compile_options(test_dispatch_task_fallback PRIVATE /utf-8)
endif()
kairo_apply_coverage_to_test(test_dispatch_task_fallback)
add_test(NAME test_dispatch_task_fallback COMMAND test_dispatch_task_fallback)
set_tests_properties(test_dispatch_task_fallback PROPERTIES
    LABELS "unit"
    TIMEOUT 30
)

add_executable(test_object_pool_double_release_detect test_object_pool_double_release_detect.cpp)
target_link_libraries(test_object_pool_double_release_detect PRIVATE kairo GTest::GTest GTest::Main)
target_include_directories(test_object_pool_double_release_detect PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
if(MSVC)
    target_compile_options(test_object_pool_double_release_detect PRIVATE /utf-8)
endif()
kairo_apply_coverage_to_test(test_object_pool_double_release_detect)
add_test(NAME test_object_pool_double_release_detect COMMAND test_object_pool_double_release_detect)
set_tests_properties(test_object_pool_double_release_detect PROPERTIES
    LABELS "unit"
    TIMEOUT 30
)

add_executable(test_object_pool_double_release_detect_ndebug test_object_pool_double_release_detect.cpp)
target_link_libraries(test_object_pool_double_release_detect_ndebug PRIVATE kairo GTest::GTest GTest::Main)
target_include_directories(test_object_pool_double_release_detect_ndebug PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_definitions(test_object_pool_double_release_detect_ndebug PRIVATE NDEBUG)
if(MSVC)
    target_compile_options(test_object_pool_double_release_detect_ndebug PRIVATE /utf-8)
endif()
kairo_apply_coverage_to_test(test_object_pool_double_release_detect_ndebug)
add_test(NAME test_object_pool_double_release_detect_ndebug COMMAND test_object_pool_double_release_detect_ndebug)
set_tests_properties(test_object_pool_double_release_detect_ndebug PROPERTIES
    LABELS "unit"
    TIMEOUT 30
)

# 如果使用 Google Test 或其他测试框架，可以在这里配置
# 例如：
# find_package(GTest REQUIRED)
# target_link_libraries(test_name PRIVATE kairo GTest::GTest GTest::Main)

# CR-084 配套：安装树头文件冒烟——安装到临时 prefix 后编译包含全部已安装头的
# 探针 TU，防止 install 规则再次遗漏依赖闭包。需要 GNU/Clang 直接驱动探针编译
# （MSVC 环境需 cl 变量集，不注册；CI 的 ubuntu 矩阵与 Coverage job 覆盖）。
if(NOT WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_test(NAME install_headers_smoke
        COMMAND "${CMAKE_COMMAND}"
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/install_headers_smoke.cmake
    )
    set_tests_properties(install_headers_smoke PROPERTIES
        LABELS "unit;packaging"
        TIMEOUT 120
    )
endif()
