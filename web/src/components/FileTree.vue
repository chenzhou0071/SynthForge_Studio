<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from "vue";
import type { WsClient } from "../ws/WsClient";

type Area = "blender" | "unity";
type Op = "mkdir" | "rename" | "delete" | "reveal" | "refresh";

interface FileEntry {
  name: string;
  path: string;
  kind: string;
}

interface DirNode {
  name: string;
  path: string;
  dirs: DirNode[];
  files: FileEntry[];
}

interface Row {
  key: string;
  depth: number;
  kind: "dir" | "file";
  label: string;
  path: string;
  fileKind?: string;
}

const props = defineProps<{ area: Area; client: WsClient }>();

const menu = ref<{ x: number; y: number; path: string; isEntry: boolean; isDir: boolean } | null>(
  null,
);
const message = ref<{ text: string; kind: "notice" | "error" } | null>(null);
const tree = ref<DirNode>({ name: "", path: "", dirs: [], files: [] });
// 默认全展开，只记收起的目录；树刷新后已有目录的展开状态不会被重置
const collapsed = ref(new Set<string>());
const unsubscribers: Array<() => void> = [];
let fetchSeq = 0;

const isBlender = computed(() => props.area === "blender");

const rows = computed<Row[]>(() => {
  const result: Row[] = [];
  const walk = (node: DirNode, depth: number) => {
    for (const dir of node.dirs) {
      result.push({ key: `d:${dir.path}`, depth, kind: "dir", label: dir.name, path: dir.path });
      if (!collapsed.value.has(dir.path)) walk(dir, depth + 1);
    }
    for (const file of node.files) {
      result.push({
        key: `f:${file.path}`,
        depth,
        kind: "file",
        label: file.name,
        path: file.path,
        fileKind: file.kind,
      });
    }
  };
  walk(tree.value, 0);
  return result;
});

// assets.list 是平铺列表（文件 + kind=dir 的目录）：按相对路径聚合成目录树，目录在前、文件在后
function buildTree(items: unknown): DirNode {
  const root: DirNode = { name: "", path: "", dirs: [], files: [] };
  if (!Array.isArray(items)) return root;
  for (const raw of items) {
    const item = raw as { relativePath?: unknown; kind?: unknown } | null;
    const relative = typeof item?.relativePath === "string" ? item.relativePath : "";
    const parts = relative.split("/").filter(Boolean);
    if (parts.length === 0) continue;
    const isDir = item?.kind === "dir";

    let dir = root;
    for (const part of isDir ? parts : parts.slice(0, -1)) {
      let next = dir.dirs.find((candidate) => candidate.name === part);
      if (!next) {
        next = { name: part, path: dir.path ? `${dir.path}/${part}` : part, dirs: [], files: [] };
        dir.dirs.push(next);
      }
      dir = next;
    }
    if (isDir) continue; // 目录条目只负责把节点建出来，不产生文件行

    const fileName = parts[parts.length - 1];
    dir.files.push({
      name: fileName,
      path: relative,
      kind: (typeof item?.kind === "string" ? item.kind : (fileName.split(".").pop() ?? ""))
        .toLowerCase(),
    });
  }

  const sortNode = (node: DirNode) => {
    node.dirs.sort((a, b) => a.name.localeCompare(b.name));
    node.files.sort((a, b) => a.name.localeCompare(b.name));
    node.dirs.forEach(sortNode);
  };
  sortNode(root);
  return root;
}

async function loadTree() {
  if (!isBlender.value) return;
  const seq = ++fetchSeq;
  try {
    const items = await props.client.request<unknown>("assets.list", {});
    if (seq !== fetchSeq) return; // 连续事件只认最后一次结果
    tree.value = buildTree(items);
  } catch (cause) {
    if (seq !== fetchSeq) return;
    message.value = { kind: "error", text: cause instanceof Error ? cause.message : String(cause) };
  }
}

function toggleDir(row: Row) {
  if (row.kind !== "dir") return;
  if (collapsed.value.has(row.path)) collapsed.value.delete(row.path);
  else collapsed.value.add(row.path);
}

function openMenu(event: MouseEvent, path: string, isEntry: boolean, isDir = false) {
  if (!isBlender.value) return;
  message.value = null;
  menu.value = { x: event.clientX, y: event.clientY, path, isEntry, isDir };
}

function closeMenu() {
  menu.value = null;
}

function openRowMenu(event: MouseEvent, row: Row) {
  openMenu(event, row.path, true, row.kind === "dir");
}

// 树体空白区就是模型库根目录；根路径用空串，与 AssetLibrary 的 isValidTarget("") 约定一致
function openRootMenu(event: MouseEvent) {
  openMenu(event, "", false, true);
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
  unsubscribers.push(
    props.client.on("fileTree.changed", (data) => {
      if ((data as { area?: string }).area === props.area) void loadTree();
    }),
  );
  void loadTree();
});

onBeforeUnmount(() => {
  window.removeEventListener("mousedown", onMouseDown);
  window.removeEventListener("keydown", onKeydown);
  unsubscribers.forEach((off) => off());
});

watch(
  () => props.area,
  () => {
    closeMenu();
    message.value = null;
    collapsed.value = new Set();
    if (isBlender.value) void loadTree();
    else tree.value = { name: "", path: "", dirs: [], files: [] };
  },
);

function parentOf(path: string) {
  return path.split("/").slice(0, -1).join("/");
}

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

  // 新建文件夹落在“选中的目录/文件所在目录”，重命名和删除落在条目本身
  const path =
    op === "mkdir" && target.isEntry && !target.isDir ? parentOf(target.path) : target.path;
  const params: Record<string, unknown> = { area: props.area, op, path };
  if (name !== undefined) params.name = name;
  try {
    const res = await props.client.request<{ ok?: boolean; error?: string }>("fs.operate", params);
    if (res?.ok === false) {
      message.value = { kind: "error", text: res.error ?? "操作失败" };
      return;
    }
    // 目录/文件变了就重拉一次；打开资源管理器不动树
    if (op !== "reveal") void loadTree();
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
        <!-- 打开的就是模型库根目录，不再显示同名根节点 -->
        <div
          v-for="row in rows"
          :key="row.key"
          class="tree-row"
          :class="row.kind === 'dir' ? 'tree-dir' : 'tree-file'"
          :style="{ paddingLeft: 6 + row.depth * 14 + 'px' }"
          :data-path="row.path"
          @click="toggleDir(row)"
          @contextmenu.prevent.stop="openRowMenu($event, row)"
        >
          <span class="tree-icon">{{ row.kind === "dir" ? (collapsed.has(row.path) ? "▸" : "▾") : "" }}</span>
          <span class="tree-name">{{ row.label }}</span>
          <span v-if="row.fileKind" class="suffix" :class="{ muted: row.fileKind !== 'glb' }">
            {{ row.fileKind }}
          </span>
        </div>
        <p v-if="rows.length === 0" class="tree-empty">暂无资产，右键可新建文件夹</p>
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
.tree-row {
  display: flex;
  align-items: center;
  gap: 4px;
  padding: 3px 6px;
  font-size: 12px;
  line-height: 1.6;
  white-space: nowrap;
  overflow: hidden;
}

.tree-row:hover {
  background: var(--panel-alt);
}

.tree-dir {
  cursor: pointer;
  color: var(--text);
}

.tree-file {
  color: var(--text);
}

.tree-icon {
  flex: none;
  width: 18px;
  color: var(--muted);
  font-size: 18px;
  line-height: 1;
}

.tree-name {
  overflow: hidden;
  text-overflow: ellipsis;
}

.suffix {
  flex: none;
  margin-left: auto;
  padding: 0 4px;
  border: 1px solid var(--border);
  border-radius: 3px;
  color: var(--muted);
  font-size: 10px;
}

.suffix.muted {
  opacity: 0.55;
}

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
