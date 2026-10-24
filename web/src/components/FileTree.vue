<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from "vue";
import type { WsClient } from "../ws/WsClient";

type Area = "blender" | "unity";
type Op = "mkdir" | "rename" | "delete" | "reveal" | "refresh";

const props = defineProps<{ area: Area; client: WsClient }>();

const menu = ref<{ x: number; y: number; path: string; isEntry: boolean } | null>(null);
const message = ref<{ text: string; kind: "notice" | "error" } | null>(null);

const isBlender = computed(() => props.area === "blender");

function openMenu(event: MouseEvent, path: string, isEntry: boolean) {
  if (!isBlender.value) return;
  message.value = null;
  menu.value = { x: event.clientX, y: event.clientY, path, isEntry };
}

function closeMenu() {
  menu.value = null;
}

// 树数据源在 P4/P6 接入；现在树体空白区就是模型库根目录
// 根路径用空串，与 AssetLibrary 的 isValidTarget("") 约定一致
function openRootMenu(event: MouseEvent) {
  openMenu(event, "", false);
}

function onMouseDown(event: MouseEvent) {
  if (!menu.value) return;
  const target = event.target as Element | null;
  if (target?.closest(".ctx-menu")) return;
  closeMenu();
}

function onKeydown(event: KeyboardEvent) {
  if (event.key === "Escape") closeMenu();
}

onMounted(() => {
  window.addEventListener("mousedown", onMouseDown);
  window.addEventListener("keydown", onKeydown);
});

onBeforeUnmount(() => {
  window.removeEventListener("mousedown", onMouseDown);
  window.removeEventListener("keydown", onKeydown);
});

watch(
  () => props.area,
  () => {
    closeMenu();
    message.value = null;
  },
);

async function runOp(op: Op) {
  const target = menu.value;
  if (!target) return;
  // 根不是文件/文件夹：重命名、删除会作用到模型库本身，直接拒绝
  if (!target.isEntry && (op === "rename" || op === "delete")) return;

  let name: string | undefined;
  if (op === "mkdir" || op === "rename") {
    const answer = window.prompt(op === "mkdir" ? "新建文件夹名称" : "重命名为", "");
    if (!answer) return closeMenu();
    name = answer;
  }
  if (op === "delete" && !window.confirm(`确定删除 ${target.path} ?`)) return closeMenu();
  closeMenu();

  // fs.operate 的 C++ handler 在 P4（Unity 在 P6）；现在会返回 unknown method
  const params: Record<string, unknown> = { area: props.area, op, path: target.path };
  if (name !== undefined) params.name = name;
  try {
    await props.client.request("fs.operate", params);
  } catch (cause) {
    const text = cause instanceof Error ? cause.message : String(cause);
    message.value = text.includes("unknown method: fs.operate")
      ? { kind: "notice", text: "文件操作尚未接入（P4 开发中）" }
      : { kind: "error", text };
  }
}
</script>

<template>
  <aside class="filetree">
    <header class="panel-head">
      <span>{{ isBlender ? "模型库" : "Unity 工程" }}</span>
    </header>

    <div class="tree-body" @contextmenu.prevent.stop="openRootMenu($event)">
      <template v-if="isBlender">
        <!-- 打开的就是模型库根目录，不再显示同名根节点；P4 在此渲染真实文件树 -->
        <p class="tree-empty">暂无资产，右键可新建文件夹</p>
      </template>
      <template v-else>
        <p class="tree-root">尚未导入工程</p>
        <p class="tree-empty">导入 Unity 工程后在此浏览资源。</p>
      </template>

      <p
        v-if="message"
        class="tree-message"
        :class="message.kind === 'error' ? 'tree-error' : 'tree-note'"
      >
        {{ message.text }}
      </p>
    </div>

    <ul
      v-if="menu"
      class="ctx-menu"
      :style="{ left: menu.x + 'px', top: menu.y + 'px' }"
      @mousedown.stop
      @click.stop
    >
      <li @click="runOp('mkdir')">新建文件夹</li>
      <li :class="{ disabled: !menu.isEntry }" @click="runOp('rename')">重命名</li>
      <li :class="{ disabled: !menu.isEntry }" @click="runOp('delete')">删除</li>
      <li @click="runOp('reveal')">在资源管理器中显示</li>
      <li @click="runOp('refresh')">刷新</li>
    </ul>
  </aside>
</template>

<style scoped>
.tree-message {
  margin: 8px 0 0;
  font-size: 12px;
  line-height: 1.5;
  word-break: break-all;
}

.tree-note {
  color: var(--muted);
}

.tree-error {
  color: #f48771;
}

/* fixed：坐标直接用鼠标的 clientX/clientY，不受面板滚动影响 */
.ctx-menu {
  position: fixed;
  z-index: 20;
  margin: 0;
  padding: 4px 0;
  list-style: none;
  min-width: 160px;
  background: var(--panel-alt);
  border: 1px solid var(--border);
  border-radius: 4px;
  box-shadow: 0 6px 18px rgba(0, 0, 0, 0.45);
}

.ctx-menu li {
  padding: 6px 14px;
  font-size: 13px;
  cursor: pointer;
}

.ctx-menu li:hover {
  background: var(--accent);
  color: #fff;
}

.ctx-menu li.disabled,
.ctx-menu li.disabled:hover {
  background: transparent;
  color: var(--muted);
  cursor: default;
}
</style>
