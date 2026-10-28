<script lang="ts">
export interface FolderNode {
  name: string;
  path: string;
  children?: FolderNode[];
}
</script>

<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from "vue";
import type { WsClient } from "../ws/WsClient";

const props = defineProps<{
  client: WsClient;
  area: string;
  requestId: string;
  rootName: string;
  tree: FolderNode;
}>();

const emit = defineEmits<{ (e: "close", started?: boolean): void }>();

// folderPick.mkdir 返回新树时在这里替换；selected 用相对路径（根目录为空串）
const nodes = ref<FolderNode>(props.tree);
const selected = ref("");
const error = ref("");
const submitting = ref(false);

const rows = computed(() => {
  const list: Array<{ node: FolderNode; depth: number }> = [];
  const walk = (node: FolderNode, depth: number) => {
    list.push({ node, depth });
    for (const child of node.children ?? []) walk(child, depth + 1);
  };
  walk(nodes.value, 0);
  return list;
});

const errorText: Record<string, string> = {
  "blender-busy": "Blender 正忙：请等当前建模/转换完成后再确认",
  "invalid-path": "目标目录不可用：请换一个子文件夹",
  "request-not-found": "这次选择已失效：请重新点开始",
};

function pick(node: FolderNode) {
  selected.value = node.path;
  error.value = "";
}

async function makeFolder() {
  const name = window.prompt("新建文件夹名称");
  if (!name) return;
  try {
    const res = await props.client.request<{ ok?: boolean; error?: string; tree?: FolderNode }>(
      "folderPick.mkdir",
      { requestId: props.requestId, path: selected.value, name },
    );
    if (res?.ok === false || !res?.tree) {
      error.value =
        res?.error === "already-exists" ? "同名文件夹已存在" : (errorText[res?.error ?? ""] ?? "新建失败");
      return;
    }
    nodes.value = res.tree;
    selected.value = selected.value ? `${selected.value}/${name}` : name; // 新建后直接选中它
    error.value = "";
  } catch (e) {
    error.value = e instanceof Error ? e.message : "新建失败";
  }
}

async function confirm() {
  if (submitting.value) return;
  submitting.value = true;
  error.value = "";
  try {
    const res = await props.client.request<{ ok?: boolean; error?: string }>("folderPick.submit", {
      requestId: props.requestId,
      relativePath: selected.value,
    });
    if (res?.ok === false) {
      // 弹窗不关：blender-busy 可空闲后重试，invalid-path 需要换目录
      error.value = errorText[res.error ?? ""] ?? res.error ?? "确认失败";
      return;
    }
    emit("close", true); // 已确认开工：父组件据此立刻显示转圈
  } catch (e) {
    error.value = e instanceof Error ? e.message : "确认失败";
  } finally {
    submitting.value = false;
  }
}

function cancel() {
  emit("close");
  props.client.request("folderPick.cancel", { requestId: props.requestId }).catch(() => {
    // 取消失败不阻断 UI：弹窗已关，pendingPick 会在下一次确认时被覆盖
  });
}

// 选目录是流程中间步骤：只接受显式关闭（取消/×/Esc）。
// 点遮罩不关闭——激活窗口的那一下会落在全屏遮罩上，误触就等于取消整个任务。
function onKeydown(event: KeyboardEvent) {
  if (event.key === "Escape") {
    event.preventDefault();
    cancel();
  }
}

onMounted(() => window.addEventListener("keydown", onKeydown));
onBeforeUnmount(() => window.removeEventListener("keydown", onKeydown));
</script>

<template>
  <div class="dialog-mask">
    <div class="dialog folder-picker" role="dialog" aria-modal="true" aria-label="选择存放位置">
      <header class="dialog-head">
        <h2>选择存放位置</h2>
        <button class="icon" title="关闭" @click="cancel">×</button>
      </header>
      <p class="pick-hint">模型文件（glb / fbx / blend）会保存到 {{ rootName }} 下的这个文件夹。</p>
      <div class="pick-tree">
        <button
          v-for="(row, index) in rows"
          :key="row.node.path || `root-${index}`"
          class="folder-row"
          :class="{ active: row.node.path === selected }"
          :style="{ paddingLeft: `${8 + row.depth * 14}px` }"
          @click="pick(row.node)"
        >
          {{ row.node.name }}
        </button>
      </div>
      <div v-if="error" class="pick-error">{{ error }}</div>
      <div class="pick-actions">
        <button class="ghost" @click="makeFolder">新建文件夹</button>
        <span class="spacer" />
        <button class="ghost" @click="cancel">取消</button>
        <button class="primary" :disabled="submitting" @click="confirm">确认</button>
      </div>
    </div>
  </div>
</template>

<style scoped>
.folder-picker {
  width: min(420px, 88vw);
}

.pick-hint {
  margin: 0 0 8px;
  color: var(--muted);
  font-size: 12px;
}

.pick-tree {
  max-height: 320px;
  overflow: auto;
  border: 1px solid var(--border);
  border-radius: 4px;
  padding: 4px 0;
  margin-bottom: 8px;
}

.folder-row {
  display: block;
  width: 100%;
  text-align: left;
  background: transparent;
  border: 0;
  border-radius: 0;
  color: var(--text);
  font-size: 13px;
  padding: 4px 8px;
}

.folder-row:hover {
  background: rgba(255, 255, 255, 0.05);
}

.folder-row.active {
  background: var(--accent);
  color: #fff;
}

.pick-error {
  margin-bottom: 6px;
  color: #f48771;
  font-size: 12px;
}

.pick-actions {
  display: flex;
  align-items: center;
  gap: 8px;
}

.spacer {
  flex: 1;
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
