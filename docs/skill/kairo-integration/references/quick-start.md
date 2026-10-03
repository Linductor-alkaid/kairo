# Quick Start

## Minimal Runnable Integration

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(executor_app LANGUAGES CXX)
find_package(executor REQUIRED)
add_executable(executor_app main.cpp)
target_link_libraries(executor_app PRIVATE executor::executor)
```

`main.cpp`:

```cpp
#include <executor/executor.hpp>
#include <exception>
#include <iostream>

int main() {
    executor::Executor executor;
    const auto init = executor.initialize_ex({});
    if (!init) {
        std::cerr << init.message << '\n';
        return 1;
    }

    try {
        auto answer = executor.submit_auto([] { return 42; });
        std::cout << answer.get() << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        executor.shutdown(false);
        return 1;
    }

    executor.shutdown();
}
```

Configure with `cmake -S . -B build`, build with `cmake --build build`, and run the resulting executable. This must print `42` before adding another capability.

Create an isolated `Executor` for application-owned lifetime. Use `Executor::instance()` only when deliberate process-wide sharing is wanted.

## First Boundary

`submit_auto()` uses the default asynchronous pool for an ordinary callable. It does not select realtime, lock-free, or GPU paths simply because those are present. Initialize explicitly before the first submission when thread count, queue capacity, timeout, or monitoring configuration matters.

## Next

For result/error or shutdown changes read [tasks and lifecycle](tasks-and-lifecycle.md). Otherwise return to the entry router and load one matching card.
