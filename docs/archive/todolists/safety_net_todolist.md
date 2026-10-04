# 兜底策略实现任务清单

本文档基于 [兜底策略设计：懒初始化与退出时自动关闭](../design/safety_net_design.md)，列出两项兜底策略的实现任务清单。

---

## 阶段 1：策略一 —— 懒初始化（Lazy Initialization）

### 1.1 ExecutorManager 改动

- [x] 在 `include/executor/executor_manager.hpp` 中增加成员
  - [x] 添加“默认初始化用”的 `std::once_flag`（如 `default_init_once_`）
  - [x] 可选：添加“是否已显式初始化”的标记（若需区分显式/懒初始化，当前设计可不区分）

- [x] 在 `src/executor/executor_manager.cpp` 中实现懒初始化
  - [x] 在 `get_default_async_executor()` 中：若 `default_async_executor_` 为空，则 `std::call_once(default_init_once_, [this]{ initialize_async_executor(default_config); })`，再返回 `default_async_executor_.get()`
  - [x] 使用默认 `ExecutorConfig`（与 `config.hpp` 中结构体默认值一致：min_threads=4, max_threads=16, queue_capacity=1000 等）
  - [x] 保证线程安全：多线程同时首次调用 `get_default_async_executor()` 时只初始化一次

### 1.2 Executor Facade 行为

- [x] 确认 `Executor::submit`、`submit_priority`、`submit_delayed`、`submit_periodic` 均通过 `manager_->get_default_async_executor()` 获取执行器
  - [x] 若当前实现中“未初始化则抛异常”的逻辑在 Executor 层，改为：获取指针后若仍为 nullptr（如已 shutdown）再抛异常或按设计文档处理；否则依赖 Manager 懒初始化后返回非空指针，不再因“未 initialize”抛异常
  - [x] 定时器线程内部通过 `get_default_async_executor()` 获取执行器，确认其会自然受益于懒初始化

### 1.3 边界行为

- [x] 懒初始化后用户再调用 `initialize(other_config)` 保持现有语义：`initialize_async_executor` 返回 false（已初始化则不再初始化），无需改代码，仅需文档说明
  - [x] 确认 `ExecutorManager::initialize_async_executor` 在“已存在 default_async_executor_”时返回 false

---

## 阶段 2：策略二 —— 退出时自动关闭（Exit-time Shutdown）

### 2.1 atexit 注册与回调

- [x] 在 `ExecutorManager::instance()` 的 `std::call_once` 初始化块中，在 `new ExecutorManager()` 之后
  - [x] 调用 `std::atexit(&ExecutorManager::atexit_shutdown)`（或注册一静态/自由函数，在其中调用 `ExecutorManager::instance().shutdown(false)`）
  - [x] 仅单例创建时注册一次，实例模式的 `ExecutorManager` 不经过 `instance()`，故不会注册 atexit

- [x] 实现 `ExecutorManager::atexit_shutdown()`（或等价回调）
  - [x] 内部调用 `ExecutorManager::instance().shutdown(false)`（不等待未完成任务）
  - [x] 确保回调不抛异常：在回调内 try-catch 吞掉异常，或确认 `shutdown(false)` 在正常使用下不抛，避免 `std::terminate`

### 2.2 头文件与声明

- [x] 在 `include/executor/executor_manager.hpp` 中声明 `atexit_shutdown` 为静态成员（若采用成员形式），或在一 .cpp 内实现自由函数并仅通过 atexit 注册使用

---

## 阶段 3：文档更新

- [x] 更新 [API.md](../API.md) 的“初始化与关闭”一节
  - [x] 说明：**若不调用 `initialize(config)`，首次提交任务时会使用默认配置自动初始化；**需要自定义线程数、队列容量等时，请在首次提交前显式调用 `initialize(config)`。
  - [x] 说明：**使用单例时，若未显式调用 `shutdown()`，进程退出时会自动关闭所有执行器；**若需在退出前等待未完成任务完成，请在业务逻辑中显式调用 `shutdown(true)`。
  - [x] 可选：在“注意事项”中补充懒初始化后不可再通过 `initialize()` 更换配置、atexit 使用 `shutdown(false)` 不等待任务、避免在静态析构中使用 Executor 等

- [x] 可选：在 [README.md](../../README.md) 的“基本用法”中简要提及“可不显式 initialize/shutdown，库会兜底”，并仍推荐显式调用

---

## 阶段 4：测试

- [x] **懒初始化**：不调用 `initialize()`，直接 `Executor::instance().submit(...)`，断言任务执行成功且不抛异常
- [x] **懒初始化线程安全**：多线程同时首次调用 `submit`，断言只初始化一次、任务均执行成功
- [x] **显式初始化优先**：先 `initialize(custom_config)` 再 `submit`，断言使用的线程数/配置与 custom_config 一致（如通过 `get_async_executor_status()` 或等价接口验证）
- [x] **退出时关闭（单例）**：使用单例、不调用 `shutdown()`，在 atexit 或退出前通过测试钩子/mock 断言 shutdown 被调用；或进程退出后无泄漏（如 valgrind/AddressSanitizer）
- [x] **实例模式不注册 atexit**：仅使用实例模式 `Executor ex; ex.submit(...);`，不触发单例，断言 atexit 未注册或进程行为正常（如 atexit 回调调用次数或单例未创建）

---

## 参考

- 设计文档：[docs/design/safety_net_design.md](../design/safety_net_design.md)
- 默认配置与接口：`include/executor/config.hpp`、`include/executor/executor_manager.hpp`
