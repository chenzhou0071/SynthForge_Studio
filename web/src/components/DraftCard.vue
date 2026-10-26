<script setup lang="ts">
import { onBeforeUnmount, ref, watch } from "vue";
import type { WsClient } from "../ws/WsClient";

const props = defineProps<{
  client: WsClient;
  id?: number;
  content: string;
  state?: "pending" | "cancelled";
  origin?: "discussion" | "generated";
}>();
const emit = defineEmits<{ sent: [] }>();

const editing = ref(false);
const text = ref(props.content);
const saved = ref(props.content);
const error = ref("");
const sending = ref(false);
const regenerating = ref(false);
const pending = () => (props.state ?? "pending") === "pending";

// 重新生成失败也要停转圈，不然按钮会永远锁死
const offFailed = props.client.on("draft.failed", (data) => {
  const event = data as { id?: number; reason?: string };
  if (event.id !== props.id) return;
  regenerating.value = false;
  error.value = "重新生成失败，请重试。";
});
onBeforeUnmount(offFailed);

watch(
  () => props.content,
  (value) => {
    // 重新生成后回填；正在编辑时不抢用户输入
    if (editing.value) return;
    text.value = value;
    saved.value = value;
    if (regenerating.value) regenerating.value = false;
  },
);

function beginEdit() {
  if (pending() && !regenerating.value) editing.value = true;
}

// 退出编辑即落库：刷新后看到的是改过的版本
async function finishEdit() {
  editing.value = false;
  if (!pending() || !props.id || text.value === saved.value) return;
  try {
    await props.client.request("draft.save", { id: props.id, content: text.value });
    saved.value = text.value;
  } catch (e) {
    error.value = e instanceof Error ? e.message : "保存失败";
  }
}

async function run() {
  if (!pending() || sending.value || regenerating.value) return;
  error.value = "";
  sending.value = true;
  try {
    const res = await props.client.request<{
      accepted?: boolean;
      error?: string;
      reason?: string;
    }>("draft.send", { id: props.id, prompt: text.value });
    if (res?.accepted === false) {
      const reason = res.error ?? res.reason;
      error.value =
        reason === "no-blend-source"
          ? "源文件已不存在，无法修改：可点「重新生成」换一个方案，或取消后重新选择资产。"
          : reason === "blender-not-ready"
            ? "Blender 未连接：请先打开 Blender 并启用插件，卡片可继续编辑。"
            : (reason ?? "执行失败");
      return;
    }
    emit("sent"); // 已转成正式指令，面板据此进入进行中
  } catch (e) {
    error.value = e instanceof Error ? e.message : "执行失败";
  } finally {
    sending.value = false;
  }
}

async function regenerate() {
  if (!pending() || sending.value || regenerating.value) return;
  error.value = "";
  regenerating.value = true;
  try {
    const res = await props.client.request<{ accepted?: boolean }>("draft.regenerate", {
      id: props.id,
    });
    if (res?.accepted === false) {
      regenerating.value = false;
      error.value = "草稿已失效，请重新讨论生成。";
    }
  } catch (e) {
    regenerating.value = false;
    error.value = e instanceof Error ? e.message : "重新生成失败";
  }
}

async function cancel() {
  if (!pending() || sending.value || regenerating.value) return;
  try {
    await props.client.request("draft.cancel", { id: props.id });
  } catch (e) {
    error.value = e instanceof Error ? e.message : "取消失败";
  }
}
</script>

<template>
  <div class="draft-card" :class="{ cancelled: !pending() }">
    <div class="draft-hint">
      {{ origin === "discussion" ? "讨论整理出的草稿" : "优化后的提示词" }}
      <template v-if="pending()"> · 双击编辑 · 执行前不会触碰 Blender</template>
      <template v-else> · 已取消</template>
    </div>
    <textarea v-if="editing && pending()" v-model="text" autofocus @blur="finishEdit" />
    <pre v-else @dblclick="beginEdit">{{ text }}</pre>
    <div v-if="error" class="draft-error">{{ error }}</div>
    <div v-if="pending()" class="draft-actions">
      <button class="primary" :disabled="sending || regenerating || !text.trim()" @click="run">
        <i v-if="sending" class="spinner" />
        {{ sending ? "执行中…" : "执行" }}
      </button>
      <button class="ghost" :disabled="sending || regenerating" @click="regenerate">
        <i v-if="regenerating" class="spinner" />
        {{ regenerating ? "重新生成中…" : "重新生成" }}
      </button>
      <button class="ghost" :disabled="sending || regenerating" @click="cancel">取消</button>
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

.draft-card.cancelled {
  border-style: solid;
  opacity: 0.65;
}

.draft-error {
  margin-bottom: 6px;
  color: #f48771;
  font-size: 12px;
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

.draft-actions button {
  display: inline-flex;
  align-items: center;
  gap: 6px;
}

.spinner {
  width: 12px;
  height: 12px;
  border: 2px solid currentColor;
  border-right-color: transparent;
  border-radius: 50%;
  animation: draft-spin 0.7s linear infinite;
}

@keyframes draft-spin {
  to {
    transform: rotate(360deg);
  }
}
</style>
