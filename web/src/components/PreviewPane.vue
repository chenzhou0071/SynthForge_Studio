<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from "vue";
import * as THREE from "three";
import { GLTFLoader } from "three/addons/loaders/GLTFLoader.js";
import { OrbitControls } from "three/addons/controls/OrbitControls.js";
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
const emit = defineEmits<{ modify: [path: string]; close: [] }>();

const host = ref<HTMLDivElement>();
const isModel = computed(() => props.file?.kind === "model");
const loadError = ref("");
const clips = ref<{ name: string; duration: number }[]>([]); // glb 自带的动画片段
const playing = ref(true);

const ASSET_SUFFIXES = ["glb", "fbx", "blend"];
// 修改入口只属于 blender 模型库：P6 的 unity 树也会列 fbx，但不能走 .blend 修改流程
const isAssetFile = computed(
  () => props.area === "blender" && !!props.file
    && ASSET_SUFFIXES.includes(props.file.path.split(".").pop()?.toLowerCase() ?? ""),
);

function languageOf(file: PreviewFile) {
  if (file.language && hljs.getLanguage(file.language)) return file.language;
  return languageForPath(file.path) || "plaintext";
}

function highlighted(file: PreviewFile) {
  return hljs.highlight(file.content ?? "", { language: languageOf(file) }).value;
}

function baseName(path: string) {
  return path.split("/").pop() ?? path;
}

function folderOf(path: string) {
  const parts = path.split("/");
  parts.pop();
  return parts.length ? parts.join("/") : "模型库根目录";
}

function typeLabel(path: string) {
  const suffix = path.split(".").pop()?.toLowerCase();
  return suffix === "glb" ? "GLB 模型（可直接预览）"
       : suffix === "fbx" ? "FBX 交换格式（导入 Unity 用）"
       : "Blender 源文件（修改用）";
}

function formattedSize(bytes?: number) {
  if (!bytes) return "—";
  return bytes >= 1024 * 1024
    ? `${(bytes / (1024 * 1024)).toFixed(1)} MB`
    : `${Math.max(1, Math.round(bytes / 1024))} KB`;
}

function formattedModified(iso?: string) {
  return iso ? new Date(iso).toLocaleString() : "—";
}

let renderer: THREE.WebGLRenderer | null = null;
let scene: THREE.Scene | null = null;
let camera: THREE.PerspectiveCamera | null = null;
let controls: OrbitControls | null = null;
let loaded: THREE.Group | null = null;
let mixer: THREE.AnimationMixer | null = null;
let frame = 0;
let resizeObserver: ResizeObserver | null = null;
const clock = new THREE.Clock();

function resize() {
  if (!host.value || !renderer || !camera) return;
  const width = host.value.clientWidth;
  const height = host.value.clientHeight;
  // 画布容器在非模型预览时是 display:none，0×0 会把相机的 aspect 算成 0，之后再也不会重算
  if (width <= 0 || height <= 0) return;
  renderer.setSize(width, height);
  camera.aspect = width / height;
  camera.updateProjectionMatrix();
}

// 资产尺度差异很大（1m 到几十米）：按包围球重新摆相机，否则动画摆了也看不见
function fitCamera(object: THREE.Object3D) {
  if (!camera || !controls) return;
  const sphere = new THREE.Box3().setFromObject(object).getBoundingSphere(new THREE.Sphere());
  const size = Math.max(sphere.radius * 2, 0.1);
  // 45° 垂直视角下把整颗包围球装进去约需 1.3×直径，再留一点余量
  camera.position.copy(sphere.center)
    .add(new THREE.Vector3(size * 0.9, size * 0.75, size * 1.15));
  camera.near = Math.max(size / 1000, 0.001);
  camera.far = size * 20;
  camera.updateProjectionMatrix();
  controls.target.copy(sphere.center);
  controls.update();
}

// 3D 画布按需创建：纯文本/图片预览不该为 WebGL 付启动成本，测试环境也能跳过真实上下文
function ensureViewer() {
  if (renderer) return true;
  if (!host.value) return false;
  try {
    renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true });
  } catch {
    loadError.value = "当前环境不支持 3D 预览";
    return false;
  }
  scene = new THREE.Scene();
  camera = new THREE.PerspectiveCamera(45, 1, 0.01, 1000);
  camera.position.set(2, 2, 3);
  controls = new OrbitControls(camera, renderer.domElement);
  scene.add(new THREE.HemisphereLight(0xffffff, 0x444444, 1));
  // 只用半球光时侧立面会很黑；补一盏方向光让低模体块与门窗可辨
  const sun = new THREE.DirectionalLight(0xffffff, 2);
  sun.position.set(5, 10, 7);
  scene.add(sun);
  renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
  host.value.appendChild(renderer.domElement);
  resize();
  const loop = () => {
    frame = requestAnimationFrame(loop);
    const delta = Math.min(clock.getDelta(), 0.1); // 切标签页回来不跳帧
    if (mixer && playing.value)
      mixer.update(delta);
    controls?.update();
    if (renderer && scene && camera)
      renderer.render(scene, camera);
  };
  loop();
  return true;
}

function clearModel() {
  if (!loaded) return;
  if (mixer) {
    mixer.stopAllAction();
    mixer.uncacheRoot(loaded);
    mixer = null;
  }
  scene?.remove(loaded);
  loaded = null;
}

function load(file: PreviewFile | null) {
  loadError.value = "";
  playing.value = true;
  clips.value = [];
  clearModel();
  if (!file || file.kind !== "model" || !file.dataUrl)
    return;
  if (!ensureViewer())
    return;
  new GLTFLoader().load(
    file.dataUrl,
    (gltf) => {
      loaded = gltf.scene;
      scene?.add(loaded);
      resize(); // 首次加载时容器可能刚结束隐藏，先拿到真实尺寸再取景
      fitCamera(loaded);
      // glTF 只能携带物体 TRS 关键帧：把自带动画全部同时循环播放
      if (gltf.animations.length) {
        mixer = new THREE.AnimationMixer(loaded);
        for (const clip of gltf.animations)
          mixer.clipAction(clip).setLoop(THREE.LoopRepeat, Infinity).play();
        clips.value = gltf.animations.map((clip) => ({
          name: clip.name || "动画",
          duration: clip.duration,
        }));
      }
    },
    undefined,
    () => { loadError.value = "模型加载失败"; },
  );
}

// flush: "post"：等 v-if/class 把画布容器从 display:none 切回可见后再初始化，否则拿到 0×0
watch(() => props.file, load, { flush: "post" });

onMounted(() => {
  window.addEventListener("resize", resize);
  // 面板拖拽/切区都会改变尺寸，仅靠 window.resize 不够
  if (typeof ResizeObserver !== "undefined" && host.value) {
    resizeObserver = new ResizeObserver(() => resize());
    resizeObserver.observe(host.value);
  }
  // 页面首次渲染就带着模型（自动预览新资产）时也要加载
  load(props.file);
});

onBeforeUnmount(() => {
  cancelAnimationFrame(frame);
  window.removeEventListener("resize", resize);
  resizeObserver?.disconnect();
  resizeObserver = null;
  if (mixer) {
    mixer.stopAllAction();
    mixer = null;
  }
  renderer?.dispose();
  renderer = null;
  scene = null;
  camera = null;
  controls = null;
  loaded = null;
});
</script>

<template>
  <section class="preview">
    <header class="panel-head">
      <span>预览</span>
      <span v-if="props.file" class="preview-path">{{ props.file.path }}</span>
      <button v-if="isAssetFile" class="modify-btn" @click="emit('modify', props.file!.path)">
        修改此资产
      </button>
      <button
        v-if="props.file"
        class="close-btn"
        title="关闭预览"
        aria-label="关闭预览"
        @click="emit('close')"
      >
        ×
      </button>
    </header>

    <div class="preview-body">
      <div ref="host" class="canvas-host" :class="{ hidden: !isModel }" />
      <p v-if="isModel && loadError" class="info-card">{{ loadError }}</p>

      <!-- GLB 只承载几何、基础材质与物体关键帧；其余效果要回 Blender 看 -->
      <p v-if="isModel && !loadError" class="preview-note">
        完整材质、动画与特效请在 Blender 中查看
      </p>
      <div v-if="isModel && clips.length" class="anim-bar">
        <button class="anim-toggle" @click="playing = !playing">
          {{ playing ? "暂停" : "播放" }}
        </button>
        <span v-for="(clip, index) in clips" :key="index" class="anim-clip">
          {{ clip.name }} {{ clip.duration.toFixed(1) }}s
        </span>
      </div>

      <template v-if="!isModel && props.file">
        <img
          v-if="props.file.kind === 'image'"
          class="preview-image"
          :src="props.file.dataUrl"
          :alt="props.file.path"
        />
        <pre v-else-if="props.file.kind === 'text'" class="code"><code class="hljs" v-html="highlighted(props.file)" /></pre>
        <div v-else-if="props.file.kind === 'info'" class="info-card file-info">
          <h3>{{ baseName(props.file.path) }}</h3>
          <dl>
            <dt>类型</dt><dd>{{ typeLabel(props.file.path) }}</dd>
            <dt>大小</dt><dd>{{ formattedSize(props.file.size) }}</dd>
            <dt>修改时间</dt><dd>{{ formattedModified(props.file.modified) }}</dd>
            <dt>所在目录</dt><dd>{{ folderOf(props.file.path) }}</dd>
            <dt>可修改</dt>
            <dd>{{ props.file.blendPath ? "是 · 已找到同名的 .blend 源文件" : "否 · 没有 .blend 源文件" }}</dd>
          </dl>
          <p v-if="!props.file.blendPath" class="file-info-hint">
            3D 高模（云 API 生成）与外部导入的模型只能重新生成，无法对话修改。
          </p>
        </div>
        <div v-else class="info-card">
          <h3>{{ props.file.path }}</h3>
          <p>暂无可用预览</p>
        </div>
      </template>
      <div v-else-if="!isModel" class="info-card">
        <h3>{{ area === "blender" ? "资产库概览" : "工程信息" }}</h3>
        <p>{{ area === "blender" ? "选中资产后在此预览" : "选中文件后在此预览" }}</p>
      </div>
    </div>
  </section>
</template>

<style scoped>
.preview {
  flex: 1;
  min-width: 0;
  display: flex;
  flex-direction: column;
  background: #1e1e1e;
}

.preview-path {
  flex: 1;
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  color: var(--muted);
}

.modify-btn {
  flex: none;
  padding: 2px 10px;
  font-size: 12px;
}

.close-btn {
  flex: none;
  margin-left: 8px;
  width: 20px;
  height: 20px;
  padding: 0;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  border: 1px solid var(--border);
  border-radius: 50%;
  background: transparent;
  color: var(--muted);
  font-size: 14px;
  line-height: 1;
  cursor: pointer;
}

.close-btn:hover {
  color: var(--text);
  border-color: var(--muted);
}

/* canvas 铺满内容区（不能 inset:0 到整个 section，否则盖住标题栏） */
.preview-body {
  position: relative;
  flex: 1;
  min-height: 0;
  display: flex;
  flex-direction: column;
}

.canvas-host {
  position: absolute;
  inset: 0;
}

.canvas-host.hidden {
  display: none;
}

.preview-note {
  position: absolute;
  top: 10px;
  right: 12px;
  z-index: 1;
  margin: 0;
  padding: 4px 8px;
  font-size: 11px;
  color: #999;
  background: rgba(20, 20, 20, 0.65);
  border-radius: 3px;
  pointer-events: none; /* 不挡鼠标拖拽旋转 */
}

.anim-bar {
  position: absolute;
  left: 50%;
  bottom: 12px;
  transform: translateX(-50%);
  z-index: 1;
  display: flex;
  align-items: center;
  gap: 10px;
  max-width: calc(100% - 24px);
  overflow-x: auto;
  padding: 6px 12px;
  font-size: 12px;
  color: #ccc;
  background: rgba(20, 20, 20, 0.75);
  border-radius: 4px;
}

.anim-toggle {
  flex: none;
  padding: 2px 10px;
  font-size: 12px;
}

.anim-clip {
  flex: none;
  color: #999;
  white-space: nowrap;
}

.preview-image {
  max-width: 100%;
  max-height: 100%;
  margin: auto;
  object-fit: contain;
}

.code {
  position: absolute;
  inset: 0;
  overflow: auto;
  margin: 0;
  padding: 12px;
}

.info-card {
  margin: auto;
  text-align: center;
  color: var(--muted);
}

.file-info {
  min-width: 280px;
  text-align: left;
}

.file-info h3 {
  margin: 0 0 12px;
  color: var(--text);
  font-size: 14px;
}

.file-info dl {
  display: grid;
  grid-template-columns: auto 1fr;
  gap: 6px 16px;
  margin: 0;
  font-size: 12px;
}

.file-info dt {
  color: var(--muted);
}

.file-info dd {
  margin: 0;
  word-break: break-all;
}

.file-info-hint {
  margin: 12px 0 0;
  color: #d7a55a;
  font-size: 12px;
}
</style>
