# CompilerWarnings.cmake
# 统一编译器警告配置，支持 GCC、Clang、MSVC

# 选项：是否将警告视为错误
option(KAIRO_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

function(kairo_enable_warnings target)
if(MSVC)
    # MSVC 警告配置
    target_compile_options(${target} PRIVATE /W4)
    if(KAIRO_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE /WX)
    endif()
    
    # 禁用一些常见的 MSVC 警告
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # GCC/Clang 警告配置
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    
    # 额外的有用警告
    set(_kairo_extra_warnings
        -Wcast-align
        -Wcast-qual
        -Wconversion
        -Wctor-dtor-privacy
        -Wdisabled-optimization
        -Wformat=2
        -Winit-self
        -Wlogical-op
        -Wmissing-declarations
        -Wmissing-include-dirs
        -Wnoexcept
        -Wold-style-cast
        -Woverloaded-virtual
        -Wredundant-decls
        -Wshadow
        -Wsign-conversion
        -Wstrict-null-sentinel
        # -Wall already enables the actionable level-1 strict-overflow checks.
        # Level 5 diagnoses optimizer transformations inside standard-library
        # templates without pointing to a source expression to correct.
        -Wstrict-overflow=1
        -Wswitch-default
        -Wundef
        -Wunused
        -Wzero-as-null-pointer-constant
    )

    # NDK clang 不认识部分 GCC 风格 warning，会产生大量 unknown warning option。
    if(ANDROID AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        list(REMOVE_ITEM _kairo_extra_warnings
            -Wlogical-op
            -Wnoexcept
            -Wstrict-null-sentinel
        )
    endif()

    target_compile_options(${target} PRIVATE ${_kairo_extra_warnings})
    
    # Clang 特定警告
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${target} PRIVATE
            -Wdocumentation
            -Wno-documentation-unknown-command
        )
    endif()
    
    # 如果启用警告为错误
    if(KAIRO_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endif()
endfunction()
