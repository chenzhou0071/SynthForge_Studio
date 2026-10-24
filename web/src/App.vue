<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from "vue";
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
// 预览内容由 P4 的 asset.created / file.read 填充；P3 只把契约与两区预览位接好
const previewFile = ref<PreviewFile | null>(null);
// 设置保存后递增，让常驻的 ChatPanel 重新拉取云API 徽标状态
const settingsVersion = ref(0);
const lamp = ref<{ blender: LampState; unity: LampState }>({ blender: "off", unity: "off" });
let restoreNativeMenu: (() => void) | null = null;

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
});

onBeforeUnmount(() => restoreNativeMenu?.());
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
        <FileTree :area="active" :client="client" />
        <PreviewPane :area="active" :file="previewFile" />
        <ChatPanel
          v-show="active === 'blender'"
          area="blender"
          :client="client"
          :settings-version="settingsVersion"
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
