<script setup lang="ts">
import { nextTick, onBeforeUnmount, onMounted, ref, watch } from "vue";
import { isCloudConfigured } from "../chat/cloud";
import { toChatItems } from "../chat/history";
import type { ChatItem, PlanGroup } from "../chat/history";
import type { WsClient } from "../ws/WsClient";
import DraftCard from "./DraftCard.vue";
import PlanCard from "./PlanCard.vue";

type Area = "blender" | "unity";

const props = defineProps<{ area: Area; client: WsClient; settingsVersion?: number }>();

const messages = ref<ChatItem[]>([]);
const draft = ref("");
const mode = ref<"lowpoly" | "cloud3d">("lowpoly");
const cloudConfigured = ref(false);
// 每区一个常驻实例：busy 属于本实例，切区只 v-show，状态不会串区
const busy = ref(false);
const scroller = ref<HTMLElement | null>(null);
const unsubscribers: Array<() => void> = [];
let streaming: ChatItem | null = null; // 指向 messages 里的流式气泡，非响应式引用

function reasonText(reason: string) {
  if (reason === "agent-not-configured") {
    return "未配置 Agent：请先在设置中填写服务商、模型与密钥。";
  }
  return reason;
}

function scrollToBottom() {
  void nextTick(() => {
    if (scroller.value) scroller.value.scrollTop = scroller.value.scrollHeight;
  });
}

function pushError(reason: string) {
  messages.value.push({ role: "error", content: reasonText(reason) });
}

async function refreshCloudBadge() {
  if (props.area !== "blender") return;
  try {
    const settings = await props.client.request<unknown>("settings.get");
    cloudConfigured.value = isCloudConfigured(settings);
  } catch {
    cloudConfigured.value = false; // 读不到设置就按未配置提示，别把徽标卡在旧状态
  }
}

async function loadHistory() {
  messages.value = [];
  streaming = null;
  try {
    // 先用 50 条覆盖常见会话；更早的分页留给 P4 的“加载更多”
    const page = await props.client.request<unknown>("session.load", { area: props.area, limit: 50 });
    messages.value = toChatItems(page);
  } catch (error) {
    pushError(error instanceof Error ? error.message : "读取历史失败");
  }
  scrollToBottom();
}

onMounted(() => {
  unsubscribers.push(
    props.client.on("chat.message", (data) => {
      const msg = data as { area?: string; role?: string; content?: string };
      if (msg.area !== props.area || !msg.role) return;
      const content = msg.content ?? "";
      if (msg.role === "assistant") {
        busy.value = false;
        if (streaming) {
          // 流结束：用完整文本覆盖气泡，避免多出一条重复消息
          streaming.content = content;
          streaming.streaming = false;
          streaming = null;
          scrollToBottom();
          return;
        }
      }
      messages.value.push({ role: msg.role === "user" ? "user" : "assistant", content });
      scrollToBottom();
    }),
    props.client.on("chat.delta", (data) => {
      const delta = data as { area?: string; text?: string };
      if (delta.area !== props.area) return;
      if (!streaming) {
        streaming = { role: "assistant", content: "", streaming: true };
        messages.value.push(streaming);
      }
      streaming.content += delta.text ?? "";
      busy.value = true; // 重试等非 send 路径也要能显示“停止”
      scrollToBottom();
    }),
    props.client.on("chat.image", (data) => {
      const image = data as { area?: string; dataUrl?: string; caption?: string };
      if (image.area !== props.area) return;
      messages.value.push({ role: "image", dataUrl: image.dataUrl ?? "", caption: image.caption });
      scrollToBottom();
    }),
    props.client.on("task.progress", (data) => {
      const progress = data as { area?: string; state?: string };
      if (progress.area !== props.area) return;
      // 低模分组进度没有 state；云任务 progress 带终态
      busy.value = progress.state !== "success" && progress.state !== "failed";
    }),
    props.client.on("asset.created", (data) => {
      if ((data as { area?: string }).area === props.area) busy.value = false; // 落盘即任务结束
    }),
    props.client.on("draft.created", (data) => {
      const draft = data as { area?: string; taskId?: string; prompt?: string };
      if (draft.area !== props.area) return;
      // 后端已停止生成，等用户编辑确认；发送后才进入规划
      busy.value = false;
      messages.value.push({
        role: "draft",
        content: draft.prompt ?? "",
        taskId: draft.taskId,
        mode: mode.value,
      });
      scrollToBottom();
    }),
    props.client.on("plan.created", (data) => {
      const payload = data as {
        area?: string;
        taskId?: string;
        groups?: PlanGroup[];
        totalSteps?: number;
      };
      if (payload.area !== props.area) return;
      // 计划已就绪，后端不再生成；等用户点开始才选存放位置
      busy.value = false;
      messages.value.push({
        role: "plan",
        plan: {
          taskId: payload.taskId ?? "",
          groups: payload.groups ?? [],
          totalSteps: payload.totalSteps ?? 0,
        },
      });
      scrollToBottom();
    }),
    props.client.on("error.card", (data) => {
      const card = data as { area?: string; reason?: string };
      if (card.area !== props.area) return;
      if (streaming) {
        // 保留已经收到的半截内容，只是不再显示光标
        streaming.streaming = false;
        streaming = null;
      }
      pushError(card.reason ?? "unknown");
      busy.value = false;
      scrollToBottom();
    }),
  );

  void loadHistory();
  void refreshCloudBadge();
});

// 设置保存后重新拉取徽标状态（对话框在本页内，不会触发重新挂载）
watch(
  () => props.settingsVersion,
  () => void refreshCloudBadge(),
);

onBeforeUnmount(() => {
  unsubscribers.forEach((off) => off());
});

async function send() {
  const text = draft.value.trim();
  if (!text || busy.value) return;
  draft.value = "";
  busy.value = true;
  try {
    const params: Record<string, unknown> = { area: props.area, text };
    if (props.area === "blender") params.mode = mode.value;
    const result = await props.client.request<{ accepted?: boolean }>("chat.send", params);
    if (result?.accepted === false) busy.value = false;
  } catch (error) {
    busy.value = false;
    pushError(error instanceof Error ? error.message : "发送失败");
  }
  scrollToBottom();
}

function removeDraft(item: ChatItem) {
  messages.value = messages.value.filter((message) => message !== item);
}

async function sendDraft(item: ChatItem, text: string) {
  if (busy.value) return;
  busy.value = true;
  try {
    const result = await props.client.request<{ accepted?: boolean }>("draft.send", {
      area: props.area,
      prompt: text,
      mode: item.mode ?? mode.value,
    });
    if (result?.accepted === false) {
      busy.value = false;
      return;
    }
    removeDraft(item); // 已转为正式指令，后端会广播 chat.message(user)
  } catch (error) {
    busy.value = false;
    pushError(error instanceof Error ? error.message : "发送失败");
  }
}

async function cancelDraft(item: ChatItem) {
  removeDraft(item);
  try {
    await props.client.request("draft.cancel", { area: props.area, taskId: item.taskId });
  } catch (error) {
    pushError(error instanceof Error ? error.message : "取消失败");
  }
}

async function cancelTask() {
  try {
    await props.client.request("chat.cancel", { area: props.area });
  } catch (error) {
    pushError(error instanceof Error ? error.message : "停止失败");
  } finally {
    busy.value = false;
  }
}

async function clearChat() {
  if (!window.confirm("清空本区对话？此操作不可撤销")) return;
  try {
    await props.client.request("chat.clear", { area: props.area });
  } catch (error) {
    pushError(error instanceof Error ? error.message : "清空失败");
    return;
  }
  messages.value = [];
  streaming = null;
  busy.value = false; // chat.clear 会取消本区任务
}

function onKeydown(event: KeyboardEvent) {
  if (event.key === "Enter" && !event.shiftKey) {
    event.preventDefault();
    void send();
  }
}
</script>

<template>
  <section class="chat">
    <header class="panel-head chat-head">
      <span>AI 对话</span>
      <span class="chat-area">{{ area === "blender" ? "Blender" : "Unity" }}</span>
      <span class="spacer" />
      <button class="ghost" title="清空本区对话" @click="clearChat">清空</button>
    </header>

    <div v-if="area === 'blender'" class="chat-tools">
      <div class="mode-switch">
        <button :class="{ active: mode === 'lowpoly' }" @click="mode = 'lowpoly'">低模</button>
        <button :class="{ active: mode === 'cloud3d' }" @click="mode = 'cloud3d'">云API</button>
      </div>
      <span class="badge" :class="cloudConfigured ? 'ok' : 'warn'">
        {{ cloudConfigured ? "云API 已配置" : "云API 未配置" }}
      </span>
    </div>

    <div ref="scroller" class="chat-log">
      <p v-if="messages.length === 0" class="chat-empty">描述你要生成的资产或要做的修改。</p>

      <template v-for="(message, index) in messages" :key="index">
        <div v-if="message.role === 'image'" class="bubble image">
          <img class="shot" :src="message.dataUrl" :alt="message.caption ?? '任务截图'" />
          <span v-if="message.caption" class="caption">{{ message.caption }}</span>
        </div>
        <div v-else-if="message.role === 'error'" class="error-card">
          <strong>请求失败</strong>
          <p>{{ message.content }}</p>
        </div>
        <div v-else-if="message.role === 'draft'" class="draft-row">
          <DraftCard
            :prompt="message.content ?? ''"
            @send="(text) => sendDraft(message, text)"
            @cancel="cancelDraft(message)"
          />
        </div>
        <div v-else-if="message.role === 'plan' && message.plan" class="draft-row">
          <PlanCard :client="client" :area="area" :plan="message.plan" />
        </div>
        <div v-else class="bubble" :class="[message.role, { streaming: message.streaming }]">
          <p>{{ message.content }}<i v-if="message.streaming" class="caret" /></p>
        </div>
      </template>
    </div>

    <footer class="chat-input">
      <textarea
        v-model="draft"
        rows="3"
        placeholder="输入消息，Enter 发送，Shift+Enter 换行"
        @keydown="onKeydown"
      />
      <div class="chat-actions">
        <button v-if="busy" class="stop" @click="cancelTask">停止</button>
        <button class="primary" :disabled="busy || !draft.trim()" @click="send">
          {{ busy ? "生成中…" : "发送" }}
        </button>
      </div>
    </footer>
  </section>
</template>

<style scoped>
.chat-head {
  gap: 6px;
}

.spacer {
  flex: 1;
}

.ghost {
  background: transparent;
  border: 1px solid var(--border);
  color: var(--muted);
  padding: 2px 8px;
  border-radius: 3px;
  font-size: 12px;
}

.ghost:hover {
  color: var(--text);
  border-color: var(--muted);
}

.chat-tools {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 0 12px 8px;
}

.mode-switch {
  display: inline-flex;
  border: 1px solid var(--border);
  border-radius: 4px;
  overflow: hidden;
}

.mode-switch button {
  border: 0;
  border-radius: 0;
  padding: 3px 10px;
  font-size: 12px;
  background: transparent;
  color: var(--muted);
}

.mode-switch button.active {
  background: var(--accent);
  color: #fff;
}

.badge {
  font-size: 12px;
  color: var(--muted);
}

.badge.ok {
  color: #4ec9b0;
}

.badge.warn {
  color: #cca700;
}

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

.chat-actions {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
}

.chat-actions .primary {
  align-self: auto;
}

.stop {
  background: transparent;
  border: 1px solid var(--danger-border);
  color: #f48771;
  border-radius: 3px;
  padding: 6px 14px;
}
</style>
