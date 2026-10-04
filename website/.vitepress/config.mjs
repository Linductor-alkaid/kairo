import { defineConfig } from 'vitepress'

export default defineConfig({
  lang: 'zh-CN',
  title: 'Kairo 使用手册',
  description: '面向 C++20 应用的进程内并发执行基础设施：统一 Facade 管理普通异步任务、低延迟队列、周期实时线程、长期 Blocking I/O 与可选 GPU 工作。',
  locales: {
    root: {
      label: '简体中文',
      lang: 'zh-CN'
    },
    en: {
      label: 'English',
      lang: 'en-US',
      title: 'Kairo Guide',
      description: 'In-process concurrency infrastructure for C++20 applications: one facade for ordinary async tasks, low-latency queues, periodic realtime threads, long-lived blocking I/O, and optional GPU work.',
      themeConfig: {
        logo: '/kairo.svg',
        siteTitle: 'Kairo Guide',
        nav: [
          { text: 'Quick Start', link: '/en/quick-start/build' },
          { text: 'Tutorials', link: '/en/tutorial/' },
          { text: 'Guides', link: '/en/guides/choosing-submit-api' },
          {
            text: 'Reference',
            items: [
              { text: 'Versions and Migration · v0.6.0', link: '/en/reference/version-and-migration' },
              { text: 'API Reference', link: '/en/reference/api' }
            ]
          },
          {
            text: 'Topics',
            items: [
              { text: 'Reliability', link: '/en/reliability/' },
              { text: 'Real-Time & Communication', link: '/en/realtime-and-communication/' },
              { text: 'GPU', link: '/en/gpu/' },
              { text: 'Advanced', link: '/en/advanced/' }
            ]
          },
          { text: 'GitHub', link: 'https://github.com/Linductor-alkaid/kairo' }
        ],
        sidebar: {
          '/en/quick-start/': [
            {
              text: 'Quick Start',
              items: [
                { text: 'What is Kairo?', link: '/en/getting-started/what-is-kairo' },
                { text: 'Build and Install', link: '/en/quick-start/build' },
                { text: 'Your First Task', link: '/en/quick-start/first-task' },
                { text: 'Submit Functions and Data', link: '/en/quick-start/task-inputs-and-ownership' },
                { text: 'Return Values and Errors', link: '/en/quick-start/return-values-and-errors' },
                { text: 'Initialization and Shutdown', link: '/en/quick-start/lifecycle' }
              ]
            }
          ],
          '/en/getting-started/': [
            {
              text: 'Getting Started',
              items: [
                { text: 'What is Kairo?', link: '/en/getting-started/what-is-kairo' },
                { text: 'Build and Install', link: '/en/quick-start/build' }
              ]
            }
          ],
          '/en/reference/': [
            {
              text: 'Reference',
              items: [
                { text: 'Versions and Migration', link: '/en/reference/version-and-migration' },
                { text: 'API Reference', link: '/en/reference/api' }
              ]
            }
          ],
          '/en/tutorial/': [
            {
              text: 'Tutorials',
              items: [
                { text: 'Robot Data Pipeline', link: '/en/tutorial/' },
                { text: 'Prioritize Control Commands', link: '/en/tutorial/priority' },
                { text: 'Delayed Retry and Health Checks', link: '/en/tutorial/delayed-and-periodic' },
                { text: 'Batch Sensor Frames', link: '/en/tutorial/batch' },
                { text: 'Load, Sense, Then Plan', link: '/en/tutorial/dependencies' },
                { text: 'Bounded Waiting and Status', link: '/en/tutorial/waiting-and-status' },
                { text: 'Fan Out Events with Topic', link: '/en/tutorial/topic-subscriptions' },
                { text: 'Complete Robot Pipeline', link: '/en/tutorial/complete-robot-pipeline' },
                { text: 'Service Data Import', link: '/en/tutorial/service-data-import' },
                { text: 'Declare Deadlines, Priorities, and Resources', link: '/en/tutorial/scheduling-runtime' }
              ]
            }
          ],
          '/en/guides/': [
            {
              text: 'Guides',
              items: [
                { text: 'Execution Models and Routing Boundaries', link: '/en/guides/execution-models-and-routing' },
                { text: 'Choose a Submission API', link: '/en/guides/choosing-submit-api' },
                { text: 'Choose a Communication Component', link: '/en/guides/choosing-communication' },
                { text: 'Interoperate with an External Event Loop', link: '/en/guides/event-loop-interop' },
                { text: 'Migrate Existing Thread Code', link: '/en/guides/migrating-existing-threads' },
                { text: 'Concurrency Architecture Antipatterns', link: '/en/guides/concurrency-antipatterns' },
                { text: 'Production Readiness Checklist', link: '/en/guides/production-readiness' }
              ]
            }
          ],
          '/en/realtime-and-communication/': [
            {
              text: 'Real-Time & Communication',
              items: [
                { text: 'Overview and Boundaries', link: '/en/realtime-and-communication/' },
                { text: 'Blocking I/O Workers', link: '/en/realtime-and-communication/blocking-io-workers' },
                { text: 'Dedicated Real-Time Control Loop', link: '/en/realtime-and-communication/realtime-control' },
                { text: 'Deliver Every Message', link: '/en/realtime-and-communication/channels' },
                { text: 'Latest Values, Snapshots, and Phases', link: '/en/realtime-and-communication/state-and-phases' },
                { text: 'Communication Observability', link: '/en/realtime-and-communication/observability' },
                { text: 'Capacity and Alerts', link: '/en/realtime-and-communication/capacity-and-alerting' },
                { text: 'Cancellation and Timers', link: '/en/realtime-and-communication/cancellation-and-timers' }
              ]
            }
          ],
          '/en/reliability/': [
            {
              text: 'Reliability',
              items: [
                { text: 'Reliability Overview', link: '/en/reliability/' },
                { text: 'Troubleshoot by Symptom', link: '/en/reliability/troubleshooting' },
                { text: 'Linux, Windows, and Android Deployment', link: '/en/reliability/platform-deployment' },
                { text: 'Failure Observability', link: '/en/reliability/failure-observability' },
                { text: 'Monitoring and Sampling', link: '/en/reliability/monitoring' }
              ]
            }
          ],
          '/en/advanced/': [
            {
              text: 'Advanced',
              items: [
                { text: 'Overview and Boundaries', link: '/en/advanced/' },
                { text: 'Source Architecture Map', link: '/en/advanced/source-architecture' },
                { text: 'Advanced Escape Hatches', link: '/en/advanced/escape-hatches' },
                { text: 'Custom Cycle Source', link: '/en/advanced/custom-cycle-manager' },
                { text: 'How Tasks Travel Through Kairo', link: '/en/advanced/execution-paths' },
                { text: 'Lock-Free and Performance Experiments', link: '/en/advanced/lockfree-and-performance' },
                { text: 'Performance Measurement and Regression Gates', link: '/en/advanced/performance-measurement' }
              ]
            }
          ],
          '/en/gpu/': [
            {
              text: 'GPU',
              items: [
                { text: 'GPU and Fallback', link: '/en/gpu/' },
                { text: 'Diagnose Backend and Fall Back Safely', link: '/en/gpu/diagnostics' },
                { text: 'Register and Submit GPU Work', link: '/en/gpu/register-and-submit' },
                { text: 'CPU/GPU Automatic Selection', link: '/en/gpu/automatic-scheduling' }
              ]
            }
          ]
        },
        outline: { level: [2, 3], label: 'On this page' },
        docFooter: { prev: 'Previous page', next: 'Next page' },
        footer: {
          message: 'MIT License · <a href="https://github.com/Linductor-alkaid/kairo/issues/new/choose">Report a documentation issue</a> · <a href="/kairo/en/maintenance">Content maintenance</a>',
          copyright: 'Kairo contributors'
        }
      }
    }
  },
  base: '/kairo/',
  cleanUrls: true,
  lastUpdated: true,
  sitemap: {
    hostname: 'https://linductor-alkaid.github.io/kairo/'
  },
  markdown: {
    config(md) {
      const defaultFence = md.renderer.rules.fence

      md.renderer.rules.fence = (tokens, index, options, env, self) => {
        const token = tokens[index]
        if (token.info.trim() === 'mermaid') {
          const source = md.utils.escapeHtml(token.content.trim())
          return `<div class="mermaid-diagram" data-mermaid-source="${source.replaceAll('"', '&quot;')}">${source}</div>`
        }
        return defaultFence(tokens, index, options, env, self)
      }
    }
  },
  head: [
    ['link', { rel: 'icon', type: 'image/svg+xml', href: '/kairo/kairo.svg?v=1', sizes: 'any' }],
    ['link', { rel: 'shortcut icon', type: 'image/svg+xml', href: '/kairo/kairo.svg?v=1' }],
    ['meta', { name: 'keywords', content: 'C++20, task executor, thread pool, real-time, realtime, robotics, lock-free, concurrent programming, C++ 任务执行器, 线程池, 实时系统, 机器人, 无锁' }],
    ['meta', { name: 'theme-color', content: '#181d26' }]
  ],
  themeConfig: {
    logo: '/kairo.svg',
    siteTitle: 'Kairo 使用手册',
    nav: [
      { text: '快速开始', link: '/zh/quick-start/build' },
      { text: '循序教程', link: '/zh/tutorial/' },
      { text: '场景指南', link: '/zh/guides/choosing-submit-api' },
      {
        text: '参考',
        items: [
          { text: '版本与迁移 · v0.6.0', link: '/zh/reference/version-and-migration' },
          { text: '完整 API 参考', link: '/zh/reference/api' }
        ]
      },
      {
        text: '专题',
        items: [
          { text: '可靠性', link: '/zh/reliability/' },
          { text: '实时与通信', link: '/zh/realtime-and-communication/' },
          { text: 'GPU', link: '/zh/gpu/' },
          { text: '高级与原理', link: '/zh/advanced/' }
        ]
      },
      { text: 'GitHub', link: 'https://github.com/Linductor-alkaid/kairo' }
    ],
    sidebar: {
      '/zh/quick-start/': [
        {
          text: '快速开始',
          items: [
            { text: 'Kairo 是什么', link: '/zh/getting-started/what-is-kairo' },
            { text: '构建与安装', link: '/zh/quick-start/build' },
            { text: '第一个任务', link: '/zh/quick-start/first-task' },
            { text: '提交自己的函数与数据', link: '/zh/quick-start/task-inputs-and-ownership' },
            { text: '返回值与异常', link: '/zh/quick-start/return-values-and-errors' },
            { text: '初始化与关闭', link: '/zh/quick-start/lifecycle' }
          ]
        }
      ],
      '/zh/getting-started/': [
        {
          text: '开始之前',
          items: [
            { text: 'Kairo 是什么', link: '/zh/getting-started/what-is-kairo' },
            { text: '构建与安装', link: '/zh/quick-start/build' }
          ]
        }
      ],
      '/zh/guides/': [
        {
          text: '场景指南',
          items: [
            { text: '执行模型与路由边界', link: '/zh/guides/execution-models-and-routing' },
            { text: '如何选择提交接口', link: '/zh/guides/choosing-submit-api' },
            { text: '如何选择通信组件', link: '/zh/guides/choosing-communication' },
            { text: '与外部事件循环互操作', link: '/zh/guides/event-loop-interop' },
            { text: '从现有线程代码迁移', link: '/zh/guides/migrating-existing-threads' },
            { text: '并发架构反模式', link: '/zh/guides/concurrency-antipatterns' },
            { text: '生产接入检查清单', link: '/zh/guides/production-readiness' }
          ]
        }
      ],
      '/zh/tutorial/': [
        {
          text: '循序教程',
          items: [
            { text: '机器人数据流水线', link: '/zh/tutorial/' },
            { text: '让控制命令优先', link: '/zh/tutorial/priority' },
            { text: '延迟重试与健康检查', link: '/zh/tutorial/delayed-and-periodic' },
            { text: '批量处理传感器帧', link: '/zh/tutorial/batch' },
            { text: '加载、感知与规划依赖', link: '/zh/tutorial/dependencies' },
            { text: '有界等待与状态快照', link: '/zh/tutorial/waiting-and-status' },
            { text: '用 Topic 扇出事件流', link: '/zh/tutorial/topic-subscriptions' },
            { text: '完整机器人数据流水线', link: '/zh/tutorial/complete-robot-pipeline' },
            { text: '服务端数据导入案例', link: '/zh/tutorial/service-data-import' },
            { text: '声明任务的期限、优先级与资源', link: '/zh/tutorial/scheduling-runtime' }
          ]
        }
      ],
      '/zh/reliability/': [
        {
          text: '可靠性',
          items: [
            { text: '可靠性概览', link: '/zh/reliability/' },
            { text: '按症状排查运行故障', link: '/zh/reliability/troubleshooting' },
            { text: 'Linux、Windows 与 Android 部署核对', link: '/zh/reliability/platform-deployment' },
            { text: '失败可观察性', link: '/zh/reliability/failure-observability' },
            { text: '监控与采样', link: '/zh/reliability/monitoring' }
          ]
        }
      ],
      '/zh/realtime-and-communication/': [
        {
          text: '实时与通信',
          items: [
            { text: '概览与边界', link: '/zh/realtime-and-communication/' },
            { text: '阻塞 I/O worker', link: '/zh/realtime-and-communication/blocking-io-workers' },
            { text: '启动专用实时控制循环', link: '/zh/realtime-and-communication/realtime-control' },
            { text: '传递每一条消息', link: '/zh/realtime-and-communication/channels' },
            { text: '传递最新值、快照和阶段', link: '/zh/realtime-and-communication/state-and-phases' },
            { text: '通信可观察性', link: '/zh/realtime-and-communication/observability' },
            { text: '容量判断与告警落地', link: '/zh/realtime-and-communication/capacity-and-alerting' },
            { text: '取消与定时', link: '/zh/realtime-and-communication/cancellation-and-timers' }
          ]
        }
      ],
      '/zh/gpu/': [
        {
          text: 'GPU',
          items: [
            { text: 'GPU 与降级', link: '/zh/gpu/' },
            { text: '诊断后端并安全降级', link: '/zh/gpu/diagnostics' },
            { text: '注册并提交 GPU 工作', link: '/zh/gpu/register-and-submit' },
            { text: 'CPU/GPU 自动选择', link: '/zh/gpu/automatic-scheduling' }
          ]
        }
      ],
      '/zh/advanced/': [
        {
          text: '高级与原理',
          items: [
            { text: '概览与边界', link: '/zh/advanced/' },
            { text: '源码架构与阅读地图', link: '/zh/advanced/source-architecture' },
            { text: '何时使用高级逃生口', link: '/zh/advanced/escape-hatches' },
            { text: '接入自定义周期源', link: '/zh/advanced/custom-cycle-manager' },
            { text: '任务如何穿过执行器', link: '/zh/advanced/execution-paths' },
            { text: '无锁与性能实验', link: '/zh/advanced/lockfree-and-performance' },
            { text: '性能测量与回归门禁', link: '/zh/advanced/performance-measurement' }
          ]
        }
      ],
      '/zh/reference/': [
        {
          text: '参考',
          items: [
            { text: 'API 参考', link: '/zh/reference/api' },
            { text: '版本与迁移', link: '/zh/reference/version-and-migration' }
          ]
        }
      ]
    },
    search: { provider: 'local' },
    outline: { level: [2, 3], label: '本页内容' },
    docFooter: { prev: '上一页', next: '下一页' },
    footer: {
      message: 'MIT License · <a href="https://github.com/Linductor-alkaid/kairo/issues/new/choose">反馈文档问题</a> · <a href="/kairo/maintenance">内容维护</a>',
      copyright: 'Kairo contributors'
    }
  }
})
