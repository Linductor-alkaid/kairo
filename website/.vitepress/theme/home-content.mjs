// 首页文案与链接的唯一来源；scripts/check-site.mjs 会校验其中所有站内路由。

const snippets = {
  async: `auto& ex = kairo::Executor::instance();
auto answer = ex.submit_auto([] { return 42; });
std::cout << answer.get() << '\\n';`,
  orchestrate: `auto urgent = ex.submit_priority(3, [] { send_stop(); });
auto load = ex.submit_with_handle([] { return load_map(); });
auto plan = ex.submit_after(load.handle, [] { return plan_route(); });`,
  scheduling: `auto task = kairo::task([] { return infer(); })
    .qos(kairo::QosClass::Interactive)
    .deadline(std::chrono::steady_clock::now() + std::chrono::seconds(1));
auto result = ex.submit_auto(task);`,
  realtime: `kairo::TaskOptions rt;
rt.intent = kairo::ExecutionIntent::RealtimeQueue;
rt.preferred_executor = "control";
auto admission = ex.dispatch_auto(rt, [] { apply_control(); });
if (!admission.accepted) { /* admission.decision / status */ }`,
  worker: `kairo::BlockingIoConfig config;
config.thread_name = "can_rx";
auto worker = ex.start_worker(kairo::BlockingWorkerSpec{
    "can_rx", config, std::make_unique<CanReader>()});
worker.stop();  // request stop, wakeup(), join`,
  gpu: `auto done = ex.submit_auto(
    kairo::cpu_gpu_task(
        [data] { run_cpu(*data); },
        [data](void* stream) { run_gpu(stream, *data); })
        .data_size(bytes)
        .fallback(kairo::FallbackPolicy::AllowCpu));`
}

export const homeContent = {
  zh: {
    hero: {
      title: '面向 C++20 应用的进程内并发执行基础设施',
      lead: '线程池、低延迟队列、周期实时线程、长期阻塞 I/O 和可选 GPU，由同一个 Executor 管理。从 submit_auto() 开始，只在约束明确时才进入专用路径。',
      primary: { text: '十分钟跑通第一个任务', link: '/zh/quick-start/build' },
      secondary: { text: 'Kairo 是什么', link: '/zh/getting-started/what-is-kairo' },
      facts: [
        ['版本', 'v0.7.0'],
        ['语言', 'C++20'],
        ['构建', 'CMake 3.16+'],
        ['平台', 'Linux、Windows、Android'],
        ['许可', 'MIT']
      ],
      imageAlt: 'Kairo 的形象角色回眸：深蓝长发夹杂蓝色与金色挑染，头戴金色星形发饰，身穿白色外套。'
    },
    lanes: {
      title: '按工作语义选择入口',
      intro: '先回答一个问题：调用方需要观察任务完成、队列准入，还是长期 worker 的生命周期？不同模型不会被伪装成同一种完成契约。',
      apiLabel: '入口',
      returnsLabel: '调用方得到',
      more: '阅读这一节',
      items: [
        {
          id: 'async',
          sticker: 'peace',
          name: '普通的一次性计算',
          api: 'submit_auto(lambda)',
          returns: 'std::future<T>：返回值，或在 get() 时重新抛出的异常',
          note: '默认路径，不需要先理解线程池或调度器。Auto 不会为了性能偷偷改投无锁、实时或 GPU 后端。',
          code: snippets.async,
          link: '/zh/quick-start/first-task'
        },
        {
          id: 'orchestrate',
          sticker: 'tablet',
          name: '优先级、延迟、批量与依赖',
          api: 'submit_priority / submit_delayed / submit_batch / submit_after',
          returns: 'future、任务 ID，或可继续串联的 TaskHandle',
          note: '优先级只影响排队顺序，不抢占已运行的任务；依赖失败时后续任务默认不执行，future 带回原因。',
          code: snippets.orchestrate,
          link: '/zh/tutorial/dependencies'
        },
        {
          id: 'scheduling',
          sticker: 'run',
          name: '声明截止时间、QoS 与资源',
          api: 'submit_auto(kairo::task(...).deadline(...).qos(...))',
          returns: 'future；错过 deadline 计入 DeadlineMissed，可在状态中观察',
          note: '0.6.0 起调度决策由可注入的 IScheduler 产出，排序层次为优先级、EDF、FIFO。',
          code: snippets.scheduling,
          link: '/zh/tutorial/scheduling-runtime'
        },
        {
          id: 'realtime',
          sticker: 'cheer',
          name: '低延迟队列与周期实时线程',
          api: 'dispatch_auto(TaskOptions, task)',
          returns: 'DispatchResult：有界队列是否接收，不是完成通知',
          note: '必须显式指定后端；丢弃和背压通过 RealtimeExecutorStatus 与失败事件观察。',
          code: snippets.realtime,
          link: '/zh/realtime-and-communication/realtime-control'
        },
        {
          id: 'worker',
          sticker: 'sleep',
          name: '长期运行、可中断的阻塞 I/O',
          api: 'start_worker(BlockingWorkerSpec{...})',
          returns: 'WorkerHandle：启动结果、状态，以及 stop / request_stop',
          note: 'shutdown() 会请求停止、唤醒并 join 所有 worker，不会 detach。',
          code: snippets.worker,
          link: '/zh/realtime-and-communication/blocking-io-workers'
        },
        {
          id: 'gpu',
          sticker: 'think',
          name: '同一任务的 CPU 与 GPU 两种实现',
          api: 'submit_auto(cpu_gpu_task(cpu, gpu))',
          returns: 'future：实际选中路径的完成或异常',
          note: '没有 GPU 时不影响普通 CPU 路径；是否回退到 CPU 由 FallbackPolicy 显式决定。',
          code: snippets.gpu,
          link: '/zh/gpu/automatic-scheduling'
        }
      ]
    },
    comm: {
      title: '线程之间怎么传数据，按语义选组件',
      intro: 'kairo::comm 只做进程内通信。先说清楚数据的含义，再选组件，而不是先选队列再补语义。',
      link: { text: '对比全部通信组件', href: '/zh/guides/choosing-communication' },
      items: [
        ['每条消息都要按顺序处理', 'MpscChannel<T>'],
        ['只关心最新的配置或目标', 'LatestMailbox<T>'],
        ['同一事件发给多个独立消费者', 'Topic<T>'],
        ['实时周期内只处理有限条命令', 'RealtimeChannel<T>'],
        ['多个读者读取一致的完整状态', 'DoubleBuffer<T>'],
        ['启动、标定、运行按阶段推进', 'PhaseGate']
      ]
    },
    trust: {
      seenTitle: '失败和过载看得见',
      seen: [
        ['提交被拒绝', '记录为 SubmitRejected，future 立即带回原因'],
        ['任务抛出异常', 'future.get() 在调用线程重新抛出'],
        ['排队超时', '单独计数，不混入任务失败'],
        ['实时队列丢弃', 'RealtimeExecutorStatus 中的 drop 与水位'],
        ['错过 deadline', 'deadline_missed_count 与结构化路由原因'],
        ['总量过载', '配置 max_in_flight_tasks 后结构化拒绝']
      ],
      seenLink: { text: '失败可观测性', href: '/zh/reliability/failure-observability' },
      notTitle: 'Kairo 不是什么',
      not: [
        '不是协程运行时：核心是普通可调用对象和 std::future。',
        '不是分布式消息系统：Topic 只在进程内扇出，没有持久化和重放。',
        '不是硬实时操作系统：最终 jitter 仍取决于任务体、系统与硬件。',
        '不能强制终止正在运行的 C++ 函数：长期工作需要主动响应 stop。',
        'submit_periodic() 是普通线程池上的软周期任务，不是实时线程。'
      ],
      notLink: { text: '完整的能力边界', href: '/zh/getting-started/what-is-kairo' }
    },
    path: {
      title: '推荐的学习顺序',
      steps: [
        { title: '构建并运行第一个任务', detail: '编译、链接、拿到返回值和异常，大约十分钟。', link: '/zh/quick-start/build' },
        { title: '按约束选择提交接口', detail: '对照工作语义，决定是否需要离开默认路径。', link: '/zh/guides/choosing-submit-api' },
        { title: '跟着机器人流水线教程', detail: '同一套业务对象，从后台解析一路走到实时控制。', link: '/zh/tutorial/' },
        { title: '上线前逐项检查', detail: '容量、关闭顺序、失败告警与平台部署。', link: '/zh/guides/production-readiness' }
      ],
      more: [
        { text: '从线程代码迁移', href: '/zh/guides/migrating-existing-threads' },
        { text: '高级与原理', href: '/zh/advanced/' },
        { text: '版本与迁移', href: '/zh/reference/version-and-migration' },
        { text: '完整 API 参考', href: '/zh/reference/api' },
        { text: '给 AI 用的 Kairo 集成 skill', href: 'https://github.com/Linductor-alkaid/kairo/blob/master/docs/skill/kairo-integration/SKILL.md' }
      ],
    },
    closing: { title: '十分钟，跑通你的第一个任务', detail: '编译、链接、拿到返回值，再故意抛一个异常看看它怎么回来。' },
    versionNote: '本手册对应 v0.7.0；master 上的后续能力需在发布 tag 后才构成稳定版承诺。'
  },
  en: {
    hero: {
      title: 'In-process concurrency infrastructure for C++20 applications',
      lead: 'Thread pools, low-latency queues, periodic realtime threads, long-lived blocking I/O, and optional GPU work, all managed by one Executor. Start with submit_auto() and move to a specialized path only when a real constraint calls for it.',
      primary: { text: 'Run your first task', link: '/en/quick-start/build' },
      secondary: { text: 'What is Kairo?', link: '/en/getting-started/what-is-kairo' },
      facts: [
        ['Version', 'v0.7.0'],
        ['Language', 'C++20'],
        ['Build', 'CMake 3.16+'],
        ['Platforms', 'Linux, Windows, Android'],
        ['License', 'MIT']
      ],
      imageAlt: 'Kairo, the project character, glancing back over her shoulder: long navy hair with blue and gold streaks, a gold star hair clip, and a white jacket.'
    },
    lanes: {
      title: 'Choose an entry point by what the work is',
      intro: 'Ask one question first: does the caller need task completion, queue admission, or the lifecycle of a long-lived worker? Kairo never disguises one as another.',
      apiLabel: 'Entry point',
      returnsLabel: 'Caller receives',
      more: 'Read this section',
      items: [
        {
          id: 'async',
          sticker: 'peace',
          name: 'Ordinary one-off work',
          api: 'submit_auto(lambda)',
          returns: 'std::future<T>: the value, or the task exception rethrown by get()',
          note: 'The default path. Auto never silently moves work to lock-free, realtime, or GPU backends for speed.',
          code: snippets.async,
          link: '/en/quick-start/first-task'
        },
        {
          id: 'orchestrate',
          sticker: 'tablet',
          name: 'Priority, delay, batches, dependencies',
          api: 'submit_priority / submit_delayed / submit_batch / submit_after',
          returns: 'a future, a task ID, or a TaskHandle you can chain',
          note: 'Priority only orders the queue and never preempts running work. If a dependency fails, dependents do not run and their futures carry the reason.',
          code: snippets.orchestrate,
          link: '/en/tutorial/dependencies'
        },
        {
          id: 'scheduling',
          sticker: 'run',
          name: 'Deadlines, QoS, and resources',
          api: 'submit_auto(kairo::task(...).deadline(...).qos(...))',
          returns: 'a future; missed deadlines are counted as DeadlineMissed',
          note: 'Since 0.6.0 an injectable IScheduler makes the decision, ordering by priority, then EDF, then FIFO.',
          code: snippets.scheduling,
          link: '/en/tutorial/scheduling-runtime'
        },
        {
          id: 'realtime',
          sticker: 'cheer',
          name: 'Low-latency queues and periodic realtime threads',
          api: 'dispatch_auto(TaskOptions, task)',
          returns: 'DispatchResult: whether the bounded queue accepted it, not completion',
          note: 'The backend must be named explicitly. Drops and backpressure show up in RealtimeExecutorStatus and failure events.',
          code: snippets.realtime,
          link: '/en/realtime-and-communication/realtime-control'
        },
        {
          id: 'worker',
          sticker: 'sleep',
          name: 'Long-lived, interruptible blocking I/O',
          api: 'start_worker(BlockingWorkerSpec{...})',
          returns: 'WorkerHandle: start result, status, stop and request_stop',
          note: 'shutdown() requests stop, wakes, and joins every worker. Nothing is detached.',
          code: snippets.worker,
          link: '/en/realtime-and-communication/blocking-io-workers'
        },
        {
          id: 'gpu',
          sticker: 'think',
          name: 'Separate CPU and GPU implementations',
          api: 'submit_auto(cpu_gpu_task(cpu, gpu))',
          returns: 'a future for whichever path was selected',
          note: 'No GPU means no change to the CPU path. Falling back to CPU is an explicit FallbackPolicy choice.',
          code: snippets.gpu,
          link: '/en/gpu/automatic-scheduling'
        }
      ]
    },
    comm: {
      title: 'Pick a communication component by what the data means',
      intro: 'kairo::comm is in-process only. Decide what the data means first, then pick the component.',
      link: { text: 'Compare every component', href: '/en/guides/choosing-communication' },
      items: [
        ['Every message must be handled in order', 'MpscChannel<T>'],
        ['Only the latest configuration matters', 'LatestMailbox<T>'],
        ['Several consumers each get the same events', 'Topic<T>'],
        ['A realtime cycle handles a bounded batch', 'RealtimeChannel<T>'],
        ['Many readers need one consistent state', 'DoubleBuffer<T>'],
        ['Setup, calibration, run advance in phases', 'PhaseGate']
      ]
    },
    trust: {
      seenTitle: 'Failure and overload stay visible',
      seen: [
        ['Rejected submission', 'recorded as SubmitRejected; the future carries the reason'],
        ['Task exception', 'rethrown by future.get() on the caller thread'],
        ['Queue timeout', 'counted separately from task failures'],
        ['Realtime drops', 'drop counts and watermarks in RealtimeExecutorStatus'],
        ['Missed deadline', 'deadline_missed_count plus a structured routing reason'],
        ['Total overload', 'structured rejection once max_in_flight_tasks is set']
      ],
      seenLink: { text: 'Failure observability', href: '/en/reliability/failure-observability' },
      notTitle: 'What Kairo is not',
      not: [
        'Not a coroutine runtime: the core is plain callables and std::future.',
        'Not a distributed messaging system: Topic fans out in-process, with no persistence or replay.',
        'Not a hard realtime OS: jitter still depends on the task body, the OS, and the hardware.',
        'It cannot force a running C++ function to stop; long-lived work must honor stop requests.',
        'submit_periodic() is soft periodic work on the ordinary pool, not a realtime thread.'
      ],
      notLink: { text: 'Full scope and boundaries', href: '/en/getting-started/what-is-kairo' }
    },
    path: {
      title: 'A good order to learn it in',
      steps: [
        { title: 'Build and run your first task', detail: 'Compile, link, get a value and an exception back. About ten minutes.', link: '/en/quick-start/build' },
        { title: 'Choose a submission API by constraint', detail: 'Match your work to a semantic, and decide whether to leave the default path.', link: '/en/guides/choosing-submit-api' },
        { title: 'Follow the robot pipeline tutorial', detail: 'One set of domain types, from background parsing to realtime control.', link: '/en/tutorial/' },
        { title: 'Check it before production', detail: 'Capacity, shutdown order, failure alerts, and platform deployment.', link: '/en/guides/production-readiness' }
      ],
      more: [
        { text: 'Migrate existing thread code', href: '/en/guides/migrating-existing-threads' },
        { text: 'Advanced topics', href: '/en/advanced/' },
        { text: 'Versions and migration', href: '/en/reference/version-and-migration' },
        { text: 'API reference', href: '/en/reference/api' },
        { text: 'Kairo integration skill for AI assistants', href: 'https://github.com/Linductor-alkaid/kairo/blob/master/docs/skill/kairo-integration/SKILL.md' }
      ],
    },
    closing: { title: 'Ten minutes to your first task', detail: 'Compile, link, get a value back, then throw on purpose and watch the exception come home.' },
    versionNote: 'This guide corresponds to v0.7.0. Later capabilities on master become stable promises only after their release tag.'
  }
}

export function homeRoutes() {
  const routes = []
  for (const locale of Object.values(homeContent)) {
    routes.push(locale.hero.primary.link, locale.hero.secondary.link)
    for (const lane of locale.lanes.items) routes.push(lane.link)
    routes.push(locale.comm.link.href, locale.trust.seenLink.href, locale.trust.notLink.href)
    for (const step of locale.path.steps) routes.push(step.link)
    for (const item of locale.path.more) routes.push(item.href)
  }
  return routes.filter((route) => route.startsWith('/'))
}
