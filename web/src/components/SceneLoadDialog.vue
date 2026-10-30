<script setup lang="ts">
import { ref } from "vue";
import type { WsClient } from "../ws/WsClient";

const props = defineProps<{
  client: WsClient;
  // 后端 sceneLoad.confirm 下发的待确认载入
  load: { area: string; requestId: string; file: string; path: string };
}>();
const emit = defineEmits<{ (e: "accepted"): void; (e: "declined"): void }>();

const error = ref("");
const busy = ref(false);

async function accept() {
  busy.value = true;
  error.value = "";
  try {
    const res = await props.client.request<{ ok?: boolean; error?: string }>("sceneLoad.accept", {
      requestId: props.load.requestId,
    });
    if (res?.ok === false) {
      // blender-busy：弹窗留着，等当前建模/转换结束后再点一次
      error.value =
        res.error === "blender-busy"
          ? "Blender 会话正忙，请稍后再点确定"
          : (res.error ?? "载入失败");
      return;
    }
    emit("accepted");
  } catch (e) {
    error.value = e instanceof Error ? e.message : "载入失败";
  } finally {
    busy.value = false;
  }
}

async function decline() {
  busy.value = true;
  await props.client
    .request("sceneLoad.decline", { requestId: props.load.requestId })
    .catch(() => {
      /* 取消失败不阻断：窗口已关，后端最多留一个未被消费的 requestId */
    });
  emit("declined");
  busy.value = false;
}
</script>

<template>
  <div class="dialog-mask" @click.self="decline">
    <div class="dialog" role="dialog" aria-modal="true" aria-label="载入资产">
      <header class="dialog-head">
        <h2>讨论前载入资产</h2>
      </header>
      <p class="load-line">
        将打开 <b>{{ load.file }}.blend</b>（{{ load.path }}），它会<b>替换 Blender 当前场景</b>。
      </p>
      <p class="load-note">
        这个弹窗不会自动关：请先切到 Blender 把当前场景保存好，再回来点确定。
        确认后系统直接打开目标 .blend，不会替你再存一份。
      </p>
      <p v-if="error" class="load-error">{{ error }}</p>
      <footer class="dialog-foot">
        <button class="ghost" :disabled="busy" @click="decline">取消，只按文字讨论</button>
        <button class="primary" :disabled="busy" @click="accept">我已保存，继续载入</button>
      </footer>
    </div>
  </div>
</template>

<style scoped>
/* 复用 styles.css 里 .dialog-mask / .dialog / .dialog-head 与 .primary / .ghost 全局观感，
   这里只补弹窗正文三条文本样式 */
.load-line,
.load-note {
  font-size: 13px;
  line-height: 1.6;
  margin: 0 0 8px;
}
.load-note {
  color: var(--muted);
}
.load-error {
  color: #f48771;
  font-size: 12px;
  margin-bottom: 6px;
}
.dialog-foot {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
  margin-top: 12px;
}
</style>
