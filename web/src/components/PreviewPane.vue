<script setup lang="ts">
import hljs from "highlight.js/lib/core";
import csharp from "highlight.js/lib/languages/csharp";
import json from "highlight.js/lib/languages/json";
import lua from "highlight.js/lib/languages/lua";
import plaintext from "highlight.js/lib/languages/plaintext";
import typescript from "highlight.js/lib/languages/typescript";
import "highlight.js/styles/vs2015.css";

import type { PreviewFile } from "../preview/file";
import { languageForPath } from "../preview/language";

hljs.registerLanguage("csharp", csharp);
hljs.registerLanguage("json", json);
hljs.registerLanguage("lua", lua);
hljs.registerLanguage("plaintext", plaintext);
hljs.registerLanguage("typescript", typescript);

const props = defineProps<{ area: "blender" | "unity"; file: PreviewFile | null }>();

function languageOf(file: PreviewFile) {
  if (file.language && hljs.getLanguage(file.language)) return file.language;
  return languageForPath(file.path) || "plaintext";
}

function highlighted(file: PreviewFile) {
  return hljs.highlight(file.content ?? "", { language: languageOf(file) }).value;
}
</script>

<template>
  <section class="preview">
    <header class="panel-head">
      <span>预览</span>
      <span v-if="props.file" class="preview-path">{{ props.file.path }}</span>
    </header>

    <template v-if="props.file">
      <img
        v-if="props.file.kind === 'image'"
        class="preview-image"
        :src="props.file.dataUrl"
        :alt="props.file.path"
      />
      <pre v-else-if="props.file.kind === 'text'" class="code"><code class="hljs" v-html="highlighted(props.file)" /></pre>
      <div v-else class="info-card">
        <h3>{{ props.file.path }}</h3>
        <p>暂无可用预览</p>
      </div>
    </template>
    <div v-else class="info-card">
      <h3>{{ area === "blender" ? "资产库概览" : "工程信息" }}</h3>
      <p>{{ area === "blender" ? "选中资产后在此预览" : "选中文件后在此预览" }}</p>
    </div>
  </section>
</template>

<style scoped>
.preview-path {
  max-width: 70%;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  color: var(--muted);
}

.preview-image {
  max-width: 100%;
  max-height: 100%;
  margin: auto;
  object-fit: contain;
}

.code {
  flex: 1;
  overflow: auto;
  margin: 0;
  padding: 12px;
}

.code code {
  padding: 0;
  background: transparent;
}

.info-card {
  margin: auto;
  padding: 24px;
  text-align: center;
  color: var(--muted);
}

.info-card h3 {
  margin: 0 0 8px;
  color: var(--text);
  font-size: 14px;
}

.info-card p {
  margin: 0;
}
</style>
