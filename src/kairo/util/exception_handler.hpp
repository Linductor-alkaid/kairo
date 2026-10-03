#pragma once

#include <string>
#include <exception>
#include <functional>
#include <mutex>

namespace kairo {
namespace util {

/**
 * @brief 异常处理器
 * 
 * 用于处理任务执行过程中的异常，防止异常传播到执行器。
 * 支持设置异常回调函数，用于自定义异常处理逻辑。
 */
class ExceptionHandler {
public:
    /**
     * @brief 构造函数
     */
    ExceptionHandler() = default;

    /**
     * @brief 析构函数
     */
    ~ExceptionHandler() = default;

    // 禁止拷贝和移动
    ExceptionHandler(const ExceptionHandler&) = delete;
    ExceptionHandler& operator=(const ExceptionHandler&) = delete;
    ExceptionHandler(ExceptionHandler&&) = delete;
    ExceptionHandler& operator=(ExceptionHandler&&) = delete;

    /**
     * @brief 处理任务异常
     * 
     * 捕获任务执行过程中的异常，防止异常传播到执行器。
     * 如果设置了异常回调，会调用回调函数。
     * 
     * @param executor_name 执行器名称
     * @param exception 异常指针
     */
    void handle_task_exception(const std::string& executor_name,
                               std::exception_ptr exception);

    /**
     * @brief 处理任务超时
     * 
     * 当任务执行超时时调用此方法。
     * 
     * @param executor_name 执行器名称
     * @param task_id 任务ID
     */
    void handle_task_timeout(const std::string& executor_name,
                            const std::string& task_id);

    /**
     * @brief 设置异常回调函数
     * 
     * 设置自定义异常处理回调。当发生异常时，会调用此回调。
     * 
     * @param callback 回调函数，参数为执行器名称和异常指针
     */
    void set_exception_callback(
        std::function<void(const std::string&, std::exception_ptr)> callback);

    /**
     * @brief 设置超时回调函数
     * 
     * 设置自定义超时处理回调。当任务超时时，会调用此回调。
     * 
     * @param callback 回调函数，参数为执行器名称和任务ID
     */
    void set_timeout_callback(
        std::function<void(const std::string&, const std::string&)> callback);

private:
    std::mutex callback_mutex_;  // 保护回调函数的互斥锁
    std::function<void(const std::string&, std::exception_ptr)> exception_callback_;
    std::function<void(const std::string&, const std::string&)> timeout_callback_;
};

} // namespace util
} // namespace kairo
