<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from "vue";
import ActivityBar from "./components/ActivityBar.vue";
import ChatPanel from "./components/ChatPanel.vue";
import FileTree from "./components/FileTree.vue";
import PreviewPane from "./components/PreviewPane.vue";
import SettingsDialog from "./components/SettingsDialog.vue";
import TopBar from "./components/TopBar.vue";
import type { PreviewFile } from "./preview/file";
import { suppressNativeContextMenu } from "./ui/contextmenu";
import { WsClient } from "./ws/WsClient";

type Area = "blender" | "unity";
type LampState = "off" | "ready" | "reconnecting";

const client = new WsClient();
const active = ref<Area>("blender");
const settingsOpen = ref(false);
const importing = ref(false);
// 预览内容由 file.read 填充；asset.created 到达时自动打开新资产
const previewFile = ref<PreviewFile | null>(null);
// 已归一化到 .blend 的修改目标；Task 6 会把它作为 chip 传给 ChatPanel
const modifyTarget = ref<string | null>(null);
// 设置保存后递增，让常驻的 ChatPanel 重新拉取云API 徽标状态
const settingsVersion = ref(0);
const lamp = ref<{ blender: LampState; unity: LampState }>({ blender: "off", unity: "off" });
let restoreNativeMenu: (() => void) | null = null;
let offAssetCreated: (() => void) | null = null;

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

function requestImport() {
  // Unity 工程导入在 P6 接入；P3 只保留入口与按钮状态。
}

async function readFile(path: string) {
  return (await client.request("file.read", { area: active.value, path })) as {
    ok?: boolean; kind?: PreviewFile["kind"]; content?: string; dataUrl?: string;
    language?: string; size?: number; modified?: string; blendPath?: string; error?: string;
  };
}

// 点文件只预览，不再等于修改（P4 的语义变更）
async function openPreview(path: string) {
  const res = await readFile(path);
  if (!res.ok || !res.kind) return;
  previewFile.value = {
    path,
    kind: res.kind,
    content: res.content,
    dataUrl: res.dataUrl,
    language: res.language,
    size: res.size,
    modified: res.modified,
    blendPath: res.blendPath,
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
}

// 切区后旧区预览不再适用，清掉避免 Unity 区显示 Blender 资产
watch(active, () => {
  previewFile.value = null;
  modifyTarget.value = null;
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
  // 生成完成自动加载新资产（规格 4.5）：只认 blender 区且带相对路径的事件
  offAssetCreated = client.on("asset.created", (data) => {
    const created = data as { area?: string; path?: string };
    if (active.value !== "blender" || created.area !== "blender" || !created.path) return;
    void openPreview(created.path);
  });
});

onBeforeUnmount(() => {
  restoreNativeMenu?.();
  offAssetCreated?.();
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
        :importing="importing"
        @import-project="requestImport"
      />
      <div class="content">
        <FileTree :area="active" :client="client" @select="openPreview" @modify="requestModify" />
        <PreviewPane :area="active" :file="previewFile" @modify="requestModify" @close="previewFile = null" />
        <ChatPanel
          v-show="active === 'blender'"
          area="blender"
          :client="client"
          :settings-version="settingsVersion"
          :selected-asset="modifyTarget ?? undefined"
          @clear-asset="modifyTarget = null"
        />
        <ChatPanel
          v-show="active === 'unity'"
          area="unity"
          :client="client"
          :settings-version="settingsVersion"
        />
      </div>
      <!-- 两个 ChatPanel 均为常驻实例：切区只 v-show，不卸载、不重建、不丢后台事件 -->
    </div>
    <SettingsDialog v-model:open="settingsOpen" :client="client" @saved="settingsVersion++" />
  </div>
</template>
