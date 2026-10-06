# Code Review 结果 — 2026-09-30

> 历史设计快照（2026-09-30，master @ 1216844 / v0.5.2）：一次性审查记录，
> 文中旧命名（executor:: / EXECUTOR_* 等）对应现行的 kairo:: / KAIRO_*。

范围：master @ 1216844（v0.5.2）全部已实现代码，不涉及新 feature 设计。
方法：7 个模块并行深度审查（facade / 线程池 / 无锁核心 / comm / 定时器与实时 / GPU / 监控与构建），高危结论经主循环逐一到源码核实。
状态标记：`待验证` → 测试复现中；`已确认`（附复现测试）；`代码推演确认`（逻辑必然、无稳定运行时复现手段）；`未复现`；`平台受限`（本机无对应 GPU/OS 无法运行时验证）。

验证测试位于 `review_verification/`（独立编译，不进主 CTest，避免已知缺陷污染 CI）；确认后的回归测试随修复迁入 `tests/`。

---

## P0 — 崩溃 / 数据竞争 / 永久挂死

### CR-001 单例退出顺序 UAF `已确认（复现）`
- 位置：`src/executor/executor.cpp:185-198`（`~Executor`）、`src/executor/executor.cpp:149-152`（`instance()`）
- `~Executor` 仅在 `owned_manager_` 非空（实例模式）时 `shutdown(true)` 排空；单例模式（`owned_manager_ == nullptr`）析构不排空、不 join。退出时函数级静态按构造逆序：`~Executor` 先析构（`failure_mutex_`/`task_graph_mutex_`/`timers_`/`cancellation_registry_` 全部销毁），之后 `ExecutorManager` 清理才开始——默认池在途任务的闭包捕获裸 `this`（`task_wrapper`、`TicketGuard`、periodic tick builder），会触达已析构成员。exit 时存在在途任务即 UAF。
- 修复方向：单例路径析构同样执行 `shutdown(true)`（排空后再拆成员）。

### CR-002 CUDA start 半途失败 → stop() 永久挂死 `平台受限（代码推演确认）`
- 位置：`src/executor/gpu/cuda_executor.cpp:328, 357-370, 393-401, 524-538`
- `start()` 在建流/建线程**之前** CAS 置 `is_running_=true`；窗口内 `submit_kernel_impl`（:1529 只查 `is_running_`）可压入任务。随后 `create_one_stream` 失败或 `std::thread` 构造抛异常 → `start()` 返回 false 但队列任务无人消费。之后任何 `stop()` → `wait_for_completion` 谓词 `task_queue_.empty() && active_kernels_==0` 永假，`cv.wait` 无退出条件 → 永久挂死。
- 本机无 NVIDIA GPU，`loader_->is_available()` 为 false，运行时不可达；逻辑经代码推演必然成立。
- 修复方向：每个失败路径返回前排空队列并对 promise `set_exception`；或谓词加"未运行且无 worker"退出条件。

### CR-003 OpenCL restart 泄漏 context、命令队列无界增长 `平台受限（代码核对确认：stop() 从不调用 cleanup，重启路径绕过全部释放逻辑）`
- 位置：`src/executor/gpu/opencl_executor.cpp:303`（`context_ = clCreateContext(...)` 直接覆盖）、`:310-319`（`queues_.push_back` 不清理）
- `stop()` 不做 cleanup；再次 `start()` 重跑 `initialize_opencl()`：每轮 start/stop 泄漏一个 cl_context；`queues_` 随 restart 次数无界增长，stream_id 语义漂移（对比 CUDA 侧 `streams_.empty()` 才重建，无此问题）。

### CR-004 before_publish_hook 字段数据竞争（UB） `已确认（TSAN 复现 + call-through-null 崩溃）`
- 位置：`src/executor/lockfree_task_executor.cpp:326-340`；契约承诺 `include/executor/lockfree_task_executor.hpp:198-205`
- `set_before_publish_hook` 对 `user_before_publish_hook_/context_` 为普通写，trampoline 在生产者线程读它们。首次安装（null→非 null）经 shared_ptr 原子发布安全；**后续切换/卸载**期间正在执行旧 trampoline 的生产者与写线程并发读写同一字段，违反头文件"每个生产者看到完整新旧 pair"的契约，形式上 UB。
- 验证手段：TSAN 构建（`build-tsan/`）下并发切换 hook + push 压力。
- 修复方向：合并为单一 `std::atomic<HookState*>` 快照。

### CR-005 `park_worker` 丢失唤醒——注释中的"封闭证明"不成立 `代码推演确认（x86 未复现：52 万投递零丢失唤醒签名）`
- 位置：`src/executor/lockfree_task_executor.cpp:526-544`（问题代码）、`:544-551`（生产者 `fetch_add(2)`）、`:514-523`（被推翻的证明）
- 驻停位用 `load` + 普通 `store` 发布：交错序列下 worker 的 `store(V|kParkedBit)` 会**覆盖掉**生产者 `fetch_add(2)` 的增量，且终扫对刚发布任务无 happens-before 保证（ARM 弱序下可驻停错过任务，直到下一次 push 恢复）。x86 TSO 窗口极窄。
- 修复方向：第 (2) 步改 `fetch_or(kParkedBit, acq_rel)`（RMW 同修改序串行化，不可能互相覆盖）。

---

## P1 — 边界条件 bug

### A. 核心 facade

#### CR-010 `submit_on_with_handle` 异常路径泄漏 registry 槽位 + 永久 Pending 图节点 `已确认（复现）`
- 位置：`include/executor/executor.hpp:1329-1335`（catch 只 `abandon`）
- `register_state`（:1256）与 `allocate_task_handle`（:1203）之后的闭包构造（bind 抛异常）直接穿出：cancellation registry active 条目永久占用（容量 65536，耗尽后可取消提交全被拒）、图节点永久 Pending（`fail_all_parked_tasks_for_shutdown` 只清 parked 节点）。对照同文件 :1532-1538 `try_submit_task` 有完整清理，证明是遗漏。

#### CR-011 `submit_tracked_with_hook` 异常路径同样漏清理，且任务图内存无界增长 `已确认（复现）`
- 位置：`include/executor/executor.hpp:1974-1988`
- 参数打包构造（`make_tuple` 拷贝抛异常）发生在注册/依赖登记之后：registry 泄漏、节点永久 Pending、上游 `task_graph_dependents_` 反向边永不摘除 → `trim_task_graph_retention_locked` 要求 dependents 为空才淘汰，终态节点无限累积。

#### CR-012 `submit_on*` 捕获 `SerialExecutionContext&` 裸引用，生命周期无契约 → UAF `已确认（复现，永久挂死形态）`
- 位置：`include/executor/executor.hpp:1217, 1430-1473, 1476-1521`
- `publish_task` 在池 worker 上调 `context.post_reserved(...)`；`SerialExecutionContext` 是调用方对象，facade 不持有。fire-and-forget 下 context 在任务执行前析构 → worker 触达悬垂 mutex。
- 验证手段：ASAN 下 submit_on 后立刻销毁 context。
- 修复方向：文档化硬契约或改 `shared_ptr` 持有。

#### CR-013 读路径懒创建默认线程池 + `call_once` 失败后无限重试 `已确认（复现）`
- 位置：`src/executor/executor_manager.cpp:167-185`
- `get_default_async_executor_snapshot()` 在未初始化时建池：`get_async_executor_status()`（const 诊断查询）会拉起 hw 条常驻线程；`shutdown()` 先建池再关池。与 `get_snapshot` 文档"查询不会创建默认异步执行器"矛盾。懒初始化 `start()` 抛异常时 `call_once` 未完成，之后每次查询/提交都重试整套建池。

#### CR-014 `~ExecutorManager` 内 `shutdown(true)` 不隔离后端 stop 异常 → 析构抛异常 → terminate `代码推演确认`
- 位置：`src/executor/executor_manager.cpp:54-56, 765-782`
- 对比同文件 :849-855 retired 池路径有 catch（注释"终局排空不外泄异常"），析构路径裸调。`~ThreadPool`（thread_pool.cpp:20-22）同样。任何后端 stop 抛出即 `std::terminate`。

### B. 线程池

#### CR-020 monitor 回调抛异常 → future 永不就绪（`get()` 永久挂起） `已确认（复现）`
- 位置：`src/executor/thread_pool/thread_pool.cpp:335-341`（`record_task_start` 在 `task.function()` 之前）、`:400-419`（catch 只保 worker 存活与统计）
- `record_task_start` 是用户可覆写的虚函数（task_monitor.hpp:42 注释自证"user-supplied"）。其抛出时 submit 包装层的 `promise->set_exception`（thread_pool.hpp:581-588）永远不会执行，`on_timeout` 仅软超时分支触发 → future 永挂。

#### CR-021 `resize_monitor_thread` 无异常屏障 → std::terminate `代码推演确认`
- 位置：`src/executor/thread_pool/thread_pool.cpp:885-918`
- `check_and_resize()` 调用链上 `resize_local_queues` 的 `make_unique`/`emplace_back`、`create_worker_thread` 的 `std::thread` 构造（资源耗尽抛 `system_error`）可抛；异常穿出线程函数即进程终止。

#### CR-022 初始化失败回滚后提交被接受且永不执行；`config_` 数据竞争 `已确认（复现，两条路径）`
- 位置：`src/executor/thread_pool/thread_pool.cpp:156-157`（`rollback_initialization_failure` 复位 `stop_=false`）、`:963`（`try_submit` 不持锁读 `config_.task_timeout_ms`）vs `:36`（initialize 锁内写 `config_`）
- 回滚后 dispatcher 为 null、无 worker：提交进 scheduler、`total_tasks_++` 但永不执行，`wait_for_completion` 挂到 300s 超时。并发 initialize 与提交构成 UB 数据竞争。

#### CR-023 worker 主循环对 dispatch 无异常屏障；lockfree push 拷贝抛异常泄漏 Task `代码推演确认`
- 位置：`src/executor/thread_pool/thread_pool.cpp:316-318`；`src/executor/thread_pool/lockfree_worker_queue.hpp:35-44`
- `dispatch_batch` 回灌时 `scheduler_.enqueue` 的 `make_unique<Task>` 可抛 bad_alloc；lockfree `push` 的 `new Task`/`copy_task`（拷贝 `std::function`）抛出时 `task_ptr` 泄漏。异常穿出 worker 线程即 terminate。

#### CR-024 严格优先级无老化，LOW/NORMAL 无限饿死（P2 级设计确认项） `已确认（复现）`
- 位置：`src/executor/thread_pool/priority_scheduler.cpp:129-186`
- 持续高优先级负载下低优先级零进度；若为有意设计应文档化。

#### CR-025 批内任务共享同一 `submit_time_ns`，同优先级批内 FIFO 失效 `已确认（复现，程度超预期：200/200 轮全乱序）`
- 位置：`src/executor/thread_pool/thread_pool.cpp:1111-1123`；契约注释 `priority_scheduler.hpp:18`
- 比较器对批内任务恒等价，堆序任意。

### C. 无锁核心 / 工具

#### CR-030 Linux `set_thread_priority` 把 nice 设到调用线程而非目标线程 `已确认（复现）`
- 位置：`src/executor/util/thread_utils.cpp:234-242`
- `setpriority(PRIO_PROCESS, 0, priority)` 的 `who=0` 按内核语义作用于**调用线程**（Linux nice 是 per-task 的），`handle` 完全被忽略。注释"Linux 无 per-thread nice"与事实相反。主线程调 `set_thread_priority(worker_handle, 10)` 会改主线程 nice 并返回 true，目标 worker 毫无变化。

#### CR-031 `ObjectPool` acquire 不构造 / release 不析构：上一个任务的 callable 在池内滞留 `已确认（复现，滞留与析构转移两项均成立）`
- 位置：`src/util/object_pool.hpp:103-131, 159-209, 87`
- worker 执行完 `release_bulk` 归还时不清 `func`，持有捕获状态（文件句柄、shared_ptr、大缓冲）的旧 callable 存活到下次被生产者线程 `acquire` 后赋值时才析构——析构成本与副作用转移到随机生产者线程；长期不复用则资源滞留到池析构。

#### CR-032 `push()` 首个 CAS 失败导致位置泄漏、队列永久卡死（当前不可达，防御性） `代码推演确认`
- 位置：`src/executor/util/lockfree_queue.hpp:127-131`
- `enqueue_pos_` 已推进后 `begin_write` 失败直接 return false，不清理。正常不可达，但一旦发生 frontier 恒为 Free，`pop_impl:669` break，队列永久无进展。

#### CR-033 `cancel_reservation` 自旋不检查 `Cancelled` 与前沿推进，多消费者叠加可空转秒级 `代码推演确认（未能稳定构造交错；无功能破坏，有界尾延迟）`
- 位置：`src/executor/util/lockfree_queue.hpp:526-548`
- 退出条件只有 `Published/Writing/BatchWriting`；另一消费者已取消时本消费者仍空转完整 `64 * claimed_work` 次 `yield()`。

#### CR-034 `register_state` 对重复 task_id 静默覆盖（需确认契约） `已确认（行为复现；facade 正常路径 id 唯一，仅外部构造重复 id 可触发）`
- 位置：`include/executor/task_cancellation.hpp:324`
- `active_[task_id] = std::move(state)` 覆盖旧表项，按 id 取消只作用于新任务。

#### CR-035 Android fallback `JThread::operator=` 标 noexcept，内部 `join()` 可抛 → terminate `代码推演确认`
- 位置：`include/executor/stop_token.hpp:121-131`

#### CR-036 Android `set_cpu_affinity` 忽略 handle 作用于调用线程却返回 true；Windows 线程名朴素宽化非 ASCII 乱码 `代码推演确认`
- 位置：`src/executor/util/thread_utils.cpp:263-268, 187`

### D. comm 通信原语

#### CR-040 KeepLatest/DropOldest 在竞争下退化为"丢最新"，与策略契约相反 `已确认（复现）`
- 位置：`include/executor/comm/bounded_queue.hpp:83-102`（enqueue 竞争路径）、`:89-92`、`:326-351`（promotion 要求 active==1）
- KeepLatest 需先 `try_promote_to_replacement()`（要求队列仅剩本生产者）再 `try_acquire_consumer()`（要求消费者空闲），任一失败即丢弃**新消息**。docs/API.md:1514 承诺"KeepLatest 保留最新值"，教程主场景（Topic + 慢消费者 + KeepLatest）恰好踩中。事件只报 `Dropped`，无法与 RejectNewest 区分。

#### CR-041 `try_pop` 移动构造抛异常仍计入 received 与延迟直方图；KeepLatest 异常路径丢失 displaced 统计 `待验证`
- 位置：`include/executor/comm/bounded_queue.hpp:141-149, 109-115, 121-127`

#### CR-042 `drain_for_cycle` 的 `max_items_per_cycle == 0` 语义为"无限制"，与 RT 预算目的相悖且未文档化 `已确认（复现）`
- 位置：`include/executor/comm/mailbox.hpp:263-267`
- 参数侧 0=默认值（API.md:1599），options 侧 0=无限，同一字面量两层含义相反。

#### CR-043 `Topic::publish` 异常导致部分扇出且无部分完成信息 `代码推演确认`
- 位置：`include/executor/comm/topic.hpp:173-188, 217-230`

#### CR-044 realtime_memory 全局 `operator new` 替换未覆盖 aligned/nothrow 变体，guard 计数可被绕过 `已确认（复现；另发现默认构建 guard 整体关闭）`
- 位置：`src/executor/comm/realtime_memory.cpp:77-111`
- 本仓库大量 `alignas(64)` 成员经对齐变体分配，绕过 guard 计数；替换在库 .cpp 中，链接顺序影响生效范围。

### E. 定时器 / 实时线程 / 阻塞 IO / 任务依赖

#### CR-050 定时器线程无异常屏障：tick_builder/dispatch 抛出即 std::terminate `代码推演确认`
- 位置：`include/executor/timer.hpp:701-705, 718, 728`
- 三处外呼无 try/catch；facade 每 tick 做堆分配（executor.cpp:969-970 等），OOM 或闭包异常 = 进程崩溃而非丢一个 tick。tick_builder 还在**持有 `mutex_` 时**调用。

#### CR-051 周期定时器无漂移补偿：下一到期锚定"醒来时刻"而非"应到期时刻" `审查结论修正：实测无漂移（见验证汇总）`
- 位置：`include/executor/timer.hpp:669-676`
- fixed-delay 而非 fixed-rate：回调耗时逐周期单调累积（100ms 周期 + 每 tick 5ms 耗时 → 实际 ~105ms 且持续漂移）。RT 侧已有 skip-late 决策（realtime_thread_executor.cpp:475-487），定时器侧无对应处理。

#### CR-052 依赖环检测：持写锁的递归 DFS，深度无界 → 栈溢出 + 读饿死 + O(n²) 建链 `已确认（复现：O(n²)、锁内 DFS、小栈 n=4000 SIGSEGV）`
- 位置：`src/executor/task/task_dependency_manager.cpp:32`（unique_lock 内调 `has_cycle`）、`:169-197`（递归 `dfs_path_exists`）
- 十万级依赖链直接栈溢出崩溃；DFS 期间所有共享读阻塞。
- 验证手段：构建 10 万级链，期望崩溃或超时。

#### CR-053 RT 队列容量取整与对象池容量不一致，drop 归因失真 `已确认（复现）`
- 位置：`src/executor/realtime_thread_executor.cpp:50-51, 631`；`src/executor/util/lockfree_queue.hpp:82-84`（队列向上取整 2 的幂）
- 非 2 幂容量（1000→1024）时池先耗尽，`queue_full_count_` 恒 0；`status.queue_capacity` 上报取整值高估深度。

#### CR-054 RT 内置循环 stop 延迟可达一个整周期；自停止后仍继续执行用户回调 `已确认（复现：stop 延迟≈周期 80%）`
- 位置：`src/executor/realtime_thread_executor.cpp:441-491`（只置 `running_=false`，无 cv 打断）、`:494-508`（回调不受 `running_` 门控）

#### CR-055 BlockingIO：持 `lifecycle_mutex_` 调用户 `wakeup()`（重入即自死锁）；并发 stop 无终结栅栏，第二个 stop 提前返回 `待验证`
- 位置：`src/executor/blocking_io_executor.cpp:95-101, 108-118`

#### CR-056 `worker_id_` join 后不复位，线程 id 复用可致误判自停止（理论性） `代码推演确认`
- 位置：`src/executor/realtime_thread_executor.cpp:88`、`blocking_io_executor.cpp:151`

#### CR-057 SerialExecutionContext：reservation 永不 publish/abandon 将永久卡死整个上下文；worker 线程内自析构 → terminate/UAF `代码推演确认`
- 位置：`include/executor/serial_execution_context.hpp:31-37, 133-147, 21, 98-99`
- 水位被缺口阻塞后所有 post 永不执行；代码层无强制"要么 publish 要么 abandon"。

### F. GPU

#### CR-060 `GpuMemoryManager` 用户指针只有 8 字节对齐，头文件宣称 256 `已确认（复现：直通与池路径均仅 8 对齐）`
- 位置：`src/executor/gpu/gpu_memory_manager.cpp:48, 61, 85, 96, 103`；契约 `gpu_memory_manager.hpp:100`
- 用户指针 = 256 对齐块 + `kHeaderSize(8)`，且逐次分配使余块起点漂移。float4/uint4 等 16B 对齐访存触发 CUDA misaligned address。`raw_alloc_` 是回调注入，可纯 CPU 复现。
- 修复方向：`kHeaderSize = align_up(sizeof(size_t), kAlignment)`。

#### CR-061 `TaskSchedulerOptimizer::select_best_device` 初值漏加任务自身开销，会选错设备 `已确认（复现；显形依赖 unordered_map 迭代序）`
- 位置：`src/executor/gpu/task_scheduler_optimizer.cpp:155-163`
- 初值用 `estimated_total_cost`，循环内比较用 `+ task.estimated_cost`，公式不一致。

#### CR-062 `completed_tasks_` 无界增长（慢性泄漏）；`remove_task` 使被移除任务的下游依赖被视为已满足 `已确认（复现：两部分均成立，100 万次 +72MB）`
- 位置：`src/executor/gpu/task_scheduler_optimizer.cpp:37`（全文件无 erase）、`:59`、`:29-32`
- 失败任务被移除后，下游带着缺失输入被判定 ready 并执行。

#### CR-063 两个 optimizer 的 `config_` 无锁数据竞争（UB） `已确认（TSAN 3 轮 10+ data race）`
- 位置：`src/executor/gpu/transfer_optimizer.cpp:14, 187-190`（读无锁）vs `:243-246`（写持 `batch_mutex_`）；`kernel_launch_optimizer.cpp:15,41,100,110-115,159-161` vs `:202-205`（写持 `cache_mutex_`）
- 读写不共享任何锁，非原子成员并发读写。
- 验证手段：TSAN。

#### CR-064 OpenCL stop() 不等在飞 GPU 工作即释放队列，async 拷贝可写入已释放 host 缓冲 `平台受限`
- 位置：`src/executor/gpu/opencl_executor.cpp:152-239, 363-404`
- CUDA 侧有 `cudaDeviceSynchronize`，OpenCL 侧 join worker 后直接 cleanup。修复方向：cleanup 前对每个队列 `clFinish`。

#### CR-065 Windows CUDA 版本化 DLL 命名错误，CUDA 9/10/11 无法被发现 `平台受限（代码推演确认）`
- 位置：`src/executor/gpu/cuda_loader.cpp:135-137, 184-189, 335-336`
- 拼接 `cudart64_11.dll` 等；实际为 `cudart64_110.dll`~`118`、`cudart64_100/101/102`、`cudart64_90/91/92`。

#### CR-066 OpenCL 设备枚举编号与执行器 device_id 语义不一致（错绑/失败） `平台受限`
- 位置：`src/executor/gpu/device_query.cpp:81-96`（跨平台连续编号）vs `opencl_executor.cpp:274`（只用 `platforms[0]`）

### G. 监控

#### CR-070 快照格式化器缺换行，破坏 line-oriented 契约 `已确认（复现）`
- 位置：`src/executor/monitor/executor_snapshot_formatter.cpp:215-217`
- `timer_slack_applied` 后缺 `'\n'`，输出 `...applied=truerealtime[rt].dropped_...`，按行解析的工具必挂。同文件其余所有 `write_bool` 均正常。

#### CR-071 TaskMonitor 单把全局 mutex 在提交/执行热路径串行化所有线程；采样率 0 时锁照样取 `待验证`
- 位置：`src/executor/monitor/task_monitor.cpp:18, 38, 52, 64, 83, 100, 132`；调用方 `thread_pool.cpp:337, 385, 397, 994, 1061, 1149`
- 默认开启 100% 采样下每提交+执行共 4 次全局锁 + string 拷贝进 map。

#### CR-072 TaskMonitor 析构竞态的注释论证只覆盖一半 + Manager 成员析构顺序不利（排空失败路径 UAF） `代码推演确认`
- 位置：`src/executor/monitor/task_monitor.cpp:68-76`、`include/executor/executor_manager.hpp:316, 324, 366`、`src/executor/executor_manager.cpp:849-856`
- `statistics_collector_`（声明最后 → 最先析构）先于各 executor 容器销毁；shutdown 排空失败时 detached worker 仍持 `TaskMonitor*` 裸指针调用 → UAF。
- 修复方向：声明顺序前移（逆序析构下最后销毁）。

#### CR-073 `TaskMonitor` 有虚函数无虚析构（测试已在派生） `已确认（编译器警告）`
- 位置：`src/executor/monitor/task_monitor.hpp:30, 42, 67`；派生 `tests/test_thread_pool_monitor_concurrent_set.cpp:26`
- 经基类指针 delete 派生对象即 UB。

#### CR-074 `set_sampling_rate` 负数/NaN → double→uint32 转换 UB `已确认（负数→100% 采样；NaN→0%）`
- 位置：`src/executor/monitor/task_monitor.cpp:272, 294`
- `static_cast<uint32_t>(rate * 100.0)` 在负数时 UB，之后才 `min(100)`。实测负值可能变成 100% 采样。

#### CR-075 in-flight 缩容驱逐计入 dropped，`incomplete` 标志永久置位 `已确认（复现）`
- 位置：`src/executor/monitor/task_monitor.cpp:263, 199`

### H. 构建 / 打包 / CMake

#### CR-080 Coverage.cmake 时序错误：executor 库永远拿不到覆盖率插桩 `已确认（构建实验）`
- 位置：`cmake/Coverage.cmake:30-39` + `CMakeLists.txt:74`（include）与 `:170`（add_subdirectory(src)）
- `if(TARGET executor)` 在 include 时恒假，库的 `--coverage` 从未生效；`run_coverage.sh` 报告静默失真（只有测试 TU 有覆盖数据）。
- 验证手段：配置 COVERAGE=ON 后检查 compile flags。

#### CR-081 package_windows.ps1 项目根多跳一级，包内文档静默缺失 `平台受限（代码核实）`
- 位置：`scripts/package_windows.ps1:28-29`（连跳两级）、`:103-108`
- 对照 package_linux.sh:58 正确。`build_windows.ps1:44-45` 同样写法但变量未使用（死代码）。

#### CR-082 build_windows.ps1 的 `-BuildTests/-BuildExamples` 开关被硬编码 OFF 无视 `平台受限（代码核实）`
- 位置：`scripts/build_windows.ps1:60-66, 108-114`；`build_and_package_windows.ps1:37-38` 如实传参但被丢弃。

#### CR-083 Sanitizer 配置：非 Debug 静默失效、ASAN+TSAN 可同时启用（互斥）、TSAN 标志重复追加 `已确认（(a)(b)；(c) TSAN 重复标志未复现——CMake 自动去重）`
- 位置：`CMakeLists.txt:57-71`（仅 Debug include）、`cmake/Sanitizers.cmake:8-49`

#### CR-084 install 规则遗漏 `src/executor/util/exception_handler.hpp`，下游包含 cuda_executor.hpp 编译失败 `已确认（复现：下游编译失败）`
- 位置：`src/CMakeLists.txt:220-224`；`cuda_executor.hpp:8` 包含 `../util/exception_handler.hpp`

#### CR-085 版本号 0.5.2 硬编码 7 处（CMakeLists + 6 个打包脚本），双份事实来源；config.hpp 无版本宏 `代码核实`
- 位置：`CMakeLists.txt:2`、`scripts/package_linux.sh:8`、`package_deb.sh:8`、`build_and_package_linux.sh:8`、`build_and_package_deb.sh:8`、`package_windows.ps1:5`、`build_and_package_windows.ps1:5`

#### CR-086 MSVC 共享库守卫未覆盖 clang/mingw-on-Windows 变体 `平台受限（代码核实）`
- 位置：`src/CMakeLists.txt:42-51`

---

## P2 — 改进建议（择要）

| ID | 位置 | 问题 | 状态 |
|---|---|---|---|
| CR-101 | executor.cpp:284-291 | worker 发起 shutdown 后 `lifecycle_state_` 停留 Draining 直到下次外部 shutdown | 待验证 |
| CR-102 | executor.cpp:713-737 | 终态保留 trim O(capacity) 线性扫描持全局图锁，tracked 提交热路径串行点 | 已确认（每终态节点 7-10µs，默认容量占 tracked 往返 30-50%） |
| CR-103 | interfaces.hpp:906-914 | `start_waiter_generation` 对可能 joinable 的线程直接赋值 → terminate | 代码推演确认 |
| CR-104 | executor.cpp:608-611 | WhenAll 依赖失败异常分类不一致（DependencyCancelled vs 透传） | 实测未复现：所有用户可见路径分类一致，降级为代码级观察 |
| CR-105 | executor.cpp:1406-1435 | `WorkerHandle` 持裸 `ExecutorManager*`，无失效机制 | 代码推演确认 |
| CR-106 | executor.hpp:1670 + executor_manager.cpp:539-632 | 每次 `submit_auto` 路由全量锁 5 把注册表采所有后端能力 | 已确认（submit_auto 慢 65-70%，主要在 route_task+决策记录而非能力采集） |
| CR-107 | executor.hpp 各提交路径 | 每任务 6-10 次堆分配（promise/state/bound/DrainBag×2），部分可省 | 代码核实 |
| CR-108 | thread_pool.cpp:422-434 | 每任务 2 次 `completion_cv_.notify_all()` | 已测量（有等待者时吞吐 -76.8%，归因含锁竞争上界） |
| CR-109 | thread_pool.cpp:712-745 | `try_wait_for_completion` 10ms 轮询（300s=3 万次全量扫队列） | 代码核实 |
| CR-110 | thread_pool.hpp:553-570 | submit 热路径 make_shared×3 + std::bind + task_id 字符串拼接 | 代码核实 |
| CR-111 | lockfree_worker_queue.hpp:35-55 | lockfree 模式每任务 `new Task` | 代码核实 |
| CR-112 | task_dispatcher.hpp:114-237 | 每 dispatch 一次堆分配的锁包装（应 `optional<shared_lock>`）；:233 注释与代码不符 | 代码核实 |
| CR-113 | thread_pool.cpp:637-649 | resize 中途失败留下队列数≠worker数的不一致状态 | 代码推演确认 |
| CR-114 | thread_pool.cpp:683-687 | worker 任务内调 resize 缩容可能 join 自己 → system_error、状态半残 | 代码推演确认 |
| CR-115 | thread_pool.cpp:534-560 | shutdown 的 join 无超时（卡死任务 → ~ThreadPool 永挂），超时参数给人有界错觉 | 代码核实 |
| CR-116 | lockfree_worker_queue.hpp:133-188 vs worker_local_queue.hpp:39 | 两种队列丢弃语义不一致（on_timeout vs broken_promise） | 代码核实 |
| CR-117 | thread_pool_resizer.cpp:67-97 | 扩缩容阈值语义混用：总量 vs 单队列 capacity；capacity=0 恒扩不缩 | 待验证 |
| CR-118 | thread_pool_resizer.cpp:164-167 等 | 死代码：mark_thread_for_exit/dispatch()/dispatch_task（返回值语义错）/WorkerQueueImpl::clear | 代码核实 |
| CR-119 | lockfree_queue.hpp:522-562 | 消费端 cancel_reservation yield 自旋上界 64×1024 无注释 | 代码核实 |
| CR-120 | lockfree_queue.hpp:416-418 | `position << 3` 在 2^61 溢出标签别名（不可达，需注释） | 代码核实 |
| CR-121 | lockfree_queue.hpp:446-447 | begin_write 每 push 原子加载 shared_ptr（旧 libstdc++ 走内锁） | 代码核实 |
| CR-122 | lockfree_task_executor.cpp:179,186 | push_tasks_batch 每次堆分配临时 vector | 代码核实 |
| CR-123 | lockfree_task_executor.cpp:483-485 | worker 自停路径静默丢弃剩余任务，头文件未文档化 | 代码核实 |
| CR-124 | object_pool.hpp:31-34 | 注释宣称 "wait-free bounded"，实际 acquire 无界 lock-free | 代码核实 |
| CR-125 | comm/lockfree_core.hpp:171-174 | CallbackSlot::set 使 entries_ 无界增长 | 代码核实 |
| CR-126 | comm/bounded_queue.hpp:374-387 | acquire_node 单遍扫描满判断假阳性，try_send 瞬时竞争返回 false 未文档化 | 代码推演确认 |
| CR-127 | comm/mailbox.hpp:63-69 | LET 同相位写者进行中返回 MissedPhase 而非 NotReady（SWSR 下不可达） | 代码核实 |
| CR-128 | comm/mailbox.hpp:213-215 | consumer_lag_ 多读者 last-writer-wins 可回退 | 代码核实 |
| CR-129 | comm/topic.hpp:173-230 | publish 异常部分扇出（同 CR-043） | — |
| CR-130 | comm/realtime_memory.hpp:14-19 | RealtimeAllocationStats 存 string_view，临时 string 悬垂 | 代码核实 |
| CR-131 | comm/phase_gate.hpp:427-438 | Sequencer CAS 失败烧掉已发放票号，未文档化 | 代码核实 |
| CR-132 | comm/mailbox.hpp:78 | LET 发布路径多一次拷贝（应 move） | 代码核实 |
| CR-133 | comm/types.hpp:207-267 | 死代码：emit_comm_event_noexcept / update_latency_stats | 代码核实 |
| CR-134 | timer.hpp:262-264 | stop() 不向在途 tick 传播取消（与 request_cancel 不对称） | 待验证 |
| CR-135 | timer.hpp:742-743 | 定时器线程无条件 1kHz 轮询抢锁；heap 顶 deadline 计算是死代码 | 代码核实 |
| CR-136 | timer.hpp:419-421, 461-463, 503-505 | request_cancel/reschedule catch-all 把 bad_alloc 伪装成 NotFound | 代码核实 |
| CR-137 | realtime_thread_executor.cpp:210, 266-267 | cycle_manager_active_ 写读无锁序，依赖 manager 幂等 | 代码推演确认 |
| CR-138 | realtime_thread_executor.cpp:238, 274-281 | ICycleManager 路径自停止检测失效可重入 stop_cycle（需结合实现确认） | 需确认 |
| CR-139 | executor.cpp:1022-1032 | 周期 tick 被 pool drain 丢弃时 active_ticks 残留（仅诊断失真） | 需确认 |
| CR-140 | cuda_executor.cpp:1068-1076 | P2P async 失败后无条件改写 last_error，掩盖真实错误 | 代码核实 |
| CR-141 | cuda_executor.cpp:1044-1049 | EnablePeerAccess 从不 Disable（有意为之但无注释） | 代码核实 |
| CR-142 | cuda_executor.cpp:998-1013 | p2p_log 调 cudaGetLastError 清 sticky error | 代码核实 |
| CR-143 | cuda_executor.cpp:1460-1462 | 非 async 任务用 cudaDeviceSynchronize 设备级同步，多流过度同步 | 代码核实 |
| CR-144 | cuda_executor.cpp:819-831 | async 拷贝 host 缓冲生命周期完全靠调用方，契约未文档化 | 代码核实 |
| CR-145 | opencl_executor.cpp:485-540 | 同步拷贝持 memory_mutex_ 进行，阻塞所有分配/释放 | 代码核实 |
| CR-146 | opencl_executor.cpp:664-694 | destroy_stream 留 null 洞不复用，queues_ 无界增长 | 待验证 |
| CR-147 | opencl_executor.cpp:729-737 | clGetDeviceInfo 返回值忽略，静默降级 | 代码核实 |
| CR-148 | opencl_executor.cpp:806-832 | 无效 stream_id 校验时机与 CUDA 不一致（延迟到 worker） | 代码核实 |
| CR-149 | opencl_loader.cpp:122 | dlopen 缺 RTLD_LOCAL，cl* 符号进全局命名空间 | 代码核实 |
| CR-150 | cuda_loader.hpp:281 / opencl_loader.hpp:168 | function_resolver_ 成员无任何 setter，死代码 | 代码核实 |
| CR-151 | kernel_launch_optimizer.cpp:69-81 | LRU 淘汰 O(n) 全表扫描且更新也先淘汰，可误逐热点 | 代码核实 |
| CR-152 | cuda_executor.cpp:1248-1272 | destroy_stream 可销毁默认流，restart 只在全空时重建 → stream_id 失效 | 代码推演确认 |
| CR-153 | cuda_executor.cpp:1110-1159 | cudaLaunchHostFunc 回调内调 CUDA API 会驱动级死锁，未文档化 | 代码核实 |
| CR-154 | cuda_executor.cpp:414-463 vs opencl 184-206 | stop 语义不一致：CUDA 排空 vs OpenCL 取消 | 代码核实 |
| CR-155 | cuda_executor.cpp:1298,1337 | const 方法调 cudaSetDevice 改变调用线程当前设备 | 代码核实 |
| CR-156 | cuda_executor.cpp:326-330 | start() 已运行时静默 false 不 set_last_error | 代码核实 |
| CR-157 | task_scheduler_optimizer.cpp:52-79 | get_ready_tasks 不消费任务，连续调用重复返回（契约需明确）；sort 非稳定 FIFO 不保 | 代码核实 |
| CR-158 | gpu_memory_manager.cpp:72-73 | first-fit 线性扫描碎片化；defragment 只合并不搬迁 | 代码核实 |
| CR-159 | opencl_executor.cpp:14-20 | 用错误消息字符串比较改写错误文案，脆弱 | 代码核实 |
| CR-160 | statistics_collector.cpp:24-27 | set_gpu_status_provider 与并发读无同步（当前靠构造顺序侥幸安全） | 代码核实 |
| CR-161 | executor_monitor.cpp:9-15 | mark_partial 拼接 provider 名无分隔空格 | 代码核实 |
| CR-162 | executor_snapshot_formatter.cpp:324-327 | 分配计数靠手工 +1 修正，依赖"输出恒大于 SSO"的脆弱假设 | 代码核实 |
| CR-163 | task_monitor.cpp | （同 CR-071 热路径；修复方向：分片锁/原子计数/interned id） | — |
| CR-164 | tests 基建 | 测试 golden-output 缺失（格式化器格式无锁定测试） | 代码核实 |

---

## 已核实无问题的关键高风险点（审查覆盖面记录）

- **LockFreeQueue 消费协议**：claim-run 双重验证、位置标签防 ABA、发布链 release/acquire 配对（弱序平台成立）。
- **线程池 PA-2 唤醒协议**：代次 bump 覆盖所有使任务可执行的路径；stop 退出守门封住滞留窗口；锁序全局一致无死锁。
- **取消/超时仲裁**：`TaskCancellationState` 单一 phase CAS，promise 恰好满足一次；软超时双满足风险已排除；ms→ns 有饱和 clamp。
- **定时器**：generation heap 与句柄取消线性化正确；shutdown 清理 Scheduled 走 ShutdownCancelled + drain future。
- **RT admission gate**：单 RMW 关门+计数，溢出保护，join 后排空顺序正确。
- **serial FIFO**：ticket 水位 + skipped 集合在各交错下保序正确。
- **comm 发布链**：`kReady release` → head CAS acq_rel → `exchange acquire`；悬挂 next 指针问题已正确规避；PhaseGate LET 无 ABA；SnapshotStore pin 协议无竞态。
- **GPU dlopen 租约**：shared_ptr lease 最后一份析构才 dlclose，正确；CUDA StreamWrapper 销毁与在飞 kernel 竞态已消除。
- **CMake**：`$<BUILD_INTERFACE>` ODR 防泄漏处理正确；Android 分支合理。

---

## 验证结果汇总（2026-09-30）

验证方法：6 个 Independent-Verification-Agent 批次，复现测试位于 `review_verification/`（独立编译，未进主 CTest，未改动库源码）。预构建库：`build/`（普通）、`build-asan/`、`build-tsan/`。

**总计：36 项进入验证 —— 30 项 CONFIRMED（运行时/构建/编译器证据），3 项代码推演确认（运行时无法复现），8 项平台受限（代码核对确认），2 项审查结论被实测修正（见下）。**

### 审查结论修正（重要）

1. **CR-051 定时器漂移——审查主张错误，撤回**。实测（`cr051_periodic_drift.cpp`）：period=100ms、回调 sleep 30ms，50 tick 平均间隔 100.06ms、累计漂移仅 3.0ms；回调 150ms 过载场景同样无漂移。实际语义是**期限锚定计划时刻 + 错过不追补**（timer.hpp:669-676 在 tick 派发前即按 `now + interval` 排下一期限），不是 fixed-delay。残留问题仅为亚毫秒唤醒抖动，不构成缺陷。
2. **CR-104 WhenAll 异常分类——降级为代码级观察**。实测矩阵（`cr104_whenall_classification.cpp`）：5 种依赖失败/取消组合下所有用户可见路径均得到一致的 `DependencyCancelled` 分类，上游真实异常两路径均透传。代码不对称存在（executor.cpp:608-611）但被 `reclassify_dependency_exception` 全路径覆盖，无法构造出行为差异。
3. **CR-083(c) TSAN 标志重复追加——未复现**。两条路径都触发但 CMake flag 生成自动去重，`flags.make` 只含一份。前两个子项（非 Debug 静默失效、ASAN+TSAN 同开构建失败）均确认。

### 验证过程中的新发现（原审查未覆盖）

- **NN-01 ODR/ABI 隐患**：`libexecutor.a` 带 `-DEXECUTOR_THREAD_POOL_TEST_HOOKS` 编译，测试 TU 不定义同宏时 ThreadPool 类布局不一致（线程池 agent 首次复现即 ASAN stack-buffer-overflow）。hook 成员布局应与宏解耦，或该宏不应影响类布局（改 pimpl/函数指针表）。
- **NN-02 CR-001 触达面**：仅 tracked 路径（`submit_with_handle` 等）触发退出 UAF；legacy `submit_auto` 路径不触达 registry/task_dependencies。修复时勿只测 legacy 路径。
- **NN-03 实时分配 guard 默认关闭**：`EXECUTOR_ENABLE_REALTIME_ALLOCATION_GUARD=OFF`（build/CMakeCache），即"实时路径无分配"的库内校验手段默认不存在，与 CR-044 叠加后承诺完全无验证。
- **NN-04 CR-030 补充**：仅 nice 子路径（priority∉[1,99] 且≠0）有缺陷；SCHED_FIFO 子路径非 root 返回 EPERM 属诚实失败。对照实验证明按 tid 定向 `setpriority` 可行，修复无移植障碍。
- **NN-05 CR-024 附带现象**：洪泛尾段，滞留 worker 本地队列的 HIGH 对全局严格优先级不可见，LOW 可提前 ~100ms 执行（本地队列优先级倒置）。
- **NN-06 CR-106 归因修正**：`get_executor_capabilities()` 本身仅 ~1µs；submit_auto 慢 65-70% 的主要开销在 `route_task` + `record_routing_decision`（互斥锁 + RoutingDecision 队列 + 字符串拷贝，executor.cpp:1532-1552）。
- **NN-07 CR-004 升级**：TSAN 插桩下竞争已造成实际崩溃（`:339` 间接调用读到撕裂的 nullptr，SEGV pc=0x0），非纯技术性报告。
- **NN-08** Coverage.cmake 的 `executor_apply_coverage_to_test()` 是死代码（tests/CMakeLists.txt 各处内联硬编码覆盖率标志，未复用该函数）。
- **NN-12 ThreadPool queued 诊断乱序（master CI 实锤，2026-10-01）**：三条 submit 路径（try_submit / try_submit_priority / try_submit_batch）均在 `scheduler_.enqueue` + 唤醒之后才锁外补记 `record_task_queued`。worker 不持有 mutex_，可在补记前取走任务执行——`record_task_start` 查不到条目成为 no-op，诊断条目以 Queued 状态在任务已运行时才创建。快照 `in_flight_state_counts` 可能缺 Running/Queued 键，gtest 断言 `.at()` 抛 out_of_range 直接 terminate（test_executor_snapshot 在 ubuntu gcc Release 与 lockfree scheduled 两次 CI abort）；complete 先于补记时还会留下永不清理的僵尸 Queued 条目。审查漏网：Phase 2 触及 monitor 析构/驱逐计数，未覆盖 submit 侧记录顺序。

### 复现测试索引

| CR | 判定 | 测试（review_verification/） | 关键证据 |
|---|---|---|---|
| CR-001 | 已确认 | `cr001_singleton_exit_uaf.cpp`（ASAN） | 静态析构窗口内确定性 SEGV：worker→executor.hpp:2034→registry finalize 锁已析构 mutex |
| CR-004 | 已确认 | `lf_cr004_hook_race.cpp`（TSAN） | 2 条 data race + `:339` call-through-null SEGV |
| CR-005 | 推演确认 | `lf_cr005_park_latency.cpp` | 52 万投递 p99.9=186µs，零丢失唤醒签名 |
| CR-010 | 已确认 | `cr010_submit_on_registry_leak.cpp` | 65536 槽全漏 → "registry capacity exhausted"，取消失效 |
| CR-011 | 已确认 | `cr011_tracked_graph_leak.cpp` | RSS 5.5MB→64.8MB 不回落，in-flight 每 4 万次 +40000 |
| CR-012 | 已确认 | `cr012_serial_context_dangling.cpp`（ASAN） | worker 在 `post_reserved` 锁已析构 context 永久 futex 等待，shutdown 挂死 |
| CR-013 | 已确认 | `cr013_lazy_pool_read_path.cpp` | 只读诊断查询线程数 1→5 |
| CR-020 | 已确认 | `cr020_monitor_throw_future_hang.cpp` | future timeout、任务未执行，但池统计 completed=1 |
| CR-022 | 已确认 | `cr022_submit_after_failed_init.cpp` | 未初始化提交 accepted=true 永不执行（两条路径） |
| CR-024 | 已确认 | `cr024_low_priority_starvation.cpp` | 3 秒洪泛期间 LOW sentinel 启动数恒 0 |
| CR-025 | 已确认 | `cr025_batch_fifo_order.cpp` | 200/200 轮乱序，误序位 6200/6400 |
| CR-030 | 已确认 | `lf_cr030_thread_priority.cpp` | 主线程 nice 0→19，worker 保持 0；按 tid 定向对照成功 |
| CR-031 | 已确认 | `lf_cr031_pool_retention.cpp` | T1 完成后 300ms Guard 仍存活；析构发生在主线程 push 期间 |
| CR-033 | 推演确认 | （代码核对 :526-548） | 退出条件确无 Cancelled；终局 CAS 兜底无功能破坏 |
| CR-034 | 已确认 | `lf_cr034_registry_overwrite.cpp` | 覆盖后 A 不可达、永不取消；facade id 唯一使其不可达 |
| CR-040 | 已确认 | `cr040_bounded_queue.cpp` | DropOldest 2 秒 20999 次不应发生的失败；KeepLatest 3 个最新序号永久丢失 |
| CR-042 | 已确认 | `cr042_drain_budget.cpp` | options 配 0 一次排空 5000 条（参数传 0 则回退默认 64） |
| CR-044 | 已确认 | `cr044_aligned_new.cpp` | guard 激活下 alignas(64) 分配 count=0 完全绕过；默认构建 guard OFF |
| CR-051 | 修正撤回 | `cr051_periodic_drift.cpp` | 100.06ms 平均间隔，无漂移 |
| CR-052 | 已确认 | `cr052_dependency_dfs.cpp` | 建链 O(n²)（1 万→12.9s）；256KB 栈 n=4000 SIGSEGV；DFS 持写锁阻塞无关 add 43ms |
| CR-053 | 已确认 | `cr053_rt_capacity.cpp` | status 上报 1024 但第 1001 个 push 因池耗尽被拒，queue_full 恒 0 |
| CR-054 | 已确认 | `cr054_rt_stop_latency.cpp` | 500ms 周期 stop 均值 400.2ms，随周期线性 |
| CR-060 | 已确认 | `test_cr060_alignment.cpp` | 直通与池路径 20/20 指针 `%16==8` |
| CR-061 | 已确认 | `test_cr061_select_device.cpp` | 选高负载设备（150>140），初值漏加任务开销 |
| CR-062 | 已确认 | `test_cr062_scheduler.cpp` | remove 后下游误 ready；100 万次 +72.3MB 无清理 API |
| CR-063 | 已确认 | `test_cr063_races.cpp`（TSAN） | 3 轮 10/10/9 条 race，写侧 update_config 读侧 9 个方法 |
| CR-065 | 平台受限 | `test_cr065_dll_names.cpp` | Windows CUDA 9/10/11 全部哑失败，仅 12.x 可发现 |
| CR-070 | 已确认 | `test_cr070_snapshot_newline.cpp` | `...applied=truerealtime[rt1].dropped_...` |
| CR-073 | 已确认 | `test_cr073_virtual_dtor.cpp` | -Wdelete-non-virtual-dtor 警告 |
| CR-074 | 已确认 | `test_cr074_sampling_rate.cpp` | -1.0 → 100% 采样；NaN → 0% |
| CR-075 | 已确认 | `test_cr075_inflight_eviction.cpp` | 任务全部完成后 incomplete 仍 =1 |
| CR-080 | 已确认 | /tmp/cov 构建实验 | executor flags 无 --coverage，测试目标有；根因 include 时序 |
| CR-083 | 已确认(a,b) | /tmp/san1、/tmp/san2 | Release+SAN 零警告失效；ASAN+TSAN 同开 cc1plus error |
| CR-084 | 已确认 | `test_cr084_downstream.cpp` | 安装树无 util/，下游 fatal error 找不到 exception_handler.hpp |
| CR-102 | 已确认 | `test_cr102_trim_perf.cpp` | cap=1024: 28.4µs vs cap=0: 18.2µs（每终态节点 7-10µs） |
| CR-106 | 已确认 | `test_cr106_submit_auto_perf.cpp` | submit 9.0µs vs submit_auto 15.5µs |
| CR-104 | 修正降级 | `cr104_whenall_classification.cpp` | 5 组合全部分类一致 |
| CR-117 | 已确认 | `cr117_resizer_cap0_expand.cpp` + 对照 | cap=0：queue=3 即扩、75s 空闲不缩；cap=512 对照缩容正常 |
| CR-108 | 已测量 | `cr108_notify_overhead.cpp` | 135k→31k ops/s（有等待者，含锁竞争上界） |
| CR-003/064/066 | 平台受限 | `probe_opencl.cpp` | 本机 0 个 ICD 平台；代码核对确认（CR-003：stop 从不 cleanup） |
| CR-002/014/021/023/035/036/043/050/056/057/081/082/086 | 平台受限/代码推演 | — | 无 CUDA GPU / 无 Windows / 触发需 OOM 等不可注入条件 |

---

## 修复计划

按"先内存安全与挂死、再功能正确性、再构建、最后性能"分四个阶段。每项流程：主循环修复代码 → Independent-Verification-Agent 用 `review_verification/` 现有复现测试复验转绿 → 复现测试迁移进 `tests/` 成为回归测试（修正断言方向：修复后应通过）。阶段一完成前不动阶段二，避免半修复状态混淆验证。

### Phase 1 — 内存安全 / 挂死 / 数据竞争（P0，最高优先）

| 项 | 修复 | 验收 |
|---|---|---|
| CR-001 | 单例 `~Executor` 同样执行 `shutdown(true)`（幂等，实例模式已是空操作）；保留 atexit 兜底 | cr001 转绿；legacy 与 tracked 两路径都测（NN-02） |
| CR-004 | `user_before_publish_hook_/context_` 合并为单一 `std::atomic<HookState*>` 快照，trampoline 一次加载后解引用 | lf_cr004 TSAN 10 万次切换零报告、零崩溃 |
| CR-012 | `submit_on*` 改持 `std::shared_ptr<SerialExecutionContext>`；或短期方案：文档化硬契约 + ticket 登记 context 存活检查 | cr012 转绿（context 先亡不再挂死/UB） |
| CR-010/011 | 两条提交路径 catch 中补齐与 `try_submit_task` 一致的清理：`state->try_reject()` + `mark_task_graph_failed(...)` + `cancellation_registry_->finalize(...)` | cr010/cr011：失败循环后 registry active 归零、RSS 稳定 |
| CR-020 | `execute_task` 外层 catch 中若任务未开始执行，镜像软超时分支调用 `on_timeout(current_exception())` 结算 promise | cr020：future 以异常就绪，worker 存活 |
| CR-022 | 提交路径锁内校验 `initialized_`；`config_.task_timeout_ms` 改 atomic 快照或纳入锁内读取 | cr022：未初始化提交被拒绝（返回失败而非接受） |
| CR-002 | CUDA `start()` 各失败路径返回前排空 `task_queue_` 并对 promise `set_exception`；`wait_for_completion` 谓词加"`!is_running_` 且无 worker"退出条件 | 代码层修复（无 GPU 机器以单测注入 fake loader 验证排空逻辑） |
| CR-003 | OpenCL `stop()`（或下次 `start()` 入口）调用 `cleanup_locked()`；对齐 CUDA 的 `streams_.empty()` 重建守卫 | 代码层修复 + 有 ICD 机器复验；先补单元级断言（context/queue 计数） |
| CR-052 | `dfs_path_exists` 改显式栈迭代；环检测增量化（入度/拓扑维护）摆脱全图 DFS | cr052：5 万链建链 <1s、256KB 栈 n=4 万不崩、无锁内长阻塞 |

### Phase 2 — 功能正确性（P1）

| 项 | 修复 | 验收 |
|---|---|---|
| CR-040 | KeepLatest/DropOldest 竞争路径（promotion/消费者认领失败）短暂自旋重试；无法重试时引入独立 `ContendedDrop` 事件区分 | cr040：DropOldest 场景 try_send 失败数→0（或独立事件计数）；KeepLatest 最新序号保全 |
| CR-060 | `kHeaderSize = align_up(sizeof(size_t), kAlignment)`，或 header 移出用户块 | test_cr060：全部指针 %256==0 |
| CR-061 | `select_best_device` 初值统一用 `estimated_total_cost + task.estimated_cost` | test_cr061：两迭代序下均选低总开销设备 |
| CR-062 | `remove_task` 级联移除/失败标记下游；`completed_tasks_` 在依赖全解析后清除（或引用计数替代） | test_cr062：remove 后下游不 ready；100 万次 RSS 稳定 |
| CR-063 | 两个 optimizer 的 `config_` 改 `std::shared_ptr<const Config>` 原子换指针（读路径零锁） | test_cr063 TSAN 零报告 |
| CR-030 | nice 调整移入目标线程执行（worker 启动自设），或按 `gettid` 定向；修正"Linux 无 per-thread nice"错误注释 | lf_cr030：worker nice 生效、主线程不变 |
| CR-031 | worker 侧 `release_bulk` 前清空 `wrapper->func`（析构回 worker 线程）；补 `static_assert(std::is_default_constructible_v<T>)` 与复用模型文档 | lf_cr031：无滞留、析构线程=worker |
| CR-013 | 读路径统一 `has_default_async_executor()` 短路；懒初始化失败用一次性 latch 固定，不再反复重试 | cr013：只读查询不建池（线程数不变） |
| CR-025 | 批内任务时间戳逐个递增（或同批序号入比较器） | cr025：单 worker 下批内执行序=提交序 |
| CR-117 | resizer 侧对 capacity=0 应用与队列层相同的钳制（或显式禁用自动扩缩） | cr117：cap=0 行为与 cap=100 一致 |
| CR-053 | RT 对象池用取整后容量构造（`lockfree_queue_.capacity()`），status 上报池容量 | cr053：容量一致、归因正确 |
| CR-054 | RT 循环 `sleep_until` 改 `cv.wait_until` + stop 通知（对齐定时器 1ms 分片方案作 TSAN 规避） | cr054：stop 延迟 <10ms 量级 |
| CR-070 | formatter :216 补 `'\n'`；新增 golden-output 测试锁定格式 | test_cr070 转绿 + golden 回归 |
| CR-073 | `virtual ~TaskMonitor() = default;` | 警告消失 |
| CR-074 | 先 `if (!(rate >= 0.0)) rate = 0.0;` 再转换（同除 NaN） | cr074：-1.0/NaN → 0% |
| CR-075 | 缩容驱逐独立计数，`incomplete` 仅反映运行期准入丢弃 | cr075：任务全部完成后 incomplete=false |
| CR-034 | `register_state` 重复 id 拒绝（返回 false）或 debug 断言 | lf_cr034：第二次注册返回 false |
| CR-005 | `park_worker` 第 (2) 步改 `fetch_or(kParkedBit, acq_rel)`，终扫带回任务保持 `fetch_and` | lf_cr005 压测无回归；注释证明改为基于 RMW 全序 |
| CR-042 | `max_items_per_cycle==0` 回退默认值或视为 InvalidArgument（与参数层语义对齐） | cr042：options 0 与参数 0 行为一致 |
| CR-044 | 补 aligned/nothrow `operator new` 替换变体；评估 guard 默认开启（NN-03） | cr044：alignas(64) 分配被计数 |
| CR-024 | 决策：文档化严格优先级语义或加老化（建议先文档，老化属行为变更） | 文档 + （如实施）cr024 洪泛期间 LOW 有进展 |
| CR-050/021/023 | 定时器线程三处外呼、resize 监控线程、worker dispatch 各包 catch-all + 计数（防 OOM terminate） | 注入式单测（hook 抛异常）线程存活 |
| CR-014 | `~ExecutorManager`/`~ThreadPool` 的 drain 包 try/catch（对齐 retired 路径） | 代码审查 + 注入单测 |

### Phase 3 — 构建 / 打包

| 项 | 修复 |
|---|---|
| CR-080 | Coverage 插桩移到 `add_subdirectory(src)` 之后（或函数式调用放 src/CMakeLists 末尾）；tests 复用该函数（顺带消灭 NN-08 死代码） |
| CR-083 | Sanitizers.cmake 无条件 include（内部按 Debug 警告）；加 ASAN/TSAN 互斥 FATAL_ERROR；删除根 CMakeLists 重复 TSAN 块 |
| CR-084 | `exception_handler.hpp`（及其依赖闭包）纳入安装，或头迁入 include/；加"安装树试编译公共头"CI 冒烟 |
| CR-081/082 | package_windows.ps1 删除多余 `Split-Path`；build_windows.ps1 开关传参修正；build_and_package 子脚本显式 `exit 0` |
| CR-085 | 打包脚本从 CMakeLists 提取版本号作为默认值；config.hpp 增 configure_file 生成的版本宏 |
| CR-086 | MSVC 守卫扩展到非 MinGW 的 WIN32 分支 |
| NN-01 | `EXECUTOR_THREAD_POOL_TEST_HOOKS` 不再影响类布局（hook 改 pimpl/always-present 成员），或在文档与代码两处强制断言宏一致 |

### Phase 4 — 性能（P2，按收益排序）

1. **CR-106**（~6.5µs/次，65-70%）：路由结果缓存 + `record_routing_decision` 降频/改 lock-free 环形缓冲，能力采集仅在 CpuOrGpu 意图时执行。验收：submit_auto 与 submit 差距 <20%。
2. **CR-108**（有等待者 -76.8%）：完成通知改原子等待者计数，仅 `>0` 时 notify。验收：cr108 有等待者场景差距显著收窄。
3. **CR-102**（每终态 7-10µs 持全局图锁）：trim 摊还（每 N 次 finalize 触发一次）。验收：test_cr102 cap=1024 与 cap=0 差距 <10%。
4. CR-109/110/111/112/121/122/151/135：轮询改谓词等待、提交路径分配合并、optional 锁包装、hook 快速开关、批处理复用缓冲、LRU 结构、定时器 cv 化——逐项小改，随 Phase 2 顺带处理。

### 明确不修（记录理由）

- **CR-051**：审查主张被实测推翻，无漂移缺陷；timer.hpp 注释可补充"期限锚定、错过不追补"语义说明即可。
- **CR-104**：行为一致，仅在代码层补一条注释说明 reclassify 全路径覆盖，防将来改动破坏。
- **CR-033**：无功能破坏的有界尾延迟，仅在自旋处补注释与 `Cancelled` 早退（1 行，随 CR-005 顺带）。
- CR-120/124 等不可达边界：补注释即可，不做防御代码。

### 执行节奏

- Phase 1 共 9 项，预计 3-4 个工作日（CR-002/003 无硬件，以注入式单测验收）；完成后一次性交 Independent-Verification-Agent 全量复验并迁移回归测试。
- Phase 2 共 24 项，多数为小改（<20 行），按模块串行：线程池 → comm → GPU → 监控 → 工具，每模块完成即复验。
- Phase 3 与 Phase 2 并行（互不触碰源码）。
- Phase 4 在 v0.5.3 修复版本之后、v0.6 性能版本落地。

---

## Phase 1 修复执行记录（2026-09-30）

全部 9 项已实施并经 Independent-Verification-Agent 复验。回归状态：普通构建 161/161 通过；ASAN 套件除既有贴边的 `benchmark_lockfree_task_executor` 延迟阈值（基线代码同样 2/3 超限，非回归）外全绿；TSAN 套件 `setarch -R` 下除负载超时外全绿。

| 项 | 修复内容 | 复验判定 | 证据 |
|---|---|---|---|
| CR-001 | 单例 `~Executor` 经 `has_default_async_executor()` 守卫后 `shutdown(true)` 排空（不触发懒建池副作用） | FIXED | widow 模式下排空发生在静态析构窗口前；`cr001_singleton_exit_uaf_fixed` ASAN 零报告、exit=0 |
| CR-004 | hook 改原子快照指针 + 不可释放去重节点，trampoline 单次加载 | FIXED | TSAN 10 万次切换 + 41 万 push：零 race 报告、零崩溃（修复前 2 race + call-through-null SEGV） |
| CR-010 | submit_on bind 抛异常 catch 补齐终态化（try_reject/图失败/finalize/terminal/admission） | FIXED | 7 万次失败提交（>65536 容量）registry 不再耗尽，取消功能完好，in_flight=0 |
| CR-011 | tracked 提交 make_tuple 抛异常同样补齐；`std::optional::emplace` 构造避免 API 回归 | FIXED | 20 万次失败提交 RSS +856KB（修复前 +59MB）、in_flight=0、取消可用；不可默认构造/可赋值类型编译恢复（探针 probeB/probeC） |
| CR-012 | SerialExecutionContext 改 pimpl（Shared+detach），facade 闭包持 `shared_ptr<Shared>` | FIXED | context 先亡不再挂死：future 以 ExecutorStopping 就绪、exit=0、ASAN 零报告；存活对照组 FIFO 正常 |
| CR-020 | execute_task catch 中对未开始任务触发 `on_timeout` 结算 promise | FIXED | monitor 抛异常时 future 2s 内以注入异常就绪，worker 存活 |
| CR-022 | 三条提交路径锁内校验 `initialized_`、timeout_ms 锁内读取 | FIXED | 未初始化/回滚两路径均拒绝（accepted=false），成功初始化路径行为不变（15/15 PASS） |
| CR-002 | CUDA start() 五个失败路径 `fail_all_queued_tasks` 排空 + wait_for_completion 谓词加 `worker_started_` 退出条件 | 代码审查通过（无 GPU 无法运行时验证） | 五路径均排空；const_cast move 安全（比较器只读整型）；新谓词在正常 stop 排空期间不提前返回。残余小风险（已记录）：排空 TOCTOU 窗口内新提交的 promise 可能不结算，但 wait 不再挂死 |
| CR-003 | OpenCL initialize_opencl 入口收编上一代资源：先 clFinish 全部存活队列再 cleanup_locked() | 代码审查通过（无 ICD 无法运行时验证） | 泄漏与 queues_ 无界增长路径被收编；锁序与全文件一致无死锁环；clFinish 阻塞 start 有界 |

### 偏差与遗留

- **CR-052 范围**：本阶段仅消除栈溢出（256KB 栈 n=10000 SURVIVED，修复前 n=4000 即 SIGSEGV；环检测正确性回归通过）。**O(n²) 建链成本与锁内遍历仍在**（n=4000 约 3.9s，倍增比 ~4.1），增量拓扑序属结构性改动，留待后续阶段。
- **执行中发现并已修正的中间回归**：CR-011 首版用具名 tuple（要求元素可默认构造/可赋值）造成 tracked 提交 API 编译回归，复验抓出后改为 `std::optional::emplace`（对元素要求与原语义一致）。
- **验证有效性备注**：复验 agent 发现 build/ 与 build-tsan/ 曾存在旧库二进制（时间戳早于源码改动），已重建后复验——后续验证一律先确认库新鲜度。
- 既有环境性问题（非本次回归，已定位）：`benchmark_lockfree_task_executor` ASAN P99 阈值贴边（基线同样超限）；test_thread_pool/test_realtime_thread_executor 在满载 ctest 下偶发超时（单跑与 8× 压力下不复现）。建议 Phase 3 一并处理（放宽 ASAN 阈值或标注性能测试非并发安全）。

---

## Phase 2 修复执行记录（2026-10-01）

全部 24 项已实施并经两个 Independent-Verification-Agent 并行复验：23 项运行时/diff 证据确认 FIXED，CR-044 复验抓出编译排序缺陷后修正并转绿。回归状态：普通构建 161/161；ASAN/TSAN 套件仅剩已记录的既有贴边项（benchmark P99 阈值、TSAN 负载超时）。

| 项 | 修复内容 | 复验判定 | 关键证据 |
|---|---|---|---|
| CR-025 | 批内 submit_time_ns 逐个递增 | FIXED | 单 worker 端到端 1600/1600 位置无乱序（修复前 6200/6400） |
| CR-117 | resizer 用有效容量（cap=0 → kDefaultCapacitySlots） | FIXED | cap=0 下扩缩容行为与 cap=100 一致 |
| CR-021/023 | resize 监控线程与 worker dispatch 异常屏障 | FIXED | diff 审查（catch + exception_handler_ 记录） |
| CR-014 | ~ThreadPool/~ExecutorManager 析构排空 try/catch | FIXED | diff 审查 |
| CR-040 | 置换路径 64 次有界自旋重试 + DropPolicy best-effort 契约注释 | FIXED | DropOldest 忙消费者失败 20999→**0**；KeepLatest 恒满最新序号 0 丢失 |
| CR-042 | options 层 max_items=0 回退默认 64 | FIXED | 配 0 单次 drain=64（修复前 5000） |
| CR-060 | kHeaderSize 提升为 kAlignment | FIXED | 40/40 指针 %256==0（修复前 %16==8）；test_gpu_defragment 布局镜像同步更新 |
| CR-061 | select_best_device 初值公式统一 | FIXED | 正/反向迭代序均选对（新增 _fixed2 反向变体对旧代码验证有效） |
| CR-062 | remove_task 级联 + completed_tasks_ 按反向依赖索引回收 | FIXED | remove 后下游不再 ready；100 万次 RSS +0.2MB（修复前 +72.3MB） |
| CR-063 | 两个 optimizer config 原子快照 | FIXED | TSAN 3 轮 0 报告（修复前 10/10/9） |
| CR-070 | 格式化器补换行 | FIXED | 0 畸形行 |
| CR-073 | TaskMonitor 虚析构 | FIXED | 警告消除 |
| CR-074 | 采样率先钳制再转换 | FIXED | -1.0/NaN → 0%（修复前 -1.0→100%） |
| CR-075 | 缩容驱逐独立计数 | FIXED | 完成后 incomplete=false |
| CR-005 | park_worker 改 fetch_or（含返回值误用的执行中修正） | FIXED | 0 丢失唤醒签名；IdleWorkerParks 通过；A/B 基准实证消除基线的 P99 秒级尖峰 |
| CR-030 | nice 经弱符号 pthread_gettid_np 定向目标线程 | FIXED（平台受限） | 主线程污染消除（5/5 轮 main nice 不变）；**glibc 不导出该符号**，跨线程定向诚实失败——自线程路径（RT 执行器）正常生效；ThreadPool 控制线程设 worker nice 场景留待后续（worker 启动自设方案） |
| CR-031 | worker 归还前清空 callable | FIXED | 无滞留、析构线程=worker |
| CR-034 | register_state 拒绝重复 id | FIXED | 第二次注册返回 false，原条目取消语义保持 |
| CR-013 | no-create 只读 getter + 懒创建失败闩存 | FIXED | 只读诊断线程数 1→1（修复前 1→5） |
| CR-044 | 补齐 aligned/nothrow operator new/delete 变体（含 MSVC 分支） | FIXED | 复验抓出定义后置的编译缺陷（guard 宏本地不开、逃过本地检查），修正后：对齐 new 计数=1、Abort 生效 |
| CR-050 | 定时器线程三处外呼异常隔离 | FIXED | diff 审查（持锁点 catch 走 continue） |
| CR-024 | 严格优先级饿死与本地队列倒置窗口文档化 | FIXED | 契约注释落地 |
| CR-021 附带 | test_executor_snapshot 软超时脆弱性修复（内层 promise 与外层 future 赛跑 + 5 次重试） | FIXED | 负载并发 100+ 轮 0 挂死（修复前 ~1/20；resolve 升级定位：非库缺陷，预存在测试脆弱性，Phase 2 改变负载形状使其显形） |

### 残余风险与新发现（记录）

- **NN-09 KeepLatest 饱和语义**：`recycle_all_for_write()` 每次置换丢弃整个队列内容——饱和下"保留最新 capacity 条"实际退化为"保留最新 1 条"，overwritten 计数少计。**HEAD 既有行为，非本次回归**；验收判据（最新不丢、不误拒）已满足，语义口径建议单列后续项。
- **NN-10 CR-030 平台限制**：glibc 无 pthread_gettid_np（弱符号为 nil），跨线程 nice 定向诚实失败。方向：worker 启动自设 nice（对齐 RT 执行器 self_handle 模式）。
- **NN-11 test_executor_facade** 负载下 80 轮出现 2 次非挂断 rc=1（未复现、非本批改动文件），留观察。
- **教训**：guard 宏路径（EXECUTOR_ENABLE_REALTIME_ALLOCATION_GUARD）不在本地默认构建中，本次逃过编译检查——后续涉及该宏的改动必须带宏编译验证（CI 已有专门 job 覆盖）。

## master CI 修复执行记录（2026-10-01，NN-12）

Phase 2 合并（0963df5）后 master push 与 scheduled 两次 CI 失败，PR 上同代码全绿，均为条件触发：

1. **test_executor_snapshot abort（ubuntu gcc Release + lockfree scheduled）**：进程启动 0.17s 内 `terminate called after throwing an instance of 'std::out_of_range (map::at)'`。根因 NN-12：`test_snapshot_reports_bounded_in_flight_tasks` 断言 `.at(Running)` 时，首任务已被 worker 执行但 `record_task_queued` 尚未在提交线程补记——诊断条目以 Queued 创建且无 Running 键。Phase 2 期间本地与 CI 均未命中（提交→补记窗口极窄，CI 2 核负载下放大）。
2. **test_worker_local_queue_steal 超时（Windows MSVC Debug）**：10000 轮 × 固定 200µs 自旋窗口，2 核 runner + ctest 并行抢占下 sleep/自旋被放大数十倍，纯时长问题非正确性问题。

修复：

- **thread_pool.cpp 三处 submit 路径**：`record_task_queued` 移入 mutex_ 临界区、`enqueue` 之前（批量路径同步撤销 PA-14 的"全局锁外补记"），建立 queued→start→complete 严格顺序；同时消除 complete-先于-补记时诊断表滞留僵尸 Queued 条目的泄漏。锁序 pool.mutex_ → monitor.mutex_ 单向（monitor 回调从不反向取 pool 锁），无反转风险。
- **test_executor_snapshot.cpp 断言加固**：worker 在结算 future 之后才清理诊断表（future 就绪 ≠ 快照已排水），复验在 14 核全超订阅下实测 :315/:402 两处 `in_flight_count == 0` 断言 ~40% 失败（旧有负载敏感，非本次引入）。新增 `wait_in_flight_drained()` 有界等待（50×2ms）替换三处立即断言（含 third/fourth 排水，保护 monitoring-off 断言语义纯净）。
- **test_worker_local_queue_steal.cpp**：竞争窗口改为"批次耗尽即收尾，200µs 封顶"（等价覆盖 pop/steal 交错，去除空转）；轮数加 2s 墙钟预算（最少 128 轮下限），负载机器上测试时长有界。

复验证据（Independent-Verification-Agent）：旧代码在 /tmp 副本注入 300µs 窗口后 10/10 重现 CI 同款 abort（gdb 定位 `:302 .at(Running)`）；新代码空载 200 连跑 + CI 级负载 100 连跑 0 abort；探针 3000 次迭代（含满载）0 错标、0 僵尸条目，旧注入版 100% 错标 + 3/300 僵尸条目。steal 测试满载 ~2s 有界。lockfree 变体（`EXECUTOR_LOCKFREE_QUEUE=ON` → `USE_LOCKFREE_WORKER_QUEUE`，即 CI scheduled job 的实际配置）33/33。ASAN/TSAN 抽查无新增报告。全量 161/161 对齐基线。

预期：in-flight 状态计数在任意负载下可信（Queued 仅表示"尚未被 worker 取走"）；steal 测试在 2 核 Debug 下秒级完成。

## Phase 3 修复执行记录（2026-10-01）

8 项全部实施。构建矩阵冒烟（本地）：

| 项 | 修复 | 本地证据 |
|---|---|---|
| CR-080 | Coverage 插桩改函数式 `executor_apply_coverage_to_target()`，src/CMakeLists 在 add_library 后显式调用（撤销 include 期恒假的 `if(TARGET executor)`） | Coverage ON 配置下 `src/CMakeFiles/executor.dir/flags.make` 首次出现 `--coverage -fprofile-arcs -ftest-coverage` |
| NN-08 | tests/CMakeLists 六处内联覆盖率块全部改调 `executor_apply_coverage_to_test()`（原"死函数"成为唯一入口） | 6 处替换后 `fprofile-arcs` 在 tests/CMakeLists 清零；4 个抽样测试 TU flags 均含插桩 |
| CR-083 | Sanitizers.cmake 无条件 include；TSAN 独立于总开关（CI Release+TSAN 路径保留）；ASAN/TSAN 互斥 FATAL_ERROR（显式同开）；根 CMakeLists 旧 TSAN 块删除 | 矩阵：TSAN+Release 配置通过且 flags 含 thread（CI 对齐）；TSAN+显式 ASAN → 配置期 FATAL_ERROR；总开关+TSAN → ASAN 让位（仅 thread+ubsan） |
| CR-084 | `executor/util/` 纳入安装（gpu 头的 `../util/exception_handler.hpp` 依赖闭包）；顺带修复 gpu 头 `../../../include/...` 源码树相对包含在安装树必断链的问题（cuda/opencl_executor.hpp 共 6 处改 `<executor/...>`，构建树经目标 PUBLIC include 解析） | 新增 `install_headers_smoke` ctest：安装到临时 prefix 后编译包含全部 43 个已安装头的探针 TU，通过 |
| CR-085 | 版本单一来源：打包脚本（4 bash + 2 ps1）默认值改从 CMakeLists `project(VERSION)` 提取（可被 env/参数覆盖）；新增 configure_file 生成的 `executor/version.hpp`（EXECUTOR_VERSION_MAJOR/MINOR/PATCH/STRING），BUILD_INTERFACE + install 双路可用 | `sed` 提取 0.5.2 ✓；build/executor/version.hpp 生成 ✓；探针含版本头编译通过 |
| CR-086 | 共享库配置期守卫从 `if(MSVC)` 扩到 `if(MSVC OR (WIN32 AND NOT MINGW))`（覆盖 clang-cl / clang-gnu-on-Windows；MinGW 默认全导出不拒） | diff 审查（本地无 Windows 工具链） |
| CR-081 | package_windows.ps1 撤销多余的第二次 `Split-Path`（包内 README/LICENSE/CHANGELOG 恢复可见）；build_windows.ps1 死代码 `$ProjectRoot` 双写删除 | diff 审查（pwsh 本地不可用，运行时验证平台受限——与审查结论一致） |
| CR-082 | build_windows.ps1 `-BuildTests/-BuildExamples` 开关透传到静态/共享两个 CMake 配置块（旧版硬编码 OFF 无视入参） | diff 审查（同上） |
| NN-01 | ThreadPool 测试钩子（3 个 std::function 成员 + 3 个 setter）无条件参与类布局，`EXECUTOR_THREAD_POOL_TEST_HOOKS` 不再影响布局——ODR 违例（宏不一致 TU ↔ 库，ASAN stack-buffer-overflow）结构性消除；宏仅为兼容保留定义 | 普通构建 162/162 全绿（含钩子测试）；"无宏 TU + 带宏库"布局一致性由独立复验以 ODR 探针确认 |

回归状态：普通构建 162/162（新增 install_headers_smoke）。全量 ctest 无失败。

**新增交付物**：`tests/install_headers_smoke.cmake`（ctest `install_headers_smoke`，GNU/Clang 注册）、`cmake/version.hpp.in`（生成 `executor/version.hpp`）。

**平台受限项**：两个 ps1 脚本的运行时行为（打包产物、开关透传效果）本地无 pwsh 无法执行验证，依据逐行 diff 审查 + 与 bash 参照实现对齐；后续如启用 Windows 打包 CI job 可补运行时冒烟。

### Phase 3 CI 后续修正（2026-10-01，PR #205 内追加提交）

1. **tests/install_headers_smoke.cmake 未入库**：根 .gitignore 的 `*.cmake` 全忽略 + 仅白名单 cmake/ 模块，冒烟脚本被静默吞掉（git status 不可见），首次 CI 全部 Linux job 以 "CMake Error: Not a file" 失败。修正：.gitignore 显式白名单该文件（296cbaa）。教训：新增 .cmake 文件时必须核对 ignore 白名单。
2. **插桩库的消费方链接失败**：CR-080 生效后库首次携带 gcov 插桩，独立定义的测试目标（test_multithread_mpsc、android_smoke）链接报 undefined `__gcov_init/__gcov_exit/__gcov_merge_add`——静态库的插桩符号须在最终链接落地。修正：executor 目标加 INTERFACE `--coverage` 链接选项，tests/examples/find_package 下游自动继承（942b5dd）。本地验证：Coverage 构建下两目标链接运行通过、库 .gcda 22 个落地；CI Code Coverage job 转绿。

## Phase 4 修复执行记录（2026-10-01）

| 项 | 修复 | 状态/证据 |
|---|---|---|
| CR-106 | `route_task`/`route_dispatch` 惰性能力采集（非 CpuOrGpu 意图与策略拒绝路径不再锁 5 把注册表）；`record_routing_decision` 增加观测快速开关（容量 0 且无回调时零开销）+ 无回调时移动入库免字符串拷贝 | **同参 Release 交错 9 轮中位：+19.9% → +11.6%（<20% 验收达成）**；record-off 变体仅再快 ~0.08µs（record 残余极小）。注：原审查"基线 65-70%"是 O2-TU+O0-lib 混合优化口径的夸大值，同参 Release 真实基线 ~+20% |
| CR-108 | 完成通知等待者门控：`completion_waiters_` 原子计数由 try_wait 的 RAII 守卫维护，`notify_completion_waiters` 仅在有等待者时 notify_all——无等待者场景每任务 2 次 notify_all 全部消失 | 无等待者吞吐 +0.6%（不回退）；**有等待者场景不收窄（83.6% vs 84.5%）——机制边界使然**：门控只消除无等待者通知，有等待者时新旧同样每任务 notify，复验证实并已修正本表预期 |
| CR-109 | `try_wait_for_completion` 改通知驱动谓词等待 + 50ms 兜底分片（旧 10ms 盲轮询：300s = 3 万次全量扫队列；现在正常推进由任务终态通知驱动，兜底分片只服务 stop 后残留任务搬运与活性兜底） | 实现 + 回归全绿 |
| CR-102 | **前提不复现，不改语义**：交错三轮测量 cap=1024 vs cap=0 差距 ±20% 摆动且大小关系翻转；插桩证明独立任务负载下 find_if 扫描深度恒 1，单次 trim O(1)。线性扫描仅在旧终态有未决 dependent 时出现，属精确保留契约的组成部分。曾试高水位摊还，被 retention 精确上界契约测试（test_executor_task_graph 两用例）否决后回退，结论写入 trim 注释 | 复验同参 Release 5 轮中位：当前 +6.4% vs master +8.5%（均 <10% 噪声阈，无每万块增长趋势）——**master 上 O(capacity) 成本也不存在，前提推翻（同 CR-051/CR-104 先例）** |
| CR-110 | submit/submit_priority 的 promise 与就绪标志合并为单次堆分配 CompletionState（旧 make_shared×2 + 双控制块捕获），两模板内引用别名保持语义不变 | 实现 + 回归全绿 |
| CR-112 | task_dispatcher 三处 `unique_ptr<shared_lock>` → `optional<shared_lock>`（每次 dispatch 免一次堆分配）；dispatch_batch 早退注释与代码不符处修正 | 实现 + 回归全绿 |
| CR-121 | `begin_write`/`begin_batch_write` 的 before_publish_hook 访问加 `before_publish_hook_active_` 原子标志快速路径——常态（无 hook）零 shared_ptr 原子读（旧 libstdc++ 该操作走内部锁池）；hook 激活对在途推送保持最终可见语义 | 实现 + 回归全绿 |
| CR-122 | `push_tasks_batch` 临时 vector → thread_local 便签（enter_push 是多生产者 CAS 门非互斥，成员缓冲会被并发生产者踩踏，故 thread_local） | 实现 + 回归全绿 |
| CR-151 | 参数缓存改 O(1) 侵入式 LRU（list + map<key, iterator>）：命中/更新 splice 队头，淘汰取队尾；**更新已有 key 不再触发淘汰**（旧"先淘汰后插入"会误逐热点） | 实现 + 回归全绿 |
| CR-135 | **文档化不改行为**：1kHz 分片轮询是刻意的时延/可见性权衡（无 cv 设计下调度变更可见性上界=分片）；cv 化被 gcc-11 TSAN 不拦截 pthread_cond_clockwait（PR101978，CI TSAN 正是 gcc-11）阻断；wake_at（heap 顶）非死代码（子分片精度依赖）。结论写入 timer.hpp 注释 | 复核结论与审查主张相反 |
| CR-111 | **缓议（记录）**：LockFreeWorkerQueue::push 每任务 new Task——指针稳定性设计（队列存 uintptr_t、Task 持 atomic 不可移动）使免分配需要带 ABA 防护的有界空闲链表真重设计，风险/收益不匹配本轮范围；LFTE 主路径已有 wrapper 池，此项仅影响 ThreadPool 的 lockfree worker-queue 变体 | 记录缓议理由 |

回归：162/162 全绿；ASAN 27 目标子集 + TSAN 22 项抽查 + lockfree 变体 145 测试全绿，零新增报告。

复验修正记录（2026-10-01）：(1) 本机 build/ 无 CMAKE_BUILD_TYPE（O0 库），旧基准以 -O2 TU 链 O0 库构成混合口径——"基线 65-70%"为该口径产物，同参 Release 真实基线约 +20%；后续性能结论一律以同参构建为准。(2) CR-108 的"有等待者收窄"预期有误：门控机制只消除无等待者通知，有等待者场景不受益也已达成不成（复验实证 83.6% vs 84.5%），机制价值在无等待者吞吐与调度噪声消除。

## 定时器事件驱动唤醒实施记录（2026-10-01，CR-135 后续，方案 A）

Phase 4 曾将 CR-135 按"文档化"处理（cv 化被 gcc-11 TSAN 缺口阻断）。经量化讨论（改/不改收益实测：恒定 ~0.7% 单核 vs 精度收益仅 ~0.1ms）后确认收益主体在能耗，且 TSAN 约束存在干净绕法，按方案 A 实施：

- **机制**：`schedule_once` / `schedule_periodic` / `reschedule` / `stop` 四个变更点持锁 bump `schedule_epoch_` 并 notify（`request_cancel` 不 notify——取消只让睡眠目标变 stale，线程醒来重算即可，lazy-deletion 契约不变）；调度线程 `wait_for(duration, pred)` 精确睡到 heap 顶 deadline（堆空 kIdleWaitMs=100ms 兜底，作为漏通知的爆炸半径上界）。
- **TSAN 约束的绕法**：gcc-11 libtsan 未拦截的是 `wait_until(steady_clock)` → `pthread_cond_clockwait` 映射；`wait_for(duration)` 走 `wait_until(system_clock)` → `pthread_cond_timedwait`，TSAN 正常拦截。代价：实时钟步进（NTP）可使单次睡眠偏早/偏晚一个步进量，循环顶部用 steady 重算一步内自愈。
- **CI 门禁强化**：CI TSAN job（g++-11，即约束工具链）的构建与运行清单补入 `test_timer_handle`（此前只有 test_timer_thread_lifecycle_race），本改动在目标工具链上的验证由该 job 承担。
- **开发中抓出并修正的自伤**：初版把 `wait_for` 放在与 due 处理相同的锁作用域内——已弹出的到期闭包被拖到下一个期限才派发（`RescheduleChangesNextExpiryOnly` 5/5 确定性失败于 81ms，插桩实证 timer 侧 30.0ms 正确弹出、闭包 80ms 才执行）。重构为"有到期项立即派发，无到期项才等待"。
- **量化收益（对照改造前，同 O2 构建）**：等待 CPU **0.7-1.0% 单核（无条件 1kHz，含零定时器）→ 0.024%（≈30 倍）**；到期抖动 p50 0.16ms → 0.16ms（睡眠过冲主导，持平）；新注册更早 deadline 感知延迟 p50 0.08 → 0.16ms（同过冲地板，持平）；注册路径 +notify 成本 ~20-60ns/op。
- 定位：kIdleWaitMs=100ms 的语义从"死常量"恢复为真实的空闲兜底上界；kWakeSlice 删除。

### 复验修正（同日，独立复验两项发现）

1. **gcc-11 TSAN 论断错误（复验源码级证伪）**：初版声称 `wait_for(duration)` 走 `pthread_cond_timedwait` 可绕开 clockwait 缺口——实际 GCC ≥ 10 的 libstdc++ 把 `wait_for(duration)` 与 `wait_until(steady_clock)` **都**映射到 `pthread_cond_clockwait`（"wait_for→system_clock" 是 GCC 9 及更早的行为），该绕法在 CI 的 gcc-11 上无效。已改为**显式 `wait_until(lock, system_clock::now() + slice, pred)`**——system_clock 重载在所有 libstdc++ 版本均走 `pthread_cond_timedwait`；实时钟步进暴露已在注释中分析（循环顶部 steady 重算自愈）。CI TSAN job 已补入 test_timer_handle 作为 gcc-11 权威门禁。
2. **periodic 漂移根因与修复**：初版实测周期抖动 826-1059µs avg（master 569-596µs），劣化 ~+45-78%。根因：周期分支 `next_execute_time = now + P` 以实际唤醒时刻锚定，每次唤醒过冲（~0.15ms）逐周期累积为漂移——旧 1ms 轮询的网格量化恰好掩盖了这一缺陷。修复为**理想网格锚定**：`next = entry.deadline + P`，已过期的周期按"错过不追补"跳到未来第一个格点（与 CR-051 结论的期限锚定语义一致）。实测周期抖动降到 **-47 ~ +24µs avg（p95 ≤ 55µs）**，比 master 好 20-50 倍。
