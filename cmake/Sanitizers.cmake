# Sanitizers.cmake
# 提供 AddressSanitizer (ASAN)、UndefinedBehaviorSanitizer (UBSAN)、
# ThreadSanitizer (TSAN) 支持
#
# CR-083 修复后的加载契约（根 CMakeLists.txt 无条件 include 本文件）：
#   * TSAN 独立于 KAIRO_ENABLE_SANITIZERS 总开关——CI 的 TSAN job 只传
#     -DKAIRO_ENABLE_TSAN=ON（可以是 Release 构建），必须保持可用。
#   * ASAN/UBSAN 由 KAIRO_ENABLE_SANITIZERS 总开关控制（默认 OFF）。
#   * ASAN 与 TSAN 运行时互斥（两套插桩运行时不能共存）：两者同时解析为
#     ON 时配置期直接 FATAL_ERROR，而不是产出链接期才爆的废构建。
#   * 建议 Debug 构建使用；非 Debug 下激活任一 sanitizer 只告警不阻断
#     （CI 的 Release+TSAN job 是合法用例）。

# TSAN 先于 ASAN 解析：显式开 TSAN 时 ASAN 默认让位为 OFF。
option(KAIRO_ENABLE_TSAN "Enable ThreadSanitizer (-fsanitize=thread)" OFF)

if(KAIRO_ENABLE_TSAN)
    # 仅当用户显式传了 -DKAIRO_ENABLE_ASAN=ON（缓存已存在且为真）时才视为
    # 冲突；否则把 ASAN 默认值改为 OFF，避免"总开关 + TSAN"意外叠加。
    # 注意 if(CACHE{VAR}) 不是缓存读取，须用 $CACHE{VAR} 展开。
    set(KAIRO_ASAN_EXPLICIT_VALUE "$CACHE{KAIRO_ENABLE_ASAN}")
    if(KAIRO_ASAN_EXPLICIT_VALUE)
        message(FATAL_ERROR
            "KAIRO_ENABLE_ASAN=ON conflicts with KAIRO_ENABLE_TSAN=ON: "
            "AddressSanitizer and ThreadSanitizer runtimes are mutually "
            "exclusive. Drop one of the two options.")
    endif()
    option(KAIRO_ENABLE_ASAN "Enable AddressSanitizer" OFF)
else()
    option(KAIRO_ENABLE_ASAN "Enable AddressSanitizer" ON)
endif()

option(KAIRO_ENABLE_SANITIZERS "Enable sanitizers (ASAN, UBSAN, etc.)" OFF)

# 解析最终生效的 ASAN/TSAN 组合，统一互斥裁决
set(KAIRO_ASAN_ACTIVE OFF)
set(KAIRO_TSAN_ACTIVE OFF)
if(KAIRO_ENABLE_TSAN)
    set(KAIRO_TSAN_ACTIVE ON)
endif()
if(KAIRO_ENABLE_SANITIZERS AND KAIRO_ENABLE_ASAN)
    set(KAIRO_ASAN_ACTIVE ON)
endif()

if(KAIRO_ASAN_ACTIVE AND KAIRO_TSAN_ACTIVE)
    message(FATAL_ERROR
        "KAIRO_ENABLE_SANITIZERS=ON (with ASAN) cannot be combined with "
        "KAIRO_ENABLE_TSAN=ON: AddressSanitizer and ThreadSanitizer "
        "runtimes are mutually exclusive. "
        "Pass -DKAIRO_ENABLE_ASAN=OFF alongside TSAN.")
endif()

if(KAIRO_ASAN_ACTIVE OR KAIRO_TSAN_ACTIVE)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT CMAKE_BUILD_TYPE STREQUAL "")
        message(WARNING
            "Sanitizers are typically used with Debug builds. "
            "Current build type: ${CMAKE_BUILD_TYPE} "
            "(Release + TSAN is an explicit, supported case.)")
    endif()
endif()

if(KAIRO_ASAN_ACTIVE OR KAIRO_TSAN_ACTIVE OR KAIRO_ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set(SANITIZER_FLAGS "")

        if(KAIRO_ASAN_ACTIVE)
            list(APPEND SANITIZER_FLAGS "-fsanitize=address")
            # 检测内存泄漏时保留栈帧信息
            list(APPEND SANITIZER_FLAGS "-fno-omit-frame-pointer")
        endif()

        if(KAIRO_TSAN_ACTIVE)
            list(APPEND SANITIZER_FLAGS "-fsanitize=thread")
            # 保留栈帧，TSAN 报告符号化需要（与原根 CMakeLists TSAN 块一致）
            list(APPEND SANITIZER_FLAGS "-fno-omit-frame-pointer")
        endif()

        # UBSAN 跟随 ASAN 归总开关管（TSAN 单开时不叠加 UBSAN，保持 CI 语义）
        if(KAIRO_ENABLE_SANITIZERS)
            option(KAIRO_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" ON)
            if(KAIRO_ENABLE_UBSAN)
                list(APPEND SANITIZER_FLAGS "-fsanitize=undefined")
                # 在第一次错误时停止
                list(APPEND SANITIZER_FLAGS "-fno-sanitize-recover=all")
            endif()
        endif()

        # 应用 sanitizer 标志
        if(SANITIZER_FLAGS)
            string(REPLACE ";" " " SANITIZER_FLAGS_STR "${SANITIZER_FLAGS}")
            add_compile_options(${SANITIZER_FLAGS})
            add_link_options(${SANITIZER_FLAGS})

            message(STATUS "Sanitizers enabled: ${SANITIZER_FLAGS_STR}")
        endif()

    elseif(MSVC)
        # MSVC 使用不同的 sanitizer 支持
        message(WARNING "Sanitizers are not fully supported on MSVC. Consider using Clang on Windows.")
    else()
        message(WARNING "Sanitizers are not supported for this compiler.")
    endif()
endif()
