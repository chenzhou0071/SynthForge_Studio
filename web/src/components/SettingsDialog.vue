<script setup lang="ts">
import type { WsClient } from "../ws/WsClient";

defineProps<{ open: boolean; client: WsClient }>();

const emit = defineEmits<{ (e: "update:open", value: boolean): void }>();
</script>

<template>
  <div v-if="open" class="dialog-mask" @click.self="emit('update:open', false)">
    <div class="dialog" role="dialog" aria-modal="true" aria-label="设置">
      <header class="dialog-head">
        <h2>设置</h2>
        <button class="icon" title="关闭" @click="emit('update:open', false)">×</button>
      </header>
      <p class="dialog-empty">Agent 模型与 3D 模型配置将在 Task 3 实现。</p>
    </div>
  </div>
</template>
