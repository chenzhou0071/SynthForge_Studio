<script setup lang="ts">
import { computed } from "vue";
import type { AssetData } from "../chat/history";

const props = defineProps<{ asset: AssetData }>();
const emit = defineEmits<{ preview: [path: string] }>();

const glbPath = computed(
  () => props.asset.files.find((file) => file.suffix === "glb")?.path ?? "",
);
const sourceText = computed(() => {
  if (props.asset.source !== "cloud3d") return "低模建模";
  const detail = [props.asset.provider, props.asset.model].filter(Boolean).join(" ");
  return detail ? `云API · ${detail}` : "云API";
});
const fileNames = computed(() => props.asset.files.map((file) => file.suffix.toUpperCase()));
</script>

<template>
  <div class="asset-card">
    <div class="asset-head">
      <strong>资产已生成</strong>
      <span class="asset-name">{{ asset.name }}</span>
    </div>
    <dl class="asset-meta">
      <div>
        <dt>来源</dt>
        <dd>{{ sourceText }}</dd>
      </div>
      <div v-if="asset.prompt">
        <dt>提示</dt>
        <dd>{{ asset.prompt }}</dd>
      </div>
      <div>
        <dt>文件</dt>
        <dd class="file-list">
          <span v-for="suffix in fileNames" :key="suffix" class="file-chip">{{ suffix }}</span>
        </dd>
      </div>
      <div>
        <dt>目录</dt>
        <dd>{{ asset.dir || "资产库根目录" }}</dd>
      </div>
      <div v-if="asset.createdAt">
        <dt>时间</dt>
        <dd>{{ asset.createdAt }}</dd>
      </div>
    </dl>
    <div class="asset-actions">
      <button class="view primary" :disabled="!glbPath" @click="emit('preview', glbPath)">
        查看
      </button>
    </div>
  </div>
</template>

<style scoped>
.asset-card {
  width: 100%;
  border: 1px solid var(--border);
  border-left: 3px solid var(--accent);
  border-radius: 6px;
  padding: 10px 12px;
  background: rgba(255, 255, 255, 0.03);
}

.asset-head {
  display: flex;
  align-items: baseline;
  gap: 8px;
  margin-bottom: 8px;
}

.asset-head strong {
  font-size: 13px;
}

.asset-name {
  color: var(--muted);
  font-family: ui-monospace, Consolas, monospace;
  font-size: 12px;
  word-break: break-all;
}

.asset-meta {
  margin: 0 0 8px;
  display: grid;
  gap: 4px;
}

.asset-meta > div {
  display: flex;
  gap: 8px;
  font-size: 12px;
  line-height: 1.5;
}

.asset-meta dt {
  flex: none;
  width: 32px;
  color: var(--muted);
}

.asset-meta dd {
  margin: 0;
  min-width: 0;
  word-break: break-word;
}

.file-list {
  display: flex;
  flex-wrap: wrap;
  gap: 4px;
}

.file-chip {
  border: 1px solid var(--border);
  border-radius: 3px;
  padding: 0 5px;
  font-size: 11px;
  color: var(--muted);
}

.asset-actions {
  display: flex;
  justify-content: flex-end;
}

.asset-actions button {
  padding: 4px 14px;
  font-size: 12px;
}
</style>
