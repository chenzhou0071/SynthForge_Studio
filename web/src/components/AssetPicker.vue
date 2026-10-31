<script setup lang="ts">
import { onMounted, ref } from "vue";
import type { WsClient } from "../ws/WsClient";

// 「添加资产」选择器：从模型库挑要导入 Unity 的资产（glb/fbx），可多选。
interface AssetItem { relativePath: string; name: string; kind: string; }
const props = defineProps<{ client: WsClient }>();
const emit = defineEmits<{ close: []; confirm: [paths: string[]] }>();

const assets = ref<AssetItem[]>([]);
const selected = ref<string[]>([]);
const loadError = ref("");

onMounted(async () => {
  try {
    const list = (await props.client.request("assets.list", {})) as unknown as AssetItem[];
    // 只有 glb/fbx 能进 Unity；其余（blend/截图/配置）过滤掉
    assets.value = (Array.isArray(list) ? list : []).filter(
      (a) => a.kind === "glb" || a.kind === "fbx",
    );
  } catch (e) {
    loadError.value = e instanceof Error ? e.message : String(e);
  }
});

function toggle(path: string) {
  const index = selected.value.indexOf(path);
  if (index >= 0) selected.value.splice(index, 1);
  else selected.value.push(path);
}
</script>

<template>
  <div class="modal-mask" @click.self="emit('close')">
    <div class="modal">
      <header>添加资产（可多选）</header>
      <p v-if="loadError" class="picker-error">{{ loadError }}</p>
      <p v-else-if="!assets.length" class="picker-empty">模型库里暂无可导入的 glb/fbx</p>
      <ul>
        <li
          v-for="a in assets"
          :key="a.relativePath"
          :class="{ picked: selected.includes(a.relativePath) }"
          @click="toggle(a.relativePath)"
        >
          {{ a.relativePath }}
        </li>
      </ul>
      <footer>
        <button @click="emit('close')">取消</button>
        <button :disabled="!selected.length" @click="emit('confirm', selected)">添加</button>
      </footer>
    </div>
  </div>
</template>

<style scoped>
.modal-mask {
  position: fixed;
  inset: 0;
  background: rgba(0, 0, 0, 0.5);
  display: grid;
  place-items: center;
  z-index: 40;
}

.modal {
  width: min(420px, 90vw);
  max-height: 70vh;
  display: flex;
  flex-direction: column;
  background: var(--panel-alt);
  border: 1px solid var(--border);
  border-radius: 6px;
}

header {
  padding: 10px 14px;
  border-bottom: 1px solid var(--border);
  font-size: 13px;
}

ul {
  margin: 0;
  padding: 6px 0;
  list-style: none;
  overflow: auto;
  flex: 1;
}

li {
  padding: 5px 14px;
  font-size: 12px;
  cursor: pointer;
}

li:hover {
  background: var(--panel);
}

li.picked {
  color: var(--accent);
}

li.picked::before {
  content: "✓ ";
}

.picker-empty,
.picker-error {
  padding: 12px 14px;
  color: var(--muted);
  font-size: 12px;
}

footer {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
  padding: 10px 14px;
  border-top: 1px solid var(--border);
}

footer button {
  border: 1px solid var(--border);
  border-radius: 3px;
  background: transparent;
  color: var(--text);
  padding: 4px 12px;
  font-size: 12px;
  cursor: pointer;
}

footer button:last-child {
  background: var(--accent);
  border-color: var(--accent);
  color: #fff;
}

footer button:disabled {
  opacity: 0.5;
  cursor: default;
}
</style>
