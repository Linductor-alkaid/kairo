# Coverage.cmake
# 提供代码覆盖率支持（gcov/lcov）
# 用于 GCC/Clang 编译器，生成 HTML 覆盖率报告
#
# CR-080 修复后的加载契约：本文件在 add_subdirectory(src) 之前被根
# CMakeLists.txt include，此时 kairo 目标尚不存在——库的插桩必须由
# src/CMakeLists.txt 在 add_library(kairo) 之后调用
# kairo_apply_coverage_to_target() 完成，而不是像旧版那样在 include
# 期用 if(TARGET kairo)（恒假，库永远拿不到 --coverage）。

# 选项：启用覆盖率
option(KAIRO_ENABLE_COVERAGE "Enable code coverage (gcov/lcov)" OFF)

if(KAIRO_ENABLE_COVERAGE)
    # 检查编译器
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # 检查是否在 Debug 模式下（推荐）
        if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT CMAKE_BUILD_TYPE STREQUAL "")
            message(WARNING "Coverage is typically enabled in Debug mode. Current build type: ${CMAKE_BUILD_TYPE}")
        endif()

        # 检查 lcov 是否可用
        find_program(LCOV_PATH lcov)
        find_program(GENHTML_PATH genhtml)

        if(NOT LCOV_PATH OR NOT GENHTML_PATH)
            message(WARNING "lcov or genhtml not found. Coverage report generation may fail.")
            message(WARNING "  Install with: sudo apt-get install lcov (Ubuntu/Debian)")
            message(WARNING "  or: sudo yum install lcov (RHEL/CentOS)")
        endif()

        # 添加覆盖率编译选项
        message(STATUS "Code coverage enabled (gcov/lcov)")

        # 覆盖率标志（供需要手动拼装的调用方使用；一般用下面的函数）
        set(KAIRO_COVERAGE_COMPILE_OPTIONS
            --coverage
            -fprofile-arcs
            -ftest-coverage
        )
        set(KAIRO_COVERAGE_LINK_OPTIONS
            --coverage
        )

        # 提供覆盖率使用说明
        if(LCOV_PATH AND GENHTML_PATH)
            message(STATUS "Coverage tools found: lcov and genhtml")
            message(STATUS "  To generate coverage report, run:")
            message(STATUS "    1. Build with: cmake -B build -DKAIRO_ENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug")
            message(STATUS "    2. Run tests: ctest --test-dir build -L unit -L integration")
            message(STATUS "    3. Generate report: lcov --capture --directory build --output-file build/coverage.info")
            message(STATUS "    4. Filter: lcov --remove build/coverage.info */tests/* */test_* */examples/* */usr/* --output-file build/coverage_filtered.info")
            message(STATUS "    5. HTML: genhtml build/coverage_filtered.info --output-directory build/coverage_html")
        else()
            message(WARNING "lcov/genhtml not found. Install lcov to generate HTML reports.")
        endif()
    else()
        message(WARNING "Code coverage is only supported for GCC/Clang compilers.")
        message(WARNING "  Current compiler: ${CMAKE_CXX_COMPILER_ID}")
    endif()
else()
    # 覆盖率未启用，提供帮助信息
    message(STATUS "Code coverage disabled. Enable with: -DKAIRO_ENABLE_COVERAGE=ON")
endif()

# 函数：为任意目标（库/测试/示例）应用覆盖率选项。
# 必须在目标创建之后调用（src/CMakeLists.txt 中 kairo 目标、
# tests/CMakeLists.txt 中各测试目标）。未启用覆盖率或编译器不支持时为 no-op。
function(kairo_apply_coverage_to_target target_name)
    if(KAIRO_ENABLE_COVERAGE)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${target_name} PRIVATE
                ${KAIRO_COVERAGE_COMPILE_OPTIONS}
            )
            target_link_options(${target_name} PRIVATE
                ${KAIRO_COVERAGE_LINK_OPTIONS}
            )
        endif()
    endif()
endfunction()

# 函数：为测试目标应用覆盖率选项（历史入口，语义同上）
function(kairo_apply_coverage_to_test target_name)
    kairo_apply_coverage_to_target(${target_name})
endfunction()
