<script setup lang="ts">
import { ref } from "vue";

const props = defineProps<{ prompt: string }>();
const emit = defineEmits<{ send: [text: string]; cancel: [] }>();
const editing = ref(false);
const text = ref(props.prompt);
</script>

<template>
  <div class="draft-card">
    <div class="draft-hint">优化后的提示词 · 双击编辑</div>
    <textarea v-if="editing" v-model="text" autofocus @blur="editing = false" />
    <pre v-else @dblclick="editing = true">{{ text }}</pre>
    <div class="draft-actions">
      <button class="ghost" @click="emit('cancel')">取消</button>
      <button class="primary" :disabled="!text.trim()" @click="emit('send', text)">发送</button>
    </div>
  </div>
</template>

<style scoped>
.draft-card {
  width: 100%;
  border: 1px dashed var(--border);
  border-radius: 6px;
  padding: 10px 12px;
  background: rgba(255, 255, 255, 0.02);
}

.draft-hint {
  margin-bottom: 6px;
  color: var(--muted);
  font-size: 12px;
}

pre {
  margin: 0 0 8px;
  max-height: 260px;
  overflow: auto;
  white-space: pre-wrap;
  word-break: break-word;
  font-family: inherit;
  font-size: 13px;
  line-height: 1.55;
}

textarea {
  width: 100%;
  min-height: 180px;
  margin-bottom: 8px;
  resize: vertical;
  font-family: inherit;
  font-size: 13px;
  line-height: 1.55;
}

.draft-actions {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
}

.ghost {
  background: transparent;
  border: 1px solid var(--border);
  color: var(--muted);
  border-radius: 3px;
  padding: 4px 12px;
  font-size: 12px;
}

.ghost:hover {
  color: var(--text);
  border-color: var(--muted);
}
</style>
