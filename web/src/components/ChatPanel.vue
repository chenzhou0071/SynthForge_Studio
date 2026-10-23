<script setup lang="ts">
import { nextTick, onBeforeUnmount, onMounted, ref } from "vue";
import type { WsClient } from "../ws/WsClient";

type Area = "blender" | "unity";
type ChatMessage = { role: "user" | "assistant"; content: string };

const props = defineProps<{ area: Area; client: WsClient }>();

const messages = ref<ChatMessage[]>([]);
const draft = ref("");
const streaming = ref("");
const errors = ref<string[]>([]);
const busy = ref(false);
const scroller = ref<HTMLElement | null>(null);
const unsubscribers: Array<() => void> = [];

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

onMounted(() => {
  unsubscribers.push(
    props.client.on("chat.message", (data) => {
      const msg = data as { area?: string; role?: string; content?: string };
      if (msg.area !== props.area || !msg.role) return;
      messages.value.push({
        role: msg.role === "user" ? "user" : "assistant",
        content: msg.content ?? "",
      });
      if (msg.role === "assistant") {
        streaming.value = "";
        busy.value = false;
      }
      scrollToBottom();
    }),
    props.client.on("chat.delta", (data) => {
      const delta = data as { area?: string; text?: string };
      if (delta.area !== props.area) return;
      streaming.value += delta.text ?? "";
      scrollToBottom();
    }),
    props.client.on("error.card", (data) => {
      const card = data as { area?: string; reason?: string };
      if (card.area !== props.area) return;
      errors.value.push(reasonText(card.reason ?? "unknown"));
      streaming.value = "";
      busy.value = false;
      scrollToBottom();
    }),
  );
});

onBeforeUnmount(() => {
  unsubscribers.forEach((off) => off());
});

async function send() {
  const text = draft.value.trim();
  if (!text || busy.value) return;
  draft.value = "";
  busy.value = true;
  errors.value = [];
  streaming.value = "";
  try {
    const result = await props.client.request<{ accepted?: boolean }>("chat.send", {
      area: props.area,
      text,
    });
    if (result.accepted === false) busy.value = false;
  } catch (error) {
    busy.value = false;
    errors.value.push(error instanceof Error ? error.message : "发送失败");
  }
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
    <header class="panel-head">
      <span>AI 对话</span>
      <span class="chat-area">{{ area === "blender" ? "Blender" : "Unity" }}</span>
    </header>

    <div ref="scroller" class="chat-log">
      <p v-if="messages.length === 0 && !streaming && errors.length === 0" class="chat-empty">
        描述你要生成的资产或要做的修改。
      </p>

      <div v-for="(message, index) in messages" :key="index" class="bubble" :class="message.role">
        <p>{{ message.content }}</p>
      </div>

      <div v-if="streaming" class="bubble assistant streaming">
        <p>{{ streaming }}<i class="caret" /></p>
      </div>

      <div v-for="(reason, index) in errors" :key="'error-' + index" class="error-card">
        <strong>请求失败</strong>
        <p>{{ reason }}</p>
      </div>
    </div>

    <footer class="chat-input">
      <textarea
        v-model="draft"
        rows="3"
        placeholder="输入消息，Enter 发送，Shift+Enter 换行"
        @keydown="onKeydown"
      />
      <button class="primary" :disabled="busy || !draft.trim()" @click="send">
        {{ busy ? "生成中…" : "发送" }}
      </button>
    </footer>
  </section>
</template>
