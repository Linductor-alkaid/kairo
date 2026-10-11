<script setup>
import { computed, nextTick, ref } from 'vue'
import { withBase } from 'vitepress'
import { homeContent } from './home-content.mjs'
import { highlightCpp } from './highlight-cpp.mjs'

const props = defineProps({
  locale: { type: String, default: 'zh' }
})

const t = computed(() => homeContent[props.locale] ?? homeContent.zh)
const activeId = ref('async')
const active = computed(() => t.value.lanes.items.find((item) => item.id === activeId.value))
const activeCode = computed(() => highlightCpp(active.value.code))
const tabRefs = ref([])

// 背景星点：位置固定，避免每次渲染跳动。[left%, top%, size px]
const sparkles = [
  [4, 16, 10], [3, 78, 7], [24, 8, 6], [40, 92, 8], [50, 14, 7],
  [53, 60, 5], [60, 9, 12], [96, 30, 8], [62, 90, 6]
]

function href(link) {
  return /^https?:/.test(link) ? link : withBase(link)
}

function sticker(name) {
  return withBase(`/stickers/${name}.webp`)
}

function selectLane(id, focus = false) {
  activeId.value = id
  if (focus) {
    const index = t.value.lanes.items.findIndex((item) => item.id === id)
    nextTick(() => tabRefs.value[index]?.focus())
  }
}

function onTabKey(event, index) {
  const items = t.value.lanes.items
  const step = { ArrowDown: 1, ArrowRight: 1, ArrowUp: -1, ArrowLeft: -1 }[event.key]
  let next = null
  if (step) next = (index + step + items.length) % items.length
  if (event.key === 'Home') next = 0
  if (event.key === 'End') next = items.length - 1
  if (next === null) return
  event.preventDefault()
  selectLane(items[next].id, true)
}
</script>

<template>
  <div class="kh" :lang="locale === 'en' ? 'en' : 'zh-CN'">
    <section class="kh-hero">
      <div class="kh-hero__sky" aria-hidden="true">
        <svg
          v-for="([x, y, size], index) in sparkles"
          :key="index"
          class="kh-sparkle"
          viewBox="0 0 24 24"
          :style="{ left: `${x}%`, top: `${y}%`, width: `${size}px`, '--i': index }"
        >
          <path d="M12 0l2.4 9.6L24 12l-9.6 2.4L12 24l-2.4-9.6L0 12l9.6-2.4z" />
        </svg>
      </div>

      <div class="kh-hero__art">
        <img
          :src="withBase('/kairo-pose.webp')"
          :alt="t.hero.imageAlt"
          width="960"
          height="960"
          fetchpriority="high"
        />
      </div>

      <div class="kh-wrap kh-hero__copy">
        <p class="kh-wordmark">Kairo</p>
        <h1 class="kh-hero__title">{{ t.hero.title }}</h1>
        <p class="kh-hero__lead">{{ t.hero.lead }}</p>
        <div class="kh-hero__actions">
          <a class="kh-btn kh-btn--gold" :href="href(t.hero.primary.link)">{{ t.hero.primary.text }}</a>
          <a class="kh-btn kh-btn--ghost" :href="href(t.hero.secondary.link)">{{ t.hero.secondary.text }}</a>
        </div>
        <dl class="kh-facts">
          <div v-for="[label, value] in t.hero.facts" :key="label">
            <dt>{{ label }}</dt>
            <dd>{{ value }}</dd>
          </div>
        </dl>
      </div>
    </section>

    <section id="kh-lanes" class="kh-section kh-lanes">
      <div class="kh-wrap">
        <h2 class="kh-h2">{{ t.lanes.title }}</h2>
        <p class="kh-intro">{{ t.lanes.intro }}</p>

        <div class="kh-picker">
          <div class="kh-picker__tabs" role="tablist" aria-orientation="vertical">
            <button
              v-for="(item, index) in t.lanes.items"
              :id="`kh-tab-${item.id}`"
              :key="item.id"
              :ref="(el) => { tabRefs[index] = el }"
              type="button"
              role="tab"
              class="kh-tab"
              :aria-selected="item.id === activeId"
              aria-controls="kh-panel"
              :tabindex="item.id === activeId ? 0 : -1"
              @click="selectLane(item.id)"
              @keydown="onTabKey($event, index)"
            >
              <span class="kh-tab__name">{{ item.name }}</span>
              <code class="kh-tab__api">{{ item.api }}</code>
            </button>
          </div>

          <div
            id="kh-panel"
            class="kh-panel"
            role="tabpanel"
            :aria-labelledby="`kh-tab-${active.id}`"
          >
            <img
              :key="active.sticker"
              class="kh-panel__sticker"
              :src="sticker(active.sticker)"
              alt=""
              aria-hidden="true"
            />
            <dl class="kh-panel__meta">
              <div>
                <dt>{{ t.lanes.apiLabel }}</dt>
                <dd><code>{{ active.api }}</code></dd>
              </div>
              <div>
                <dt>{{ t.lanes.returnsLabel }}</dt>
                <dd>{{ active.returns }}</dd>
              </div>
            </dl>
            <pre class="kh-code"><code v-html="activeCode"></code></pre>
            <p class="kh-panel__note">{{ active.note }}</p>
            <a class="kh-link" :href="href(active.link)">{{ t.lanes.more }}</a>
          </div>
        </div>
      </div>
    </section>

    <section id="kh-comm" class="kh-section kh-comm">
      <div class="kh-wrap">
        <div class="kh-comm__head">
          <div>
            <h2 class="kh-h2">{{ t.comm.title }}</h2>
            <p class="kh-intro">{{ t.comm.intro }}</p>
          </div>
          <a class="kh-link" :href="href(t.comm.link.href)">{{ t.comm.link.text }}</a>
        </div>
        <ul class="kh-comm__grid">
          <li v-for="[need, component] in t.comm.items" :key="component">
            <span>{{ need }}</span>
            <code>{{ component }}</code>
          </li>
        </ul>
      </div>
    </section>

    <section class="kh-section kh-trust">
      <div class="kh-wrap kh-trust__grid">
        <div>
          <h2 class="kh-h2">{{ t.trust.seenTitle }}</h2>
          <dl class="kh-seen">
            <div v-for="[what, how] in t.trust.seen" :key="what">
              <dt>{{ what }}</dt>
              <dd>{{ how }}</dd>
            </div>
          </dl>
          <a class="kh-link" :href="href(t.trust.seenLink.href)">{{ t.trust.seenLink.text }}</a>
        </div>
        <div class="kh-not">
          <img class="kh-not__sticker" :src="sticker('question')" alt="" aria-hidden="true" loading="lazy" />
          <h2 class="kh-h3">{{ t.trust.notTitle }}</h2>
          <ul>
            <li v-for="line in t.trust.not" :key="line">{{ line }}</li>
          </ul>
          <a class="kh-link" :href="href(t.trust.notLink.href)">{{ t.trust.notLink.text }}</a>
        </div>
      </div>
    </section>

    <section class="kh-section kh-path">
      <div class="kh-wrap">
        <h2 class="kh-h2">{{ t.path.title }}</h2>
        <ol class="kh-steps">
          <li v-for="step in t.path.steps" :key="step.link">
            <a :href="href(step.link)">
              <span class="kh-steps__title">{{ step.title }}</span>
              <span class="kh-steps__detail">{{ step.detail }}</span>
            </a>
          </li>
        </ol>
        <ul class="kh-more">
          <li v-for="item in t.path.more" :key="item.href">
            <a :href="href(item.href)">{{ item.text }}</a>
          </li>
        </ul>
      </div>
    </section>

    <section class="kh-closing">
      <div class="kh-wrap kh-closing__inner">
        <img class="kh-closing__sticker" :src="sticker('gaze')" alt="" aria-hidden="true" loading="lazy" />
        <div>
          <h2 class="kh-closing__title">{{ t.closing.title }}</h2>
          <p class="kh-closing__detail">{{ t.closing.detail }}</p>
          <a class="kh-btn kh-btn--gold" :href="href(t.hero.primary.link)">{{ t.hero.primary.text }}</a>
          <p class="kh-version">{{ t.versionNote }}</p>
        </div>
      </div>
    </section>
  </div>
</template>
