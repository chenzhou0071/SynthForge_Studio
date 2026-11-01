<script setup lang="ts">
import { onBeforeUnmount, ref } from "vue";
import type { WsClient } from "../ws/WsClient";

interface LogEntry {
  level: string;
  category: string;
  message: string;
  time: string;
}
interface EnvItem {
  state?: string;
  detail?: string;
}
type EnvReport = { uvx?: EnvItem; blender?: EnvItem; unity?: EnvItem };

const props = defineProps<{ client: WsClient; open: boolean }>();
const emit = defineEmits<{ close: [] }>();

// 环形上限：后端一路吐 warning 也不能把页面吃光；完整历史在日志文件里
const MAX_ENTRIES = 200;

const entries = ref<LogEntry[]>([]);
const env = ref<EnvReport>({});

const offLog = props.client.on("log.append", (data) => {
  const entry = data as LogEntry;
  if (!entry?.message) return;
  entries.value.unshift(entry);
  if (entries.value.length > MAX_ENTRIES) entries.value.length = MAX_ENTRIES;
});
const offEnv = props.client.on("env.status", (data) => {
  env.value = (data as EnvReport) ?? {};
});

onBeforeUnmount(() => {
  offLog();
  offEnv();
});

// 抽屉关闭时不清列表：刚看过的告警要能回头接着看，
// 内存与刷屏风暴由上面的环形上限兼顾；抽屉全量历史本来就在日志文件里。

// failed 的 detail 往往是内部错误码，面板只说人话
function envText(item?: EnvItem) {
  if (!item?.state) return "未检查";
  if (item.state === "ready") return item.detail || "就绪";
  return "未就绪";
}

function recheck() {
  // env.check 立即返回，结果仍旧走 env.status 事件
  void props.client.request("env.check");
}

// 面板级清空：只清这一屏，日志文件仍是真相源
function clearLog() {
  entries.value = [];
}
</script>

<template>
  <div v-if="open" class="log-mask" @click.self="emit('close')">
    <aside class="log-drawer">
      <header class="drawer-head">
        <span>诊断</span>
        <div class="head-actions">
          <button class="clear" @click="clearLog">清空</button>
          <button class="recheck" @click="recheck">重新自检</button>
          <button class="close" @click="emit('close')">✕</button>
        </div>
      </header>
      <section class="env">
        <div>uvx：{{ envText(env.uvx) }}</div>
        <div>Blender：{{ envText(env.blender) }}</div>
        <div>Unity：{{ envText(env.unity) }}</div>
      </section>
      <section class="logs">
        <p v-if="!entries.length" class="log-empty">暂无警告或错误日志</p>
        <div v-for="(line, i) in entries" :key="i" class="log-line" :class="line.level">
          <span class="log-time">{{ line.time }}</span>
          <span class="log-cat">{{ line.category }}</span>
          <span class="log-msg">{{ line.message }}</span>
        </div>
      </section>
    </aside>
  </div>
</template>

<style scoped>
.log-mask {
  position: fixed;
  inset: 0;
  background: rgba(0, 0, 0, 0.35);
  display: flex;
  justify-content: flex-end;
  z-index: 40;
}

.log-drawer {
  width: min(520px, 92vw);
  height: 100%;
  background: var(--panel, #23272e);
  border-left: 1px solid var(--border);
  color: var(--text);
  display: flex;
  flex-direction: column;
}

.drawer-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 10px 12px;
  border-bottom: 1px solid var(--border);
  font-size: 13px;
}

.head-actions {
  display: flex;
  gap: 6px;
}

.head-actions button {
  background: transparent;
  border: 1px solid var(--border);
  border-radius: 3px;
  color: var(--text);
  padding: 2px 8px;
  font-size: 12px;
}

.env {
  padding: 8px 12px;
  border-bottom: 1px solid var(--border);
  font-size: 12px;
  color: var(--muted);
  display: grid;
  gap: 2px;
}

.logs {
  flex: 1;
  min-height: 0;
  overflow-y: auto;
  padding: 8px 12px;
  scrollbar-width: thin;
}

.log-empty {
  color: var(--muted);
  font-size: 12px;
}

.log-line {
  display: flex;
  gap: 8px;
  font-size: 12px;
  padding: 2px 0;
  border-bottom: 1px solid rgba(255, 255, 255, 0.04);
}

.log-time {
  color: var(--muted);
  flex: none;
}

.log-cat {
  color: var(--muted);
  flex: none;
}

.log-msg {
  white-space: pre-wrap;
  word-break: break-word;
}

.log-line.error .log-msg {
  color: #e06c75;
}
</style>
