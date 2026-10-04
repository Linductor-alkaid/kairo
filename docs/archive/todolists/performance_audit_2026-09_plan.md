# 性能审查收敛计划（2026-09 全库审查）

本计划以 2026-09-08 对 master `e0924c4` 的四路并行审查为输入（核心线程池热路径 /
comm·timer·取消原语 / GPU·监控路径 / 文档同步），把确认属实的问题编号登记为待修复
条目（PA-xx），并给出分阶段收敛顺序。审查方式为逐行核对源码，每条问题均有 file:line
证据；行号为审查时快照，实现阶段需重新核对。

排序依据：**阶段 P1（线程池提交热路径）是单任务延迟与多生产者扩展性的最大瓶颈**，
三条高位问题同源（全局锁 + 持锁谓词 + 每次派发堆分配），一次重构一并解决；阶段 P2
兑现"无锁"组件的对外承诺并消除 RT 优先级反转；阶段 P3/P4 为定时器、comm 原语与
facade/监控的单点修复，可独立小步合入；阶段 P5 为 GPU 路径。文档同步缺口（阶段 D0）
无正确性风险且阻塞用户正确使用 API，先行落地。

### 审查中已核实无问题的部分（不重复立项）

- 无锁队列头尾/统计计数全部 `alignas(64)`；状态机（Free/Reserved/Writing/
  BatchWriting/Published/Cancelled）带绝对位置标签，无经典 ABA；push 退避有界；
  `push_batch_exact` 真单 CAS 全体发布。
- `PriorityScheduler` 四条优先级队列各自持锁（无全局调度器锁）；监控关闭时
  `TaskMonitor::record_*` 经 relaxed atomic 早退，近零开销。
- RT 周期循环无忙等（`sleep_until` + 迟到重相位）；线程工具的亲和性/优先级/mlockall
  均只在启动时设置一次，mlockall 已是引用计数租约。
- `cudaGetDeviceProperties` 只查一次并缓存；CUDA 批提交按 64 任务块摊销队列锁；
  BoundedQueue 节点预分配无消息级分配；取消快路径是单 acquire load；
  `SnapshotStore`/`DoubleBuffer` 的发布交换是单 CAS，不做全量拷贝。

---

## 问题登记总表

严重度：H = 热路径显著退化或对外承诺被违背；M = 负载相关的中等退化；L = 边界或低频。

| 编号 | 严重度 | 摘要 | 位置（2026-09-08 快照） | 阶段 |
| --- | --- | --- | --- | --- |
| PA-1 | H | ✅ 每次提交两获全局 `mutex_` + `notify_all` 惊群 | `thread_pool/thread_pool.cpp:963-983,752-755` | P1 |
| PA-2 | H | ✅ worker 等待谓词持全局锁做 steal/dequeue（含分配+排序） | `thread_pool.cpp:218-241` | P1 |
| PA-3 | H | ✅ `dispatch_batch` 每次派发 4-6 次堆分配，判空前即分配 | `thread_pool/task_dispatcher.hpp:199-219` | P1 |
| PA-4 | H | ✅ Task 从提交到执行复制 4-5 次（每次复制 2 个 std::function） | `priority_scheduler.cpp:8-30`、`task_dispatcher.hpp:21-31`、`worker_local_queue.cpp:28,66-73` | P1 |
| PA-5 | H | ✅ `ObjectPool` 带互斥锁；LockFree 提交/RT 消费串行化，RT 路径有优先级反转 | `src/util/object_pool.hpp:60,80`；`lockfree_task_executor.cpp:108,117,389`；`realtime_thread_executor.cpp:416,548` | P2 |
| PA-6 | H | facade tracked 提交/完成路径 `task_graph_mutex_` + `notify_all` 每任务 3 次 | `executor.cpp:308-427`、`executor.hpp:1837-1849` | P4 |
| PA-7 | H | ✅ `LockFreeWorkerQueue` push 每次 `new Task`，pop/steal/size 共用 `consume_mx_` | `thread_pool/lockfree_worker_queue.hpp:33-134` | P2 |
| PA-8 | M | ✅ LockFree worker 空闲永驻 1µs-sleep 轮询（约 10⁶ syscall/s/核） | `lockfree_task_executor.cpp:408-418` | P2 |
| PA-9 | M | ✅ `std::atomic_load(shared_ptr*)` 每次 worker 迭代/派发/提交（libstdc++ 库级自旋锁） | `thread_pool.cpp:200,227,248,276`、`task_dispatcher.hpp:256` | P1 |
| PA-10 | M | LoadBalancer 写锁每任务一次（所有 worker 串行完成） | `thread_pool.cpp:274-280`、`load_balancer.cpp:87-96` | P4 |
| PA-11 | M | `ready_approx_`/`reserved_approx_` 全局缓存行 RMW（仅服务近似 size） | `util/lockfree_queue.hpp:132-133,543` | P4 |
| PA-12 | M | ThreadPool 统计原子与 LockFree `push_gate_`/计数器未按缓存行隔离 | `thread_pool.hpp:463-468`、`lockfree_task_executor.hpp:257-263` | P4 |
| PA-13 | M | ✅ steal 路径每次分配 2 vector + sort | `thread_pool.cpp:797-829` | P1 |
| PA-14 | M | `try_submit_batch` 全局锁内逐任务调 monitor 回调 | `thread_pool.cpp:1085-1114` | P4 |
| PA-15 | M | facade 每次提交前置 `default_async_mutex_` + `thread_pool_mutex_` 两跳 | `executor_manager.cpp:167-182`、`thread_pool_executor.cpp:141-153` | P4 |
| PA-16 | M | `submit_batch_priority` 逐个循环，批次 API 零摊销；worker 队列 `push_batch` 同样逐项 | `executor.hpp:2532-2534`、`lockfree_worker_queue.hpp:79` | P4 |
| PA-17 | M | ✅ `should_exit` 每迭代锁 mutex + 线性扫（谓词内嵌套） | `thread_pool.cpp:874-878`（调用点 191,222,239,285） | P1 |
| PA-18 | H | timer 线程永久 1kHz 轮询（libtsan workaround 被当成生产设计） | `timer.hpp:736-748` | P3 |
| PA-19 | H | comm 阻塞原语全为无上界 yield 自旋 + 每迭代读时钟（无 futex/cv 驻停） | `comm/phase_gate.hpp:317-337,457-475`、`comm/channel.hpp:37-91`、`comm/snapshot_store.hpp:81-92`、`comm/double_buffer.hpp:221-227` | P3 |
| PA-20 | M | comm 每消息无条件 2 次时钟读；`CommStats` 默认全开（每消息约 6 RMW） | `comm/bounded_queue.hpp:111,145-149`、`comm/types.hpp:118,201-202`、`double_buffer.hpp:24-25`、`task_options.hpp:103-104`、`types.hpp:196-197` | P3 |
| PA-21 | H | `Topic::publish` 每消息全局 mutex + 堆分配订阅快照 | `comm/topic.hpp:232-241` | P3 |
| PA-22 | M | BoundedQueue 队满拒绝路径 O(capacity) CAS 扫描；`depth_` 与生产者计数同缓存行；节点未填充 | `comm/bounded_queue.hpp:374-387,485-488,241-249` | P3 |
| PA-23 | M | 每个可取消提交 4-5 次分配 + 取消 registry 全局 mutex 两趟 | `executor.hpp:1171-1245`、`task_cancellation.hpp:317-358` | P3 |
| PA-24 | M | 周期 tick 每跳分配 state/闭包/字符串；`release_tick` 持锁 O(n) 扫描 | `executor.cpp:764-834`、`timer.hpp:531-574` | P3 |
| PA-25 | H | OpenCL `memory_mutex_` 持锁跨阻塞拷贝/`clFinish`（嵌套队列锁） | `gpu/opencl_executor.cpp:485-504,521-540,557-580` | P5 |
| PA-26 | H | CUDA 每 stream mutex 持锁跨 kernel 执行与 `cudaStreamSynchronize` | `gpu/cuda_executor.cpp:1451-1523` | P5 |
| PA-27 | H | loader 每次 API 调用锁全局 mutex + 拷贝约 40 指针函数表 + lease 引用计数 | `gpu/cuda_loader.cpp:112-122`、`gpu/opencl_loader.cpp:97-102` | P5 |
| PA-28 | M | 每次 API 调用 `cudaSetDevice`；`get_status()` 改变调用线程当前设备 | `gpu/cuda_executor.cpp:295-305`（调用点 806,1337 等） | P5 |
| PA-29 | M | `copy_from_peer` 每次重新查询/使能 P2P | `gpu/cuda_executor.cpp:1024,1044` | P5 |
| PA-30 | M | 三个 GPU optimizer 是死代码（仅测试引用），宣传优化收益为零 | `gpu/transfer_optimizer.cpp`、`kernel_launch_optimizer.cpp`、`task_scheduler_optimizer.cpp` | P5 |
| PA-31 | M | 全库无 pinned host memory，async H2D/D2H 对 pageable 内存退化为同步拷贝 | `gpu/cuda_loader.cpp:237-288` | P5 |
| PA-32 | M | 按名单查询 GPU 状态会构建全部执行器状态（各含驱动查询+持锁汇总） | `monitor/statistics_collector.cpp:36-38`、`executor_manager.cpp:505-523`、`cuda_executor.cpp:1337-1376` | P5 |
| PA-33 | M | OpenCL 每提交堆分配闭包；单 worker 线程串行执行所有队列 | `gpu/opencl_executor.cpp:806,884-916` | P5 |
| PA-34 | M | `GpuMemoryManager::free` 每次全量排序合并；allocate O(n) 首次适配 | `gpu/gpu_memory_manager.cpp:72-73,128-129` | P5 |
| PA-35 | H | TaskMonitor 默认开启，每任务 2-3 事件各取全局 mutex；表满后仍锁 mutex 只为 ++dropped | `monitor/task_monitor.cpp:18-97`、`config.hpp:78`、`task_monitor.hpp:114-118` | P4 |
| PA-36 | L | 杂项：完成路径双 `notify_all`；`push_realtime_task` 成功路径也先拷全量 status；task id 字符串超 SSO；`blocking_io` 持锁调用户 wakeup 回调；snapshot formatter 每次 ostringstream；async 默认流落到 legacy stream | `thread_pool.cpp:320-322,413`、`executor.cpp:1235-1236`、`task/task.cpp:38-41`、`blocking_io_executor.cpp:82-103`、`executor_snapshot_formatter.cpp:303-307`、`cuda_executor.cpp:825,883,942` | P3/P5 顺带 |

---

## 阶段 D0：文档同步（先行）✅ 已完成

输入：审查的文档审计结论。全部条目随本计划同批落地：

- [x] `CHANGELOG.md` `[Unreleased]` 补 2026-08-30 之后约 16 个提交：S2 串行上下文、
  总量有界 admission、P-001/P-002 停机准入门闩、P-003 池容量、P-004 mlockall 租约、
  P-008 Windows 处理器组、P-006/P-007 相关测试与修复。
- [x] `docs/design/lockfree_user_api.md` 修正"集成到 Executor Facade"小节：删除不存在的
  `submit_lockfree`、更正 `register_lockfree_executor` 真实签名（接收
  `std::unique_ptr<LockFreeTaskExecutor>`）、修正构造函数三参数签名、标注
  `util/lockfree_queue.hpp` 为内部头文件。
- [x] `docs/API.md` 新增 §5.9 Facade 注册与生命周期（`register_lockfree_executor` /
  `start_lockfree_executor` / `stop_lockfree_executor` / `get_lockfree_executor_names`），
  §3.7 交叉引用；§3.10 补 `get_max_in_flight_tasks()`；§7.3 修正 `config.hpp:67` →
  `config.hpp:90`；§8.3 移除悬空的"详见 P-001 commit"引用。
- [x] `docs/design/executor.md` §5 线程池内部结构更新为当前实现（PriorityScheduler
  细粒度锁 + worker 本地队列 + TaskDispatcher），修正线程数默认值描述。
- [x] `docs/design/android_port.md` 状态横幅更新为一期已交付，阻塞点标注为历史。
- [x] `docs/todolists/todolist.md` 阶段 16/17/18 已完成项勾选。
- [x] website 中文 Facade 覆盖索引补取消/定时句柄/串行上下文/admission/lockfree 注册
  行；英文站新增本地 API 参考页（与中文页同源的仓库 `docs/API.md` 指针）。

### 验收

- [x] 文档一致性守护测试（`test_api_doc_*` 5 项）全部通过；website `docs:check` 通过。
- [x] 按审查清单逐条复核，无残留虚构 API 或过时签名。

---

## 阶段 P1：线程池提交热路径三连 + Task 复制链（PA-1/2/3/4、PA-9/13/17）✅ 已完成

落地实现（2026-09-09，与下述基准数据同批）：worker 驻停从「`mutex_` +
`condition_` 重谓词」改为 32 位驻停代次计数 + C++20 `std::atomic::wait`
（Linux futex 直达路径；**必须 32 位**——libstdc++ 仅对 4 字节标量走
futex，64 位会落入内部 mutex+condvar waiter 池）。实现要点：

- 代次在完整空扫描【之前】采样，`wait` 的原子 check-and-block 保证
  两个方向（bump 在采样前/后）均无丢失唤醒窗口，内存序
  acquire/release 已足够（推导见 `thread_pool.cpp` worker_thread 注释）。
- stop_ 下的 worker 退出加「退出守门」：持 `dispatcher_mutex_` 复核代次
  未变才退出，排除任务处于 dispatch 搬运途中（已离开 scheduler、未落地
  local）被永久滞留的窗口——TSAN 放大下该窗口曾稳定复现
  （`test_thread_pool` delayed-worker shutdown 用例）。
- notify_one 需要配套「接力唤醒」：worker 拿到任务时再唤醒一个同伴，
  并行度按 1→2→4… 指数恢复；否则单 notify_one 会把多 worker 执行
  串行化（满载下唤醒延迟逐任务叠加，曾表现为满载挂死级慢）。
- 提交路径不再内联 `dispatch(1)`：满载下内联派发与 worker 的自由扫描
  互相抢 dispatcher/scheduler/queue 锁，锁交接的调度延迟逐任务叠加。

### 任务

- [x] PA-2：等待谓词瘦身为单原子"有活可干"检查（代次计数或非空标志），把
  pop/steal/dequeue 全部移出谓词与 `mutex_` 临界区。
- [x] PA-1：提交路径消除双重加锁——解锁后再 notify；单任务 `notify_one`，仅批次用
  `notify_all`；评估按优先级分队列 condvar。
- [x] PA-3：`dispatch_batch` 先查 `scheduler_.size()` 再分配；锁包装与批缓冲区复用
  （成员或 thread_local 便签），`by_worker` 提升为可复用便签。
- [x] PA-4：Task 全链路改 `unique_ptr<Task>` 移动传递（enqueue → dequeue → dispatch →
  本地队列 → pop），消除逐跳 std::function/string/vector 复制。
- [x] PA-9：`local_queues_` 改 C++20 `std::atomic<std::shared_ptr>` 或发布不可变裸指针
  + epoch，消除 libstdc++ `atomic_load(shared_ptr*)` 的库级自旋锁。
- [x] PA-13：steal 选 victim 改固定大小栈数组 O(n) 扫描（去掉 vector 分配 + sort）。
- [x] PA-17：`should_exit` 快路径改原子标志（空集早退），仅在缩容窗口走锁。

### 验收

- [x] 多生产者（8 线程）提交吞吐与单任务延迟基准对比基线（`benchmark_baseline`、
  `benchmark_batch_scales`）有可测量改善；无回归。
  14 核桌面（Intel Ultra 5 225H，gcc 13 Release，3 次取中位）：
  - `benchmark_baseline`：提交吞吐 125.5k → 565.4k tasks/s（4.50x），
    e2e 120.2k → 416.7k（3.47x）。
  - `benchmark_batch_submit_concurrent`：loop submit 2.2x~4.7x
    （2T 138.9k→333.3k、8T 92.6k→434.8k、16T 85.5k→312.5k、
    32T 86.8k→344.3k），多生产者扩展曲线从随线程数恶化变为平坦；
    batch 路径 2.2x~4.2x。
  - `benchmark_thread_pool_hotpath`（本阶段新增，直连 ThreadPool）：
    1 生产者 28.1k → 380.3k（13.5x）；唤醒 p50 19.2µs → 18.3µs，
    4T+ 唤醒 p99 185.6µs → 37.3µs、336.7µs → 103.7µs。
  - 口径说明：`benchmark_baseline` 的 round_trip_latency p99 从 0.37µs
    升至 ~10.9µs——基线提交慢 4.5 倍，任务在 get 前早已执行完，
    旧值接近零是排队被提交耗时掩盖的假象；新值是真实的「尾部任务
    等执行」延迟，绝对值仍在 10µs 量级的正常池尾延迟范围。
- [x] 既有全部 CTest 通过；TSAN 全量无新增报告（gcc-11 libtsan clockwait 误报按既有
  清单甄别）。本地 gcc-13 全量 117/117、TSAN 专项 14/14、lockfree 双模式
  117/117、12 核满载下 `test_serial_context_stress` 3/3 通过。
- [x] worker 等待语义不变：任务到达后有限时间内被唤醒（唤醒风暴回归测试）。

### 测试

- [x] 新增多生产者争用基准与唤醒延迟分布测试，注册 CTest
  （`benchmark_thread_pool_hotpath`：1~16 生产者争用吞吐 + 间隔采样式
  wake-to-exec 延迟分布 p50/p95/p99/max，文本与 JSON 双输出）。


---

## 阶段 P2：无锁组件兑现与 RT 优先级反转（PA-5/7/8）✅ 已完成

落地实现（2026-09-09）：

- **PA-5**：`ObjectPool` 改 tagged Treiber 索引 freelist——head 为 64 位
  `(tag:32 | index:32)` 单字，release 先写 `node->next` 再 CAS head（tag+1），
  acquire 失败指数 PAUSE 退避后**重读期望值**再重试（见下述陷阱）；
  `release_bulk` 把整链一次 splice 回池（消费者侧 head_ 往返每批一次）。
  ABA 由 tag 封闭（同一 head 值经 release 后不可复现）。
  双重释放检测从 `in_free_list` 布尔改为 per-node `state` CAS（1→0），
  语义与旧 mutex 版一致（重取后的二次释放同为盲区）。容量上限
  `kNilIndex-1`（构造期拒绝，含 0 容量原校验）。提交路径（LockFree
  push_task/push_tasks_batch）与 RT 消费路径（process_tasks 每任务
  release）不再共享任何 mutex，RT 优先级反转面消除。实现期深挖出两个
  并发陷阱（修复均有注释与压力测试守护）：其一，生产者侧「条件 load
  驻停标志」的唤醒会被 x86 StoreLoad 重排打穿（满载实测挂死），驻停
  标志必须并入被生产者无条件 RMW 的同一字；其二，CAS 循环带退避前的
  陈旧期望值重试在高争用下确定性失败，偶发参与者（RT 线程）曾饿死
  20 秒+，每轮重试必须重读期望值。
- **PA-7**：`LockFreeQueue` 消费侧 MPMC 化——`pop_impl` 从「load/store
  dequeue_pos_」改为「验证前沿连续 Published run → 单 CAS 认领 [pos,
  pos+run) → 独占消费」，Published 槽位不回退的性质保证验证跨 CAS 有效；
  Reserved/Cancelled 前沿的维护性跳过与 BatchWriting scan-ahead 取消逻辑
  原样保留（认领者负责清理，CAS 失败方重载前沿）。单消费者批量路径
  仍为每批一次 CAS。`LockFreeWorkerQueue` 随之删除 `consume_mx_` 与共享
  `steal_buffer_`（steal=pop，同 FIFO 最老端——测试钉死 steal 必须取最老
  任务，Chase-Lev 的另一端窃取与该契约冲突，故未采用 deque 方案）；
  `size()` 改用队列无锁近似值。
- **PA-8**：LockFree worker 空闲退避改为 PAUSE(32) → yield(64) →
  10µs-sleep ×50（缓冲带）→ futex 驻停。驻停编码进 32 位 `wake_seq_`
  单字（bit0=驻停位，bit1..30=唤醒计数）：生产者 push 成功后**无条件**
  `fetch_add(2)`（RMW 全序排空 store buffer——条件 load 检查会被
  StoreLoad 重排打穿，曾实测挂死），返回值带驻停位才 `notify_one`，
  忙碌路径零 syscall。缓冲带（≈500µs）钉住「数百 µs 间隔零星任务」的
  尾延迟：超载机器上刚被 futex 唤醒的线程排在持续 runnable 的轮询线程
  之后，无缓冲带时该负载 p99 从 ~60µs 恶化到 ~150µs+（对照实验实锤，
  100µs 级承诺见 benchmark_lockfree_task_executor）。长期空闲（≥~0.6ms）
  才进入零 CPU 驻停；实测空闲 500ms 窗口进程 CPU 0.0ms（旧轮询为
  10⁶ syscall/s/核）。停滞生产者恢复（scan-ahead 取消）不经过 push 成功
  路径，由 before_publish hook 的 trampoline 在进入停滞窗口前定向唤醒；
  失败的提交（取消后槽位需消费者物理推进）同样定向唤醒。

### 任务

- [x] PA-5：`ObjectPool` 改无锁索引 freelist（连续数组 + index/tag 防 ABA）；
  同批消除 RT 线程经 `release` 的 mutex（PA-5 的 RT 优先级反转面）。
- [x] PA-7：pop/steal 不再共用 `consume_mx_`（消费侧 MPMC 化）；
  `size()` 改用队列已有的无锁近似值。Chase-Lev 方案因与「steal 取最老
  任务 + prefetch 对 pop/size 可见」的既定测试契约冲突而未采用（见上）。
- [x] PA-8：LockFree worker 空闲退避升级为缓冲带 + futex 驻停（生产者在
  push 路径 RMW 定向唤醒）。

### 验收

- [x] `docs/API.md` §5.4 的性能承诺与实现一致：MPSC 提交路径除队列自身原子操作外无
  全局锁（以 TSAN/基准佐证）。TSAN 对池/队列/驻停/窃取全部改动零告警；
  提交路径 = admission RMW + 无锁池 acquire + 无锁队列 push + 唤醒 RMW，
  无任何 mutex。
- [x] RT 周期抖动基准（`docs/optimization/realtime_precision_*.json` 口径）不劣化；
  消除 RT 线程等待 pool mutex 的窗口。1ms 周期 jitter p50 由 ~78.7µs 降至
  ~16.8µs（gcc-13 Release 本机 3 次取中位；n=20/周期的该基准方差大，
  逐轮 p50 为 24.7/21.6/140.5µs vs 基线 78.7/74.5/2280µs）。
- [x] 既有 lockfree/realtime 全部测试通过（默认模式 140 项全量、lockfree 双模式
  140 项全量、TSAN CI 同款 14/14 + 扩展并发集 21/21，均零 ThreadSanitizer
  告警；满载 ctest -j4 下仅既有的 benchmark_lockfree_task_executor P99
  抖动与 master 同负载同频率复现，对照实验确认非本批引入）。

### 基准对比（14 核 Ultra 5 225H，gcc-13 Release，3 次取中位）

| 指标 | 基线 | 本批 | 变化 |
| --- | --- | --- | --- |
| mpsc 吞吐 1 生产者 | 874k/s | 1.62M/s | +86% |
| mpsc 吞吐 2 生产者 | 2.16M/s | 1.65M/s | **-24%（已知取舍，见下）** |
| mpsc 吞吐 4 生产者 | 2.21M/s | 1.69M/s | **-23%（同上）** |
| mpsc 吞吐 8 生产者 | 732k/s | 2.85M/s | +289% |
| mpsc 吞吐 16 生产者 | 380k/s | 2.21M/s | +482% |
| mpsc 吞吐 32 生产者 | 397k/s | 1.98M/s | +399% |
| lockfree 执行器提交延迟 avg/p50 | 1217/1069ns | 994/1107ns | -18%/+4% |
| RT 执行器提交延迟 | 274ns | 215ns | -21% |
| RT 1ms 周期 jitter p50 | 78.7µs | 16.8µs | -79% |
| 空闲 lockfree 执行器 CPU | ~10⁶ syscall/s | 0.0ms/500ms | ≈清零 |
| lockfree 模式 thread pool 提交吞吐 1P | 470k/s | 500k/s | +6% |

已知取舍：2-4 生产者的 mpsc 吞吐 -23%~-24%。归因实验（mutex 池 + 新队列/驻停
= 恢复基线）确认瓶颈是 Treiber 单链 head_ 在低生产者数下的缓存行往返（mutex 的
自适应自旋在该区间恰好占优）；实现中已做三项缓解（消费者 release_bulk 整链
splice、CAS 失败指数 PAUSE 退避、每轮重试重读期望值防确定性饿死）。彻底恢复
需要 per-thread 旁路缓存设计（与池容量语义的精确断言测试有冲突面），登记为
后续独立条目，不在 P2 范围内。

### 测试

- [x] 新增 RT 消费者与多生产者并发 acquire/release 的压力测试
  （`test_object_pool_lockfree_stress`：6 生产者锤击 + RT 线程按 100µs 周期
  采样 acquire→release 延迟分布；per-object CAS 标记断言无双重发放；
  空闲 500ms 进程 CPU 上界守护（实测 0.0ms）；驻停唤醒分布
  （p50 ~12µs）与丢失唤醒守护）。

---

## 阶段 P3：定时器与 comm 原语（PA-18/19/20/21/22/23/24）

### 任务

- [ ] PA-18：timer 等待默认改 condvar/timerfd 驻停，仅在 `__SANITIZE_THREAD__` 下保留
  1ms 分片轮询（gcc-11 libtsan `pthread_cond_clockwait` workaround）。
- [ ] PA-19：comm 阻塞 wait 族（phase_gate/channel/snapshot_store/double_buffer）加
  指数退避收敛到 `sleep_for(剩余时间分数)`，时钟检查按 N 次自旋一次摊销；评估
  futex 驻停路径。
- [ ] PA-21：`Topic::publish` 改 RCU/COW 订阅快照（subscribe/unsubscribe 时发布
  `shared_ptr<const vector>`），发布路径只剩一次原子 load + 扇出。
- [ ] PA-22：BoundedQueue 拒绝路径先查 `depth_` 快速失败；`depth_` 独立缓存行；节点
  填充至缓存行边界。
- [ ] PA-20：时钟读与 CommStats 按 `enable_stats` 门控（含默认成员初始化器中的隐藏
  读钟）；评估 realtime channel 统计默认关闭。
- [ ] PA-23：取消状态对象池化；registry 分片锁或仅在需要取消能力时注册。
- [ ] PA-24：tick 路径用户 callable 移动/shared 捕获一次；`release_tick` O(1) 句柄
  移除；`report_tick_success`/`release_tick` 合并为单次加锁。

### 验收

- [ ] 空闲 executor（零定时器、零通信）CPU 占用近零（对比当前 1kHz 轮询 + 自旋）。
- [ ] 1s 超时等待不再烧满一核（阻塞 wait 的 CPU 时间回归测试）。
- [ ] timer 精度基准（5ms 延迟平均误差约 0.9ms 的既有口径）不劣化。

### 测试

- [ ] 新增空闲 CPU 占用守护测试（采样窗口内线程 CPU 时间上界）与 topic 高频发布
  基准。

---

## 阶段 P4：facade 与监控串行化（PA-6/10/11/12/14/15/16/35）

### 任务

- [ ] PA-6：task graph 分片（按 handle hash 分桶锁）或原子依赖计数 + 定向唤醒，
  消除全局 `task_graph_mutex_` + `notify_all`。
  （2026-09-22：深化方案转入主清单阶段 21——调度侧唤醒、依赖驱动调度，
  依赖未就绪不入队，设计文档 docs/design/dependency_driven_scheduling.md；
  2026-09-28 PR-3 落地：`task_graph_cv_` 与全部 `notify_all` 退役，定向出队
  替代惊群。验收基准 `tests/benchmark_task_graph_paths`：tracked 提交吞吐
  1/2/4/8 生产者 30.3k/74.3k/65.9k/45.1k ops/s；parked fan-out 256 依赖
  28.0µs/依赖、64 依赖 16.1µs/依赖（定向唤醒，无惊群）；256 链 22.4µs/跳。
  每提交图锁获取 2 次（登记 + park 决策各一次，completion 1 次），为常数；
  原"分桶锁"备选不再需要——无 worker 持锁等待，图锁持有时间缩为纯簿记。
  分片如仍有收益由后续基准数据决定，本项主体关闭待该 PR 合入后勾选）
- [ ] PA-15：默认执行器引用改 `std::atomic<std::shared_ptr>` 快照，消除每次提交的
  `default_async_mutex_`/`thread_pool_mutex_` 两跳。
- [ ] PA-35：TaskMonitor 先查容量/采样再锁；dropped 计数改原子；in-flight 表分片或
  按小整型 type id 键化。
  （2026-09-22 进度注记：queued/pending 先查再锁已落地，见 task_monitor.cpp:11-22；
  剩余 dropped 计数原子化与 in-flight 表分片/键化未做）
- [ ] PA-10：LoadBalancer 负载字段改 per-worker 填充原子，去掉每任务写锁。
- [x] PA-14：`try_submit_batch` 的 monitor 事件移出全局锁后补记。
  （已随 P1 顺带落地：monitor id 锁前收集、record_task_queued 锁外补记，
  见 thread_pool.cpp:1142 注释；2026-09-22 回填）
- [ ] PA-16：`submit_batch_priority` 路由到带优先级参数的 `try_submit_batch`；worker
  队列 `push_batch` 改 `push_batch_exact` 语义。
- [ ] PA-11/12：近似计数器退出数据路径（由 `enqueue_pos_ - dequeue_pos_` 推导）；
  统计原子与 `push_gate_` 按缓存行隔离。

### 验收

- [ ] tracked 提交/完成路径在多 worker 下的争用基准改善；facade 层每提交锁获取次数
  降到常数 1 以内（不含池自身）。
- [ ] 监控默认开启时的提交吞吐劣化有界并记录数据；关闭时仍近零开销。

### 测试

- [ ] 新增 facade 提交并发基准与监控开销对比测试。

---

## 阶段 P5：GPU 路径（PA-25~34、PA-36 部分）

### 任务

- [ ] PA-25/26：OpenCL `memory_mutex_` 与 CUDA stream mutex 收窄到"查表/取裸句柄"，
  阻塞调用（`clEnqueueWriteBuffer(blocking)`、`clFinish`、`cudaStreamSynchronize`）
  在锁外执行，用事件等待替代持锁同步。
- [ ] PA-27：执行器内缓存函数表（含 lease）一次获取，loader mutex 只在 load/unload
  路径出现。
- [ ] PA-28：`cudaSetDevice` 每线程一次（thread_local 已置位检查）；`get_status()`
  不再改变调用线程设备（保存/恢复或只读路径）。
- [ ] PA-29：P2P enable 结果按 {(dst,src)} 缓存，命中即跳过驱动查询。
- [ ] PA-31：加载 `cudaHostAlloc`/`cudaMallocHost`/`cudaHostRegister` 符号，提供
  pinned host 分配器或内部 staging ring，async 拷贝路由经 pinned 内存。
- [ ] PA-30：三个 optimizer 决策——接入执行器路径（transfer/kernel_launch）或删除，
  二选一并记录；保留则修 LRU O(n) 扫描与 O(V·E) 不动点。
- [ ] PA-32：单名单 GPU 状态查询走单执行器路径；`memory_used_bytes` 改原子累计；
  去重 `get_last_error()` 双拷贝。
- [ ] PA-33：OpenCL 任务体直接存 {kernel, config, promise}（对齐 CUDA 结构）；评估
  N 队列 N worker。
- [ ] PA-34：free-block 管理改有序 set 或 size-bucket freelist，免除每次 free 全排序。
- [ ] PA-36（GPU 部分）：async 且 `stream_id==0` 的拷贝路由到非阻塞默认流池。

### 验收

- [ ] GPU 测试套件（含 P-007 内存校验回归）全绿；无 GPU 环境的 CI 路径不受影响。
- [ ] 并发 submit + 拷贝混合负载下锁等待时间对比基线改善（有 GPU 环境记录数据；
  无环境则以 stub 测试佐证锁窗口消除）。

---

## 建议合并顺序

1. D0 文档同步（本批）。
2. P1 线程池热路径（最大收益，独立 PR，需完整基准 + TSAN）。
3. P2 无锁兑现（RT 语义敏感，需 realtime 精度回归）。
4. P3/P4 单点修复按条目独立小步合入（PA-18、PA-35、PA-27 可先行——低成本高回报）。
5. P5 GPU（有真机环境时验证；stub 路径先行）。

每阶段独立可回滚提交合入；基准与 TSAN 数据随 PR 附上。任何阶段发现语义风险
（如唤醒语义、恰好一次结算不变式）先停下评审，不以性能名义放宽正确性约束。

## 风险与待决项

- [x] P1 谓词瘦身后的唤醒及时性：代次计数方案的内存序（acquire/release 是否足够）
  与虚假唤醒窗口需要设计说明。→ 已随 P1 落地：acquire/release 足够，推导与
  32 位 futex 前置条件、接力唤醒、退出守门的设计说明见阶段 P1 小节与
  `thread_pool.cpp` 注释。
- [x] P2 无锁池的 tag 宽度与容量上限（注释中旧实现移除的原因需考古确认）。
  → 已考古：dc04c73（2026-04-09）移除的是**无 tag 的裸指针 CAS freelist**，
  其 ABA 无法用载荷内 next 指针封闭；本批实现的 `(tag:32 | index:32)` 单字
  head 中 tag 每次 release +1，同一 head 值经 release 后不可复现，ABA 窗口
  封闭（tag 环绕需同一竞争窗口内 2³² 次 release，实践不可达）。容量上限
  2³²-2（32 位 index 留 nil 哨兵），构造期拒绝越界。
- [ ] P3 timer 驻停与 libtsan workaround 的条件编译边界（CI 的 TSAN 任务必须仍走
  分片轮询路径）。
- [ ] P4 task graph 分片与既有"计数先于 future"不变式（PR #177）的交互评审。
  （2026-09-22：评审入口转入主清单阶段 21 的 dependency-driven scheduling 设计文档；
  parked 任务沿用 admission 槽位与 completion sink 的既有释放时序，评审随设计文档进行）
- [ ] P5 optimizer 去留需要用户反馈（是否有下游依赖其 advisory API）。
