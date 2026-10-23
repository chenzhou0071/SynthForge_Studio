<script setup lang="ts">
defineProps<{ active: "blender" | "unity" }>();

const emit = defineEmits<{
  (e: "select", area: "blender" | "unity"): void;
  (e: "openSettings"): void;
}>();
</script>

<template>
  <nav class="activitybar">
    <button
      class="activity"
      :class="{ active: active === 'blender' }"
      title="Blender 区"
      @click="emit('select', 'blender')"
    >
      <svg viewBox="0 0 24 24" aria-hidden="true">
        <path d="M12 3 4 7.5v9L12 21l8-4.5v-9L12 3Z" />
        <path d="M4 7.5 12 12l8-4.5M12 12v9" />
      </svg>
    </button>
    <button
      class="activity"
      :class="{ active: active === 'unity' }"
      title="Unity 区"
      @click="emit('select', 'unity')"
    >
      <svg viewBox="0 0 24 24" aria-hidden="true">
        <path d="m12 3 7 4v10l-7 4-7-4V7l7-4Z" />
        <path d="m12 7 3.5 2v6L12 17l-3.5-2V9L12 7Z" />
      </svg>
    </button>
    <span class="activity-spacer" />
    <button class="activity gear" title="设置" @click="emit('openSettings')">
      <span aria-hidden="true">⚙</span>
    </button>
  </nav>
</template>
