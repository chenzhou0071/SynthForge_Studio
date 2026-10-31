<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from "vue";
import type { WsClient } from "../ws/WsClient";
import { contextMenuTop } from "../ui/menuPosition";

type Area = "blender" | "unity";
type Op = "mkdir" | "rename" | "delete" | "reveal" | "refresh";

interface FileEntry {
  name: string;
  path: string;
  kind: string;
  source?: string;
  provider?: string;
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
  source?: string;
  provider?: string;
}

const props = defineProps<{ area: Area; client: WsClient }>();
const emit = defineEmits<{ select: [path: string]; modify: [path: string] }>();

const menu = ref<{ x: number; y: number; path: string; isEntry: boolean; isDir: boolean } | null>(
  null,
);
// 菜单实际顶边：弹出后按实测高度算，靠近视口底部就向上翻，避免被截断点不到
const menuEl = ref<HTMLUListElement | null>(null);
const menuTop = ref(0);
watch(menu, async (m) => {
  if (!m) return;
  await nextTick(); // 等 <ul> 挂上再量高度
  menuTop.value = contextMenuTop(m.y, menuEl.value?.offsetHeight ?? 0, window.innerHeight);
});
const message = ref<{ text: string; kind: "notice" | "error" } | null>(null);
// 提示几秒后自动消失：后台偶发的超时不该在树底常驻一条红字吓人
let messageTimer: ReturnType<typeof setTimeout> | null = null;
function flashMessage(kind: "notice" | "error", text: string, ms = 6000) {
  message.value = { kind, text };
  if (messageTimer) clearTimeout(messageTimer);
  messageTimer = setTimeout(() => {
    message.value = null;
    messageTimer = null;
  }, ms);
}
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
        source: file.source,
        provider: file.provider,
      });
    }
  };
  walk(tree.value, 0);
  return result;
});

const providerNames: Record<string, string> = {
  tripo: "Tripo",
  hunyuan: "混元",
  custom: "自定义服务",
};

function cloudTitle(provider?: string) {
  return `云生成 · ${providerNames[provider ?? ""] ?? "云服务"}`;
}

// assets.list 是平铺列表（文件 + kind=dir 的目录）：按相对路径聚合成目录树，目录在前、文件在后
function buildTree(items: unknown): DirNode {
  const root: DirNode = { name: "", path: "", dirs: [], files: [] };
  if (!Array.isArray(items)) return root;
  for (const raw of items) {
    const item = raw as {
      relativePath?: unknown;
      kind?: unknown;
      source?: unknown;
      provider?: unknown;
    } | null;
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
      source: typeof item?.source === "string" ? item.source : undefined,
      provider: typeof item?.provider === "string" ? item.provider : undefined,
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

// tree.refresh 返回的嵌套节点：{name,path,type,children?}
interface TreeNode {
  name: string;
  path: string;
  type: "dir" | "file";
  children?: TreeNode[];
}

// 把 Unity 工程树（相对 <project>/Assets）转成与 blender 模型库同形的 DirNode，复用同一套渲染
function unityToDirNode(node: TreeNode): DirNode {
  const dir: DirNode = { name: node.name, path: node.path, dirs: [], files: [] };
  for (const child of node.children ?? []) {
    if (child.type === "dir") dir.dirs.push(unityToDirNode(child));
    else
      dir.files.push({
        name: child.name,
        path: child.path,
        kind: (child.name.split(".").pop() ?? "").toLowerCase(),
      });
  }
  dir.dirs.sort((a, b) => a.name.localeCompare(b.name));
  dir.files.sort((a, b) => a.name.localeCompare(b.name));
  return dir;
}

async function loadTree() {
  const seq = ++fetchSeq;
  try {
    if (isBlender.value) {
      const items = await props.client.request<unknown>("assets.list", {});
      if (seq !== fetchSeq) return; // 连续事件只认最后一次结果
      tree.value = buildTree(items);
      return;
    }
    // Unity 区：tree.refresh 是唯一树数据源，返回以 <project>/Assets 为根的嵌套节点
    const res = await props.client.request<{ ok?: boolean; tree?: TreeNode }>("tree.refresh", {
      area: "unity",
    });
    if (seq !== fetchSeq) return;
    tree.value =
      res && res.ok && res.tree
        ? unityToDirNode(res.tree)
        : { name: "", path: "", dirs: [], files: [] };
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

// 点目录折叠/展开，点文件只预览；是否进入修改流程由右键菜单或预览区按钮决定
function onRowClick(row: Row) {
  if (row.kind === "dir") toggleDir(row);
  else emit("select", row.path);
}

function openModify() {
  const target = menu.value;
  if (!target || target.isDir) return;
  closeMenu();
  emit("modify", target.path);
}

function openMenu(event: MouseEvent, path: string, isEntry: boolean, isDir = false) {
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
    void loadTree();
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
    if (text.includes("unknown method: fs.operate")) {
      flashMessage("notice", "文件操作尚未接入（P4 开发中）");
    } else if (text.startsWith("timeout:")) {
      flashMessage("notice", "后台正忙，文件操作超时了，稍后再试一次");
    } else {
      flashMessage("error", text);
    }
  }
}
</script>

<template>
  <aside class="filetree">
    <header class="panel-head">
      <span>{{ isBlender ? "模型库" : "Unity 工程" }}</span>
    </header>

    <div class="tree-body" @contextmenu.prevent.stop="openRootMenu($event)">
      <!-- 两区共用同一套 rows 渲染：blender 是模型库，unity 是工程 Assets 树 -->
      <div
        v-for="row in rows"
        :key="row.key"
        class="tree-row"
        :class="row.kind === 'dir' ? 'tree-dir' : 'tree-file'"
        :style="{ paddingLeft: 6 + row.depth * 14 + 'px' }"
        :data-path="row.path"
        @click="onRowClick(row)"
        @contextmenu.prevent.stop="openRowMenu($event, row)"
      >
        <span class="tree-icon">{{ row.kind === "dir" ? (collapsed.has(row.path) ? "▸" : "▾") : "" }}</span>
        <span class="tree-name">{{ row.label }}</span>
        <span
          v-if="row.fileKind && isBlender"
          class="suffix"
          :class="{ muted: row.fileKind !== 'glb', cloud: row.source === 'cloud' }"
          :title="row.source === 'cloud' ? cloudTitle(row.provider) : undefined"
        >
          {{ row.source === "cloud" ? `云 · ${row.fileKind}` : row.fileKind }}
        </span>
      </div>
      <p v-if="rows.length === 0" class="tree-empty">
        {{ isBlender ? "暂无资产，右键可新建文件夹" : "尚未导入工程，或 Assets 为空" }}
      </p>

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
      ref="menuEl"
      class="ctx-menu"
      :style="{ left: menu.x + 'px', top: menuTop + 'px' }"
      @mousedown.stop
      @click.stop
    >
      <li @click="runOp('mkdir')">新建文件夹</li>
      <li :class="{ disabled: !menu.isEntry }" @click="runOp('rename')">重命名</li>
      <li :class="{ disabled: !menu.isEntry }" @click="runOp('delete')">删除</li>
      <li @click="runOp('reveal')">在资源管理器中显示</li>
      <li @click="runOp('refresh')">刷新</li>
      <li v-if="menu.isEntry && !menu.isDir && isBlender" @click="openModify">修改此资产</li>
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

/* 云生成来源：蓝字蓝边，和本地导出的灰标签区分 */
.suffix.cloud {
  color: #4daafc;
  border-color: rgba(77, 170, 252, 0.45);
  background: rgba(77, 170, 252, 0.12);
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
