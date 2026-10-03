# 发布文档同步清单

每次发布 tag 前，由发布维护者逐项确认：

- [ ] CMake 项目版本、`CHANGELOG.md` 和 `docs/MIGRATION.md` 已更新并相互一致。
- [ ] `README.md`、`README_zh.md`、`docs/API.md` 与网站版本标识已核对；开发快照能力不会标为既有稳定版能力。
- [ ] 打包脚本（`scripts/package_*.sh` / `scripts/package_windows.ps1` 及对应 build_and_package 编排）的版本默认值与本版本一致。
- [ ] `v*` tag 推送后 release workflow（`.github/workflows/release.yml`）三个 job 全绿：deb-amd64（CUDA devel 容器）、windows-static-x64、release。
- [ ] GitHub Release 资产齐全：`kairo-<版本>-linux-x86_64.tar.gz`、`libkairo_<版本>_amd64.deb`、`libkairo-dev_<版本>_amd64.deb`、`kairo-<版本>-windows-x64.zip`；`dpkg-deb -I` 抽查 control 字段（版本、维护者、架构）。
- [ ] Release 发布说明由 `CHANGELOG.md` 对应版本小节自动截取生成，人工复核排版与链接。
- [ ] 网站中英文 `version-and-migration` 已新增本版本小节，`index`/`maintenance`/`decisions`/`api` 的版本标识已同步。
- [ ] 新增或变更的公开 Facade 已通过网站 [API 覆盖索引](../website/zh/reference/api.md) 找到教程、专题、选型或参考入口。
- [ ] 受影响的 `examples/tutorial/` 已构建，并通过 `ctest --test-dir build -L tutorial --output-on-failure`。
- [ ] 已执行 `npm ci --prefix website`、`npm run docs:check --prefix website` 和 `npm run docs:build --prefix website`。
- [ ] 已确认仓库 **Settings → Pages** 的部署来源为 **GitHub Actions**，并检查文档 workflow 使用 Node.js 24。
- [ ] 已检查 GitHub Pages 预览：首页、快速开始、API 参考和 404 页面可访问。
- [ ] TSAN 本地运行注意：较新内核的 ASLR 熵（`vm.mmap_rnd_bits`）与
  GCC TSAN 运行时不兼容时会报 `unexpected memory mapping`（exit 66）；
  用 `setarch $(uname -m) -R ./test_xxx` 进程级关闭 ASLR 重跑即可，
  CI 侧如遇同样报错按同法处理。
- [ ] **0.6.0 一次性步骤——GitHub 仓库改名（executor → kairo）**：仓库
  Settings → General → Rename；改名后旧 URL 由 GitHub 301 重定向，但需
  同步更新：本地各 clone 的 remote URL、Codecov/Pages 绑定、badge URL
  （README 已预写 `Linductor-alkaid/kairo`）、docs.yml 的 Pages 部署
  （base 路径 `/kairo/`）、以及 website 自定义域名/URL 引用。
- [ ] 已审阅用户反馈、404 和失效链接；需要修复的内容已建立 issue。
- [ ] Android CI（NDK r26c / r28b，arm64-v8a / x86_64，static / shared）最近一次为 success。
- [ ] Android 官方模拟器已运行 `scripts/run_android_tests.sh` 全部 standalone 测试并 PASS。
- [ ] ARM64 concurrency workflow（4 核、单核、ASan/UBSan、600s soak）最近一次为 success；结果同步至 `docs/performance/android_a3_validation.md`。
- [ ] 至少一台 big.LITTLE Android 真机已复测 A3 测试集和 10 分钟 soak；如未完成，本版本不得宣称已在 big.LITTLE 设备验证。
- [ ] 网站 Blocking I/O 页面在窄屏与宽屏下长 API 名、表格和 code block 不溢出（人工视觉核对，自 blocking_io_executor_update_plan 迁入）。
- [ ] release 文档核对：版本范围、API、迁移材料、教程、中文页、英文页和 translation status 相互同步（自 blocking_io_executor_update_plan 迁入）。

稳定版以发布 tag 触发正式发布；`master` 推送部署当前开发快照。若需要变更这一策略，先更新网站版本说明和 Pages workflow，再发布内容。
