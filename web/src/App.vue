<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from "vue";
import ActivityBar from "./components/ActivityBar.vue";
import ChatPanel from "./components/ChatPanel.vue";
import FileTree from "./components/FileTree.vue";
import PreviewPane from "./components/PreviewPane.vue";
import SettingsDialog from "./components/SettingsDialog.vue";
import TopBar from "./components/TopBar.vue";
import type { PreviewFile } from "./preview/file";
import { waitForStableFile } from "./preview/stability";
import { suppressNativeContextMenu } from "./ui/contextmenu";
import { WsClient } from "./ws/WsClient";

type Area = "blender" | "unity";
type LampState = "off" | "ready" | "reconnecting";

const client = new WsClient();
const active = ref<Area>("blender");
const settingsOpen = ref(false);
// 「去设置」根据错误类型直达 Agent / 3D 模型页
const settingsTab = ref<"agent" | "cloud3d">("agent");
const picking = ref(false);
const importError = ref("");
// project.state 事件：{path,name,state,waiting?,attempt?}，用于导入横幅与等待提示
const projectState = ref<{
  path?: string;
  name?: string;
  state?: string;
  waiting?: boolean;
  attempt?: number;
} | null>(null);
// 预览内容由 file.read 填充；asset.created 到达时自动打开新资产
const previewFile = ref<PreviewFile | null>(null);
// 导出回合：glb 还在写，预览先挂“导出中”，等 asset.created 再自动加载
const exporting = ref(false);
const showExporting = computed(() => active.value === "blender" && exporting.value);
let previewSeq = 0; // 连续点文件时只让最后一次等待生效
// 已归一化到 .blend 的修改目标；Task 6 会把它作为 chip 传给 ChatPanel
const modifyTarget = ref<string | null>(null);
// 选中资产的产出形态：ChatPanel 据此把三档开关锁到该档（未知则不锁）
const modifyTargetShape = ref<"prop" | "diorama" | "open" | undefined>(undefined);
// 设置保存后递增，让常驻的 ChatPanel 重新拉取云API 徽标状态
const settingsVersion = ref(0);
const lamp = ref<{ blender: LampState; unity: LampState }>({ blender: "off", unity: "off" });
let restoreNativeMenu: (() => void) | null = null;
let offAssetCreated: (() => void) | null = null;
let offExportStarted: (() => void) | null = null;
let offExportFinished: (() => void) | null = null;
let offErrorCard: (() => void) | null = null;
let offProjectState: (() => void) | null = null;

const lampHints = computed(() => ({
  blender: `Blender：${stateLabel(lamp.value.blender)}`,
  unity: `Unity：${stateLabel(lamp.value.unity)}`,
}));

function stateLabel(state: LampState) {
  if (state === "ready") return "就绪";
  if (state === "reconnecting") return "重连中";
  return "未连接";
}

function selectArea(area: Area) {
  active.value = area;
}

function newRequestId() {
  return typeof crypto !== "undefined" && crypto.randomUUID
    ? crypto.randomUUID()
    : `r-${Date.now()}-${Math.random().toString(16).slice(2)}`;
}

// 先订阅再发请求：requestId 由前端生成，事件即使早于响应到达也不会丢
function waitDialogResult(requestId: string) {
  let off = () => {};
  let timer: ReturnType<typeof setTimeout>;
  const promise = new Promise<string | null>((resolve, reject) => {
    timer = setTimeout(() => { off(); reject(new Error("dialog-timeout")); }, 10 * 60 * 1000);
    off = client.on("dialog.result", (d) => {
      const evt = d as { requestId?: string; cancelled?: boolean; path?: string };
      if (evt.requestId !== requestId) return;
      clearTimeout(timer);
      off();
      resolve(evt.cancelled ? null : evt.path ?? null);
    });
  });
  return { promise, cancel: () => { clearTimeout(timer); off(); } };
}

async function importProject() {
  if (picking.value) return;
  picking.value = true;
  importError.value = "";
  const requestId = newRequestId();
  const waiter = waitDialogResult(requestId);
  try {
    const res = (await client.request("dialog.pickFolder", { purpose: "unityProject", requestId })) as
      { accepted?: boolean; reason?: string };
    if (!res.accepted) {
      waiter.cancel();
      if (res.reason !== "busy")
        importError.value = res.reason === "dialog-unavailable"
          ? "无法打开文件夹选择框" : String(res.reason);
      return;
    }
    const path = await waiter.promise;
    if (path) await selectProject(path);              // 取消：静默
  } catch (e) {
    waiter.cancel();
    const msg = e instanceof Error ? e.message : String(e);
    importError.value = msg === "dialog-timeout" ? "文件夹选择超时，请重试" : (msg || "导入失败");
  } finally {
    picking.value = false;
  }
}

async function selectProject(path: string) {
  const res = (await client.request("project.select", { path })) as {
    accepted?: boolean; reason?: string; runningTasks?: number; newPath?: string;
  };
  if (res.accepted) return;
  if (res.reason === "switch-confirm") {
    if (!window.confirm(`当前工程有 ${res.runningTasks ?? 0} 个任务运行中，切换将终止这些任务`)) return;
    await client.request("project.switchConfirm", { path: res.newPath });
    return;
  }
  importError.value = res.reason === "invalid-project"
    ? "不是有效 Unity 工程（需含 Assets 与 ProjectSettings）"
    : String(res.reason ?? "导入失败");
}

function openSettings(target: "agent" | "cloud") {
  settingsTab.value = target === "cloud" ? "cloud3d" : "agent";
  settingsOpen.value = true;
}

function previewAsset(path: string) {
  void openPreview(path, true);
}

async function readFile(path: string, area: Area = active.value) {
  return (await client.request("file.read", { area, path })) as {
    ok?: boolean; kind?: PreviewFile["kind"]; content?: string; dataUrl?: string;
    language?: string; size?: number; modified?: string; blendPath?: string; shape?: string;
    error?: string;
  };
}

// 点文件只预览，不再等于修改（P4 的语义变更）
async function openPreview(path: string, notifyMissing = false) {
  const area = active.value;
  const seq = ++previewSeq;
  // 导出中不加载半成品；asset.created 到达后会自动打开完整文件
  if (area === "blender" && exporting.value) return;

  const first = await readFile(path, area);
  if (!first.ok || !first.kind) {
    // 资产卡上的文件可能已被手动删除/移动，点「查看」要给出明确提示
    if (notifyMissing && seq === previewSeq) {
      window.alert("无法预览：文件不存在或已被移动，请刷新文件树后重试。");
    }
    return;
  }
  if (seq !== previewSeq) return;

  if (first.kind === "model") {
    // 导出的 glb 会边写边变大：连续 300ms 大小与修改时间不变才交给 three.js
    const stable = await waitForStableFile(async () => {
      const res = await readFile(path, area);
      if (!res.ok || res.kind !== "model" || res.size === undefined || !res.modified)
        return null;
      return { size: res.size, modified: res.modified };
    });
    if (!stable || seq !== previewSeq) return;
    const latest = await readFile(path, area);
    if (seq !== previewSeq || !latest.ok || !latest.kind) return;
    previewFile.value = {
      path,
      kind: latest.kind,
      content: latest.content,
      dataUrl: latest.dataUrl,
      language: latest.language,
      size: latest.size,
      modified: latest.modified,
      blendPath: latest.blendPath,
    };
    return;
  }

  previewFile.value = {
    path,
    kind: first.kind,
    content: first.content,
    dataUrl: first.dataUrl,
    language: first.language,
    size: first.size,
    modified: first.modified,
    blendPath: first.blendPath,
  };
}

// 修改入口：预览区按钮与树右键菜单共用。没有 .blend 时明确告知只能重新生成
async function requestModify(path: string) {
  const res = await readFile(path);
  if (!res.ok) return;
  if (!res.blendPath) {
    window.alert(
      "无法修改此资产\n\n"
      + "未找到同名的 .blend 源文件。\n"
      + "3D 高模（云 API 生成）与外部导入的模型不支持对话修改，只能重新生成。",
    );
    return;
  }
  modifyTarget.value = res.blendPath; // 输入框上方出现 chip，随下一条消息发给 chat.send
  modifyTargetShape.value =
    res.shape === "prop" || res.shape === "diorama" || res.shape === "open"
      ? res.shape
      : undefined;
}

// 取消修改目标（ChatPanel 的 chip 关闭 / 提交后）：连带清掉锁定的档位
function clearModify() {
  modifyTarget.value = null;
  modifyTargetShape.value = undefined;
}

// 切区后旧区预览不再适用，清掉避免 Unity 区显示 Blender 资产
watch(active, () => {
  previewFile.value = null;
  modifyTarget.value = null;
  modifyTargetShape.value = undefined;
  previewSeq++; // 旧区的等待结果作废
});

onMounted(async () => {
  // 只放行输入框的复制/粘贴菜单，页面其他位置不弹 Chromium 原生菜单
  restoreNativeMenu = suppressNativeContextMenu(document);
  const params = new URLSearchParams(location.search);
  try {
    await client.connect(params.get("ws") ?? "ws://127.0.0.1:0");
  } catch {
    // 后端未启动时保持双灯灰，页面骨架仍可用。
  }
  client.on("conn.status", (data) => {
    const next = data as { blender?: LampState; unity?: LampState };
    lamp.value = {
      blender: next.blender ?? lamp.value.blender,
      unity: next.unity ?? lamp.value.unity,
    };
  });
  offProjectState = client.on("project.state", (data) => {
    projectState.value = data as {
      path?: string; name?: string; state?: string; waiting?: boolean; attempt?: number;
    };
  });
  offExportStarted = client.on("export.started", (data) => {
    const evt = data as { area?: string };
    if (evt.area === "blender") exporting.value = true;
  });
  offExportFinished = client.on("export.finished", (data) => {
    const evt = data as { area?: string };
    if (evt.area === "blender") exporting.value = false;
  });
  offErrorCard = client.on("error.card", (data) => {
    const evt = data as { area?: string };
    if (evt.area === "blender") exporting.value = false; // 导出失败取消也要解锁
  });
  // 生成完成自动加载新资产（规格 4.5）：只认 blender 区且带相对路径的事件
  offAssetCreated = client.on("asset.created", (data) => {
    const created = data as { area?: string; path?: string };
    if (created.area === "blender") exporting.value = false;
    if (active.value !== "blender" || created.area !== "blender" || !created.path) return;
    void openPreview(created.path);
  });
});

onBeforeUnmount(() => {
  restoreNativeMenu?.();
  offAssetCreated?.();
  offExportStarted?.();
  offExportFinished?.();
  offErrorCard?.();
  offProjectState?.();
});
</script>

<template>
  <div class="app">
    <ActivityBar :active="active" @select="selectArea" @open-settings="settingsOpen = true" />
    <div class="main">
      <TopBar
        :area="active"
        :blender="lamp.blender"
        :unity="lamp.unity"
        :hints="lampHints"
        :importing="picking"
        @import-project="importProject"
      />
      <p v-if="importError" class="import-error">{{ importError }}</p>
      <div
        v-if="active === 'unity' && projectState?.state === 'activating'"
        class="project-banner"
      >
        等待 Unity 启动（第 {{ projectState.attempt ?? 1 }} 次尝试）…
      </div>
      <div
        v-else-if="active === 'unity' && projectState?.state === 'active'"
        class="project-banner"
      >
        当前工程：{{ projectState.name }}
      </div>
      <div class="content">
        <FileTree :area="active" :client="client" @select="openPreview" @modify="requestModify" />
        <PreviewPane
          :area="active"
          :file="previewFile"
          :exporting="showExporting"
          @modify="requestModify"
          @close="previewFile = null"
        />
        <ChatPanel
          v-show="active === 'blender'"
          area="blender"
          :client="client"
          :settings-version="settingsVersion"
          :selected-asset="modifyTarget ?? undefined"
          :selected-asset-shape="modifyTargetShape"
          @clear-asset="clearModify"
          @preview="previewAsset"
          @open-settings="openSettings"
        />
        <ChatPanel
          v-show="active === 'unity'"
          area="unity"
          :client="client"
          :settings-version="settingsVersion"
          @open-settings="openSettings"
        />
      </div>
      <!-- 两个 ChatPanel 均为常驻实例：切区只 v-show，不卸载、不重建、不丢后台事件 -->
    </div>
    <SettingsDialog
      v-model:open="settingsOpen"
      :client="client"
      :initial-tab="settingsTab"
      @saved="settingsVersion++"
    />
  </div>
</template>
