# Executor 使用手册网站

<p align="center">
  <img src="/kairo.svg" width="160" alt="Executor logo">
</p>

本目录包含 Executor 的 VitePress 使用手册站点。首发以中文内容为主，路由结构预留 `zh/` 和 `en/` 对称目录。

## 要求

- Node.js 24 LTS 或更新版本
- npm（使用提交的 `package-lock.json` 进行严格安装）

## 本地开发

```bash
npm ci --prefix website
npm run docs:dev --prefix website
```

## 构建与预览

```bash
npm run docs:build --prefix website
npm run docs:preview --prefix website
```

静态文件输出到 `website/.vitepress/dist/`。页面通过 VitePress 引用或仓库链接关联完整教程示例，示例仍由根 CMake 工程构建和测试。

## 本地检查

```bash
npm run docs:check --prefix website
```

该检查验证站内路由、相对文件链接及教程嵌入源文件；外部链接不在 PR 阶段访问，以避免网络波动阻塞构建。GitHub Actions 对 PR 执行严格安装、站点构建和教程 smoke tests；仅 `master` 与 `v*` tag 会部署 GitHub Pages。

## 首次启用 GitHub Pages

工作流会通过 `actions/configure-pages@v5` 的 `enablement: true` 自动启用 GitHub Pages 并配置为 GitHub Actions 部署。若 GitHub Pages 服务暂时返回 5xx，重新运行该 workflow；若仓库或组织策略禁止 Pages，请由管理员在 GitHub 的 **Settings → Pages → Build and deployment** 中确认允许使用 **GitHub Actions**。工作流使用 Node.js 24；第三方 action 产生的 Node 20 弃用提示不影响站点构建或部署。

## 目录约定

- `.vitepress/`：站点配置、主题和样式。
- `zh/`：中文页面；首发默认语言。
- `en/`：后续英文镜像，必须和中文页面共享示例源文件。
- `decisions.md`：URL、版本和内容事实源的已冻结决策。
