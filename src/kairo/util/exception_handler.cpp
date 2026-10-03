#include "exception_handler.hpp"
#include <stdexcept>

namespace kairo {
namespace util {

void ExceptionHandler::handle_task_exception(
    const std::string& executor_name,
    std::exception_ptr exception) {
    
    // 获取回调函数（需要加锁保护）
    std::function<void(const std::string&, std::exception_ptr)> callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback = exception_callback_;
    }

    // 如果设置了回调，调用回调函数
    if (callback) {
        try {
            callback(executor_name, exception);
        } catch (...) {
            // 回调函数本身抛出的异常被忽略，防止异常传播
        }
    }
    // 如果没有设置回调，异常被静默处理（不抛出）
}

void ExceptionHandler::handle_task_timeout(
    const std::string& executor_name,
    const std::string& task_id) {
    
    // 获取回调函数（需要加锁保护）
    std::function<void(const std::string&, const std::string&)> callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback = timeout_callback_;
    }

    // 如果设置了回调，调用回调函数
    if (callback) {
        try {
            callback(executor_name, task_id);
        } catch (...) {
            // 回调函数本身抛出的异常被忽略，防止异常传播
        }
    }
    // 如果没有设置回调，超时被静默处理
}

void ExceptionHandler::set_exception_callback(
    std::function<void(const std::string&, std::exception_ptr)> callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    exception_callback_ = std::move(callback);
}

void ExceptionHandler::set_timeout_callback(
    std::function<void(const std::string&, const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    timeout_callback_ = std::move(callback);
}

} // namespace util
} // namespace kairo
