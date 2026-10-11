import { existsSync, readdirSync, readFileSync, statSync } from 'node:fs'
import { join, normalize, relative, resolve } from 'node:path'

const websiteRoot = resolve(import.meta.dirname, '..')
const repositoryRoot = resolve(websiteRoot, '..')
const markdownFiles = []

function collectMarkdown(directory) {
  for (const entry of readdirSync(directory)) {
    const path = join(directory, entry)
    if (statSync(path).isDirectory()) {
      if (!['node_modules', '.vitepress'].includes(entry)) collectMarkdown(path)
    } else if (entry.endsWith('.md')) {
      markdownFiles.push(path)
    }
  }
}

function isExistingPage(route) {
  const pathname = route.replace(/[?#].*$/, '').replace(/^\//, '')
  if (!pathname) return existsSync(join(websiteRoot, 'index.md'))
  return [
    join(websiteRoot, `${pathname}.md`),
    join(websiteRoot, pathname, 'index.md')
  ].some(existsSync)
}

function fail(message) {
  console.error(message)
  process.exitCode = 1
}

collectMarkdown(websiteRoot)

for (const filename of markdownFiles) {
  const source = readFileSync(filename, 'utf8')
  const displayName = relative(repositoryRoot, filename)

  for (const match of source.matchAll(/\[[^\]]*\]\(([^)]+)\)/g)) {
    const target = match[1].trim()
    if (!target || /^(https?:|mailto:|#)/.test(target)) continue

    if (!target.startsWith('/') && !target.startsWith('.')) continue

    if (target.startsWith('/')) {
      if (!isExistingPage(target)) fail(`${displayName}: missing site route ${target}`)
      continue
    }

    const targetPath = normalize(join(resolve(filename, '..'), target.replace(/[?#].*$/, '')))
    if (!targetPath.startsWith(websiteRoot) || !existsSync(targetPath)) {
      fail(`${displayName}: missing relative link ${target}`)
    }
  }

  for (const match of source.matchAll(/<<<\s*@\/([^\s{]+\.cpp)/g)) {
    const targetPath = resolve(websiteRoot, match[1])
    if (!existsSync(targetPath)) fail(`${displayName}: missing embedded source @/${match[1]}`)
  }
}

const { homeRoutes } = await import('../.vitepress/theme/home-content.mjs')
for (const route of homeRoutes()) {
  if (!isExistingPage(route)) fail(`.vitepress/theme/home-content.mjs: missing site route ${route}`)
}

if (!process.exitCode) console.log(`OK: checked ${markdownFiles.length} Markdown files and local links.`)
