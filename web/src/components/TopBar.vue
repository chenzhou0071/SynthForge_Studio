<script setup lang="ts">
type LampState = "off" | "ready" | "reconnecting";

const props = defineProps<{
  area: string;
  blender: LampState;
  unity: LampState;
  hints?: { blender?: string; unity?: string };
  importing?: boolean;
}>();

const emit = defineEmits<{ (e: "importProject"): void }>();

const color = (state: LampState) =>
  state === "ready" ? "#4ec9b0" : state === "reconnecting" ? "#cca700" : "#555";

const label = (state: LampState) =>
  state === "ready" ? "就绪" : state === "reconnecting" ? "重连中" : "未连接";

const tip = (area: "blender" | "unity") =>
  props.hints?.[area] ?? `${area === "blender" ? "Blender" : "Unity"}：${label(props[area])}`;
</script>

<template>
  <header class="topbar">
    <span v-if="area === 'blender'" class="fixed-path">模型库：文档/SynthForgeStudio/ModelLibrary（固定）</span>
    <button v-else class="import" :disabled="importing" @click="emit('importProject')">导入 Unity 工程</button>
    <span class="spacer" />
    <span class="lamp" :title="tip('blender')"><i :style="{ background: color(blender) }" />Blender</span>
    <span class="lamp" :title="tip('unity')"><i :style="{ background: color(unity) }" />Unity</span>
  </header>
</template>
