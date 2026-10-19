#include "test_gpu_timing_feedback_support.h"

// 预先展开 executor.hpp 传递包含的标准库头（header guard 使 define 期间
// 的二次包含成为 no-op），避免 #define private public 撕裂标准库定义
// （如 sstream 的 __xfer_bufptrs "redeclared with different access"）。
#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>

#define private public
#include <kairo/executor.hpp>
#undef private

namespace gpu_timing_test {

void register_fake_gpu(kairo::Executor& executor, const std::string& name,
                       std::unique_ptr<kairo::IGpuExecutor> fake) {
    // 仅数据成员访问 + ExecutorManager 公开方法：无跨 TU 成员函数符号
    // 引用，MSVC 修饰名口径安全（见头文件注释）。
    executor.manager_->register_gpu_executor(name, std::move(fake));
}

}  // namespace gpu_timing_test
