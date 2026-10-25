<script setup lang="ts">
import { ref } from "vue";
import type { WsClient } from "../ws/WsClient";

const props = defineProps<{
  client: WsClient;
  area: string;
  plan: { taskId: string; groups: { title: string; items: string[]; acceptance: string }[]; totalSteps: number };
}>();
const decided = ref<"pending" | "started" | "cancelled">("pending");
const error = ref("");

async function start() {
  error.value = "";
  try {
    const res = await props.client.request<{ ok?: boolean; error?: string }>("plan.confirm", {
      area: props.area,
      taskId: props.plan.taskId,
    });
    if (res?.ok === false) {
      // blender-busy：计划卡保留，等当前建模/转换结束后可再次点开始
      error.value = res.error === "blender-busy"
        ? "Blender 会话正忙：请等当前建模/转换完成后再点开始"
        : (res.error ?? "确认失败");
      return;
    }
    decided.value = "started";
  } catch (e) {
    error.value = e instanceof Error ? e.message : "确认失败";
  }
}

function cancel() {
  decided.value = "cancelled";
  props.client.request("plan.cancel", { area: props.area, taskId: props.plan.taskId }).catch(() => {
    // 取消失败不阻断 UI：卡片已置灰，后端即便没收到也不会再执行
  });
}
</script>

<template>
  <div class="plan-card">
    <div class="plan-hint">建造计划 · 预估 {{ plan.totalSteps }} 步</div>
    <div class="plan-warn">开始后会先清空 Blender 当前场景（未保存的手工修改会丢失）</div>
    <ol>
      <li v-for="(g, i) in plan.groups" :key="i">
        <div class="group-title">{{ i + 1 }}. {{ g.title }}</div>
        <div v-if="g.items.length" class="group-items">{{ g.items.join(" / ") }}</div>
        <div class="group-accept">验收：{{ g.acceptance }}</div>
      </li>
    </ol>
    <div v-if="error" class="plan-error">{{ error }}</div>
    <div v-if="decided === 'pending'" class="plan-actions">
      <button class="ghost" @click="cancel">取消</button>
      <button class="primary" @click="start">开始生成</button>
    </div>
    <div v-else class="plan-state">
      {{ decided === "started" ? "已确认，等待选择存放位置…" : "已取消" }}
    </div>
  </div>
</template>

<style scoped>
.plan-card {
  width: 100%;
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 10px 12px;
  background: rgba(255, 255, 255, 0.02);
}

.plan-hint {
  color: var(--muted);
  font-size: 12px;
}

.plan-warn {
  font-size: 12px;
  color: #d97706;
  margin: 2px 0 8px;
}

ol {
  margin: 0 0 8px;
  padding-left: 18px;
}

li {
  margin-bottom: 6px;
}

.group-title {
  font-size: 13px;
}

.group-items,
.group-accept {
  color: var(--muted);
  font-size: 12px;
}

.plan-error {
  margin-bottom: 6px;
  color: #f48771;
  font-size: 12px;
}

.plan-actions {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
}

.plan-state {
  color: var(--muted);
  font-size: 12px;
  text-align: right;
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
