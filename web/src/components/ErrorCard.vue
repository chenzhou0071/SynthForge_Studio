<script setup lang="ts">
import { computed } from "vue";

type Action = "retry" | "restart" | "settings";
type SettingsPage = "agent" | "cloud";

const props = defineProps<{
  reason: string;
  message?: string;
  steps?: number;
  maxSteps?: number;
  resumable?: boolean;
  busy?: boolean;
  // 已点过重试/续跑：保留文案但收起按钮，避免同一张卡重复发起任务
  resolved?: boolean;
}>();

const emit = defineEmits<{
  retry: [];
  restartBridge: [];
  continueWork: [];
  openSettings: [page: SettingsPage];
}>();

interface Entry {
  text: string;
  actions: Action[];
  page?: SettingsPage;
}

// 文案面向使用者：先说清「发生了什么」，再说「现在能做什么」。
// 后端 error.card 新增 reason 时必须同时加一条，否则前端会把内部错误码直接甩给用户。
const dictionary: Record<string, Entry> = {
  "blender-not-ready": {
    text: "Blender 链路未就绪：请打开 Blender 并启用 MCP for Blender 插件。",
    actions: ["retry", "restart"],
  },
  "unity-not-ready": {
    text: "Unity 链路未就绪：请确认工程已导入，且 MCP for Unity 窗口处于运行状态。",
    actions: ["retry", "restart"],
  },
  "agent-not-configured": {
    text: "未配置 Agent：请先在设置中填写服务商、模型与密钥。",
    actions: ["settings"],
    page: "agent",
  },
  "cloud3d-not-configured": {
    text: "未配置云 API：请先在设置中填写 3D 服务商与密钥。",
    actions: ["settings"],
    page: "cloud",
  },
  "llm-request-failed": {
    text: "模型请求失败：请检查 API 密钥、额度或网络。具体原因见下方。",
    actions: ["retry", "settings"],
    page: "agent",
  },
  "cloud3d-failed": {
    text: "云 3D 任务失败，可稍后重试。",
    actions: ["retry"],
  },
  "bridge-crashed": {
    text: "MCP 桥接异常退出，正在自动重连；等不及可以手动重启。",
    actions: ["restart"],
  },
  "bridge-not-registered": {
    text: "当前区没有可重启的桥接（未接入或配置有误）。",
    actions: [],
  },
  "blender-busy": {
    text: "Blender 正忙：建模任务或 GLB 转换还在跑，等它结束后再试一次。",
    actions: ["retry"],
  },
  // 回执超时不等于转换失败：命令常已发出，fbx 会迟到落地，重试会直接复用产物
  "blender-convert-timeout": {
    text: "转换没等到结果：Blender 可能还在执行上一条命令。等它空闲后再点一次导入即可，已生成的 fbx 会直接复用。",
    actions: ["retry"],
  },
  "blender-convert-failed": {
    text: "glb→fbx 转换失败：请查看 Blender 端报错后重试。",
    actions: ["retry", "restart"],
  },
  "import-failed": {
    text: "导入 Unity 失败：请查看 Unity Console 的报错后重试。",
    actions: ["retry"],
  },
  "invalid-attachment": {
    text: "附件无效：文件不存在，或不在模型库目录内。",
    actions: [],
  },
  "invalid-project": {
    text: "不是有效的 Unity 工程：需要同时包含 Assets 与 ProjectSettings。",
    actions: [],
  },
  "project-not-active": {
    text: "还没有激活的 Unity 工程：请先在顶栏导入工程。",
    actions: [],
  },
  "empty-plan": {
    text: "没拿到建造计划（模型没有给出分组）：请补充需求描述后重试。",
    actions: ["retry"],
  },
  "clear-scene-failed": {
    text: "进入建造前没能清空 Blender 场景，任务已停止。",
    actions: ["retry", "restart"],
  },
  // 建造轮连续空转被叫停：不是建坏了，是模型一直在复述自检、根本没动手
  "group-stalled": {
    text: "这一组连续几次都没有任何建造动作，已停止，避免空转。请把这一组拆分成更小的几组后重发，或直接说明这一步具体要建什么。",
    actions: ["retry"],
  },
  "max-steps-exceeded": {
    text: "步数预算用完了。前面的成果还在 Blender 场景里，点「继续工作」接着干，不会重新清场。",
    actions: [],
  },
  "user-cancelled": {
    text: "已取消。",
    actions: [],
  },
  "nothing-to-resume": {
    text: "这份任务的现场已经不在了（可能已有新任务开始），请重新发指令。",
    actions: [],
  },
  "no-task-to-retry": {
    text: "没有可重试的任务，请重新发送指令。",
    actions: [],
  },
};

const entry = computed(() => dictionary[props.reason]);

// 云失败带服务端原因时优先显示原文：它比任何模板文案都具体。
// 未收录的 reason 也要有东西可看：有原文给原文，实在没有就露出码本身，绝不给空白卡。
const text = computed(() => {
  if (props.reason === "cloud3d-failed" && props.message) return props.message;
  if (props.reason === "max-steps-exceeded") {
    const usage =
      typeof props.steps === "number" && typeof props.maxSteps === "number"
        ? `（已用 ${props.steps} / 上限 ${props.maxSteps} 步）`
        : "";
    return `${dictionary["max-steps-exceeded"].text.replace("。", `${usage}。`)}`;
  }
  if (entry.value) return entry.value.text;
  return props.message || props.reason;
});

// 已收录的码 + 带原文：原文单独一行展示，不覆盖解释文案
const detail = computed(() =>
  entry.value && props.message && text.value !== props.message ? props.message : "",
);

const actions = computed<Action[]>(() => entry.value?.actions ?? ["retry"]);
const settingsPage = computed(() => entry.value?.page);
const canRestart = computed(() => actions.value.includes("restart"));
const canSettings = computed(() => actions.value.includes("settings") && !!settingsPage.value);
const hasActions = computed(
  () => !props.resolved && (actions.value.length > 0 || props.resumable === true),
);
</script>

<template>
  <div class="error-card">
    <strong>请求失败</strong>
    <p class="error-text">{{ text }}</p>
    <p v-if="detail" class="error-detail">{{ detail }}</p>
    <div v-if="hasActions" class="error-actions">
      <button v-if="actions.includes('retry')" :disabled="busy" @click="emit('retry')">重试</button>
      <button v-if="canRestart" :disabled="busy" @click="emit('restartBridge')">重启桥接</button>
      <button v-if="canSettings" @click="emit('openSettings', settingsPage!)">打开设置</button>
      <button v-if="resumable" :disabled="busy" @click="emit('continueWork')">继续工作</button>
    </div>
  </div>
</template>

<style scoped>
.error-card {
  border: 1px solid #a04040;
  border-left-width: 3px;
  border-radius: 4px;
  background: var(--panel, #2a1f1f);
  padding: 8px 10px;
  font-size: 13px;
}

.error-card strong {
  display: block;
  color: #e06c75;
  font-size: 12px;
  margin-bottom: 4px;
}

.error-card p {
  margin: 0;
  white-space: pre-wrap;
}

.error-detail {
  margin-top: 4px !important;
  color: var(--muted);
  font-size: 12px;
}

.error-actions {
  display: flex;
  gap: 6px;
  flex-wrap: wrap;
}

.error-actions button {
  margin-top: 6px;
  background: var(--accent);
  border: 0;
  color: #fff;
  border-radius: 3px;
  padding: 4px 12px;
  font-size: 12px;
}

.error-actions button:disabled {
  opacity: 0.5;
}
</style>
