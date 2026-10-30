<script setup lang="ts">
import type { ChatItem } from "../chat/history";
import type { WsClient } from "../ws/WsClient";
import { renderMarkdown } from "../chat/markdown";
import AssetCard from "./AssetCard.vue";
import DraftCard from "./DraftCard.vue";
import PlanCard from "./PlanCard.vue";

// 单条消息的渲染。父组件只传 item（对象引用），流式时只有这一条子组件因 item.content
// 变化而重渲，不再牵动整个消息列表——这是「吐字慢」的根治点。
defineProps<{ item: ChatItem; area: string; client: WsClient; busy: boolean }>();
const emit = defineEmits<{
  preview: [path: string];
  openSettings: [target: "agent" | "cloud"];
  resume: [item: ChatItem];
  retry: [item: ChatItem];
  draftSent: [];
}>();
</script>

<template>
  <div v-if="item.role === 'image'" class="bubble image">
    <img class="shot" :src="item.dataUrl" :alt="item.caption ?? '任务截图'" />
    <span v-if="item.caption" class="caption">{{ item.caption }}</span>
  </div>
  <div v-else-if="item.role === 'error'" class="error-card">
    <strong>请求失败</strong>
    <p>{{ item.content }}</p>
    <button v-if="item.resumable" class="resume" :disabled="busy" @click="emit('resume', item)">
      继续工作
    </button>
    <button v-else-if="item.retryable" class="resume" :disabled="busy" @click="emit('retry', item)">
      重试
    </button>
    <button v-else-if="item.settings" class="resume" @click="emit('openSettings', item.settings)">
      去设置
    </button>
  </div>
  <div v-else-if="item.role === 'asset' && item.asset" class="draft-row">
    <AssetCard :asset="item.asset" @preview="emit('preview', $event)" />
  </div>
  <div v-else-if="item.role === 'draft'" class="draft-row">
    <DraftCard
      :client="client"
      :id="item.id"
      :content="item.content ?? ''"
      :state="item.state === 'cancelled' ? 'cancelled' : 'pending'"
      :origin="item.origin"
      @sent="emit('draftSent')"
    />
  </div>
  <div v-else-if="item.role === 'plan' && item.plan" class="draft-row">
    <PlanCard
      :client="client"
      :area="area"
      :plan="item.plan"
      :id="item.id"
      :state="item.state"
    />
  </div>
  <div v-else-if="item.role === 'group'" class="group-divider">
    <span>第 {{ item.groupIndex }}/{{ item.groupCount }} 组 · {{ item.title }}</span>
  </div>
  <div v-else class="bubble" :class="[item.role, { streaming: item.streaming }]">
    <!-- 流式期间也走 markdown：否则 ** 和 ## 会原样露出 -->
    <div class="md" v-html="renderMarkdown(item.content ?? '')" />
    <i v-if="item.streaming" class="caret" />
  </div>
</template>

<style scoped>
.bubble.image {
  padding: 6px;
}

.shot {
  display: block;
  max-width: 100%;
  border: 1px solid var(--border);
  border-radius: 4px;
}

.caption {
  display: block;
  margin-top: 4px;
  color: var(--muted);
  font-size: 12px;
}

.error-card .resume {
  margin-top: 6px;
  background: var(--accent);
  border: 0;
  color: #fff;
  border-radius: 3px;
  padding: 4px 12px;
  font-size: 12px;
}

.error-card .resume:disabled {
  opacity: 0.5;
}

.group-divider {
  display: flex;
  align-items: center;
  gap: 8px;
  color: var(--muted);
  font-size: 12px;
}

.group-divider::before,
.group-divider::after {
  content: "";
  flex: 1;
  border-top: 1px solid var(--border);
}
</style>
