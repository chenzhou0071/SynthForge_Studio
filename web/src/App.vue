<script setup lang="ts">
import { computed, onMounted, ref } from "vue";
import ActivityBar from "./components/ActivityBar.vue";
import ChatPanel from "./components/ChatPanel.vue";
import FileTree from "./components/FileTree.vue";
import PreviewPane from "./components/PreviewPane.vue";
import SettingsDialog from "./components/SettingsDialog.vue";
import TopBar from "./components/TopBar.vue";
import { WsClient } from "./ws/WsClient";

type Area = "blender" | "unity";
type LampState = "off" | "ready" | "reconnecting";

const client = new WsClient();
const active = ref<Area>("blender");
const settingsOpen = ref(false);
const importing = ref(false);
const lamp = ref<{ blender: LampState; unity: LampState }>({ blender: "off", unity: "off" });

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
        <FileTree :area="active" />
        <PreviewPane v-if="active === 'blender'" />
        <ChatPanel v-show="active === 'blender'" area="blender" :client="client" />
        <ChatPanel v-show="active === 'unity'" area="unity" :client="client" />
      </div>
      <!-- 两个 ChatPanel 均为常驻实例：切区只 v-show，不卸载、不重建、不丢后台事件 -->
    </div>
    <SettingsDialog v-model:open="settingsOpen" :client="client" />
  </div>
</template>
