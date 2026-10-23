<script setup lang="ts">
defineProps<{ area: "blender" | "unity" }>();
</script>

<template>
  <aside class="filetree">
    <header class="panel-head">
      <span>{{ area === "blender" ? "模型库" : "Unity 工程" }}</span>
    </header>
    <div class="tree-body">
      <p class="tree-root">
        {{ area === "blender" ? "文档/SynthForgeStudio/ModelLibrary" : "尚未导入工程" }}
      </p>
      <p class="tree-empty">文件列表将在后续里程碑接入。</p>
    </div>
  </aside>
</template>
