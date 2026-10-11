import DefaultTheme from 'vitepress/theme'
import { computed, h, nextTick, onMounted, ref, watch } from 'vue'
import { useRoute } from 'vitepress'
import '@fontsource-variable/plus-jakarta-sans'
import '@fontsource-variable/unbounded'
import '@fontsource-variable/jetbrains-mono'
import './custom.css'
import './home.css'
import KairoHome from './KairoHome.vue'

const zoomStep = 0.25
const minZoom = 0.5
const maxZoom = 3
const siteBase = '/kairo'

function withSiteBase(path) {
  return `${siteBase}${path}`
}

function normalizeRoutePath(path) {
  const cleanPath = path.replace(/\.html$/, '').replace(/^\/kairo(?=\/|$)/, '') || '/'
  return cleanPath.endsWith('/') || cleanPath === '/' ? cleanPath : cleanPath.replace(/\/$/, '')
}

function switchLocalePath(path) {
  if (path === '/') return '/en/'
  if (path === '/en/' || path === '/en') return '/'
  // 站务页（decisions/maintenance/translation-status）的中文版在 root，
  // 英文版在 /en/ 下，与前述内容目录方向相反。
  if (path.startsWith('/en/decisions') || path.startsWith('/en/maintenance') ||
      path.startsWith('/en/translation-status')) {
    return path.replace(/^\/en/, '') || '/'
  }
  if (path.startsWith('/zh/')) return path.replace(/^\/zh(?=\/)/, '/en')
  if (path.startsWith('/en/')) return path.replace(/^\/en(?=\/)/, '/zh')
  return `/en${path}`
}

function requestedPath(routePath) {
  if (typeof window !== 'undefined') return normalizeRoutePath(window.location.pathname)
  return normalizeRoutePath(routePath)
}

function isEnglishPath(path) {
  return path === '/en' || path.startsWith('/en/')
}

const LanguageSwitch = {
  setup() {
    const route = useRoute()
    const isEnglish = computed(() => isEnglishPath(normalizeRoutePath(route.path)))
    const target = computed(() => switchLocalePath(normalizeRoutePath(route.path)))
    const label = computed(() => isEnglish.value ? '简体中文' : 'English')

    return () => h('a', {
      class: 'language-switch',
      href: withSiteBase(target.value)
    }, label.value)
  }
}

const NotFound = {
  setup() {
    const route = useRoute()
    const path = ref(normalizeRoutePath(route.path))
    const updatePath = () => { path.value = requestedPath(route.path) }
    onMounted(updatePath)
    watch(() => route.path, updatePath)
    const isEnglish = computed(() => isEnglishPath(path.value))

    return () => {
      if (isEnglish.value) {
        return h('main', { class: 'language-not-found VPContent', lang: 'en' }, [
          h('div', { class: 'container' }, [
            h('div', { class: 'content' }, [
              h('h1', 'Page not found'),
              h('p', 'This link may have moved, or the requested page is not published.'),
              h('ul', [
                h('li', [h('a', { href: withSiteBase('/en/') }, 'Return to the English home page')]),
                h('li', [h('a', { href: withSiteBase('/en/quick-start/first-task') }, 'Start the ten-minute quick start')]),
                h('li', [h('a', { href: withSiteBase('/en/reference/version-and-migration') }, 'Check versions and migration')])
              ])
            ])
          ])
        ])
      }

      return h('main', { class: 'language-not-found VPContent', lang: 'zh-CN' }, [
        h('div', { class: 'container' }, [
          h('div', { class: 'content' }, [
            h('h1', '页面未找到'),
            h('p', '该链接可能已经移动，或对应内容尚未发布。'),
            h('ul', [
              h('li', [h('a', { href: withSiteBase('/') }, '从首页重新开始')]),
              h('li', [h('a', { href: withSiteBase('/zh/quick-start/first-task') }, '进入十分钟快速开始')]),
              h('li', [h('a', { href: withSiteBase('/zh/reference/version-and-migration') }, '查看版本与迁移')])
            ])
          ])
        ])
      ])
    }
  }
}

function createDiagramViewer() {
  const viewer = document.createElement('div')
  viewer.className = 'mermaid-viewer'
  viewer.hidden = true
  viewer.innerHTML = `
    <div class="mermaid-viewer__panel" role="dialog" aria-modal="true" aria-label="流程图放大查看">
      <div class="mermaid-viewer__toolbar">
        <span class="mermaid-viewer__title">流程图</span>
        <div class="mermaid-viewer__actions">
          <button type="button" data-action="zoom-out" aria-label="缩小流程图">−</button>
          <output aria-live="polite">100%</output>
          <button type="button" data-action="zoom-in" aria-label="放大流程图">＋</button>
          <button type="button" data-action="reset">重置</button>
          <button type="button" data-action="close" aria-label="关闭流程图查看器">关闭</button>
        </div>
      </div>
      <div class="mermaid-viewer__canvas"></div>
    </div>`
  document.body.appendChild(viewer)

  const canvas = viewer.querySelector('.mermaid-viewer__canvas')
  const output = viewer.querySelector('output')
  const closeButton = viewer.querySelector('[data-action="close"]')
  let zoom = 1
  let trigger = null

  const applyZoom = () => {
    const svg = canvas.querySelector('svg')
    if (svg) {
      const viewBox = svg.viewBox.baseVal
      svg.style.width = `${viewBox.width * zoom}px`
      svg.style.height = `${viewBox.height * zoom}px`
    }
    output.value = `${Math.round(zoom * 100)}%`
  }

  const close = () => {
    viewer.hidden = true
    canvas.replaceChildren()
    document.body.classList.remove('mermaid-viewer-open')
    trigger?.focus()
    trigger = null
  }

  viewer.addEventListener('click', (event) => {
    if (event.target === viewer) close()
    const action = event.target.closest('button')?.dataset.action
    if (action === 'zoom-in') zoom = Math.min(maxZoom, zoom + zoomStep)
    if (action === 'zoom-out') zoom = Math.max(minZoom, zoom - zoomStep)
    if (action === 'reset') zoom = 1
    if (action === 'close') return close()
    if (action) applyZoom()
  })
  document.addEventListener('keydown', (event) => {
    if (viewer.hidden) return
    if (event.key === 'Escape') close()
    if (event.key === '+' || event.key === '=') {
      zoom = Math.min(maxZoom, zoom + zoomStep)
      applyZoom()
    }
    if (event.key === '-') {
      zoom = Math.max(minZoom, zoom - zoomStep)
      applyZoom()
    }
  })

  return {
    open(diagram, button) {
      const svg = diagram.querySelector('svg')
      if (!svg) return
      trigger = button
      zoom = 1
      canvas.replaceChildren(svg.cloneNode(true))
      viewer.hidden = false
      document.body.classList.add('mermaid-viewer-open')
      applyZoom()
      closeButton.focus()
    }
  }
}

function addDiagramControls(diagram) {
  if (diagram.querySelector('.mermaid-diagram__expand')) return
  const button = document.createElement('button')
  button.type = 'button'
  button.className = 'mermaid-diagram__expand'
  button.setAttribute('aria-label', '放大查看流程图')
  button.textContent = '放大查看'
  button.addEventListener('click', () => {
    window.executorMermaidViewer ??= createDiagramViewer()
    window.executorMermaidViewer.open(diagram, button)
  })
  diagram.appendChild(button)
}

async function renderMermaidDiagrams() {
  if (typeof window === 'undefined') return

  const diagrams = [...document.querySelectorAll('.mermaid-diagram:not([data-mermaid-rendered])')]
  if (diagrams.length === 0) return

  const { default: mermaid } = await import('mermaid')
  mermaid.initialize({
    startOnLoad: false,
    securityLevel: 'strict',
    theme: document.documentElement.classList.contains('dark') ? 'dark' : 'base'
  })

  for (const diagram of diagrams) {
    const source = diagram.getAttribute('data-mermaid-source') || ''
    try {
      const id = `executor-mermaid-${Math.random().toString(36).slice(2)}`
      const { svg, bindFunctions } = await mermaid.render(id, source)
      diagram.innerHTML = svg
      diagram.setAttribute('data-mermaid-rendered', 'true')
      bindFunctions?.(diagram)
      addDiagramControls(diagram)
    } catch (error) {
      diagram.setAttribute('data-mermaid-error', 'true')
      console.error('Failed to render Mermaid diagram', error)
    }
  }
}

const Layout = {
  setup() {
    const route = useRoute()
    onMounted(() => renderMermaidDiagrams())
    watch(() => route.path, () => nextTick(() => renderMermaidDiagrams()))
    return () => h(DefaultTheme.Layout, null, {
      'nav-bar-content-after': () => h(LanguageSwitch)
    })
  }
}

export default {
  ...DefaultTheme,
  Layout,
  NotFound,
  enhanceApp(ctx) {
    DefaultTheme.enhanceApp?.(ctx)
    ctx.app.component('KairoHome', KairoHome)
  }
}
