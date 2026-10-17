# SynthForge Studio P3：前端骨架与 GUI 壳实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Vue3+TS 前端工程与 WS 客户端；侧边导航式布局四件套（活动栏 / 顶部栏双灯 / 文件树 + 预览 / 聊天面板）与两栏设置对话框；C++ 侧 QWebEngineView 壳（dev=Vite 直连，prod=qrc + `app://` 协议），端到端打通设置与聊天文本流。

**Architecture:** 前端独立工程 `web/`（Vite 构建，产物 `web/dist`）；`WsClient` 封装请求-响应匹配、事件订阅、超时与指数退避重连，上层组件只与服务通信。C++ 壳极薄：MainWindow 内嵌 QWebEngineView，自定义 `app://` scheme 从 qrc 供页，dev 模式直接加载 Vite dev server。

**Tech Stack:** Vue 3（`<script setup>` + TS）、Vite、vitest；Qt6::WebEngineWidgets；MessagePack 不需要，纯 JSON。

**规格映射：** 4.1 布局图、4.2 双灯、4.3 活动栏、7.1 设置页、8.3 WS 协议、8.7 前端结构。

**依赖：** P2 的 WS 协议与 `--headless` 模式。

## Global Constraints

- 活动栏仅两项：`blender` / `unity`（图标 + 悬停提示），底部 `⚙` 设置；AI 对话区永远最右侧。
- 双灯 `●Blender` `●Unity`：灰=未连接、绿=就绪、黄=重连中；悬停显示卡在哪一步（数据来自 `conn.status` 事件，P6 完善，P3 先订阅占位）。
- 设置对话框两栏：`Agent 模型`（主 Agent / 总结 Agent 两组参数，组内 服务商下拉 + 模型 + 密钥；仅选「自定义」时显示 Base URL 输入框）/ `3D 模型`（混元3D / Tripo / 自定义）。
- 预览配置：只读弹窗，密钥显示为 `****`（前端脱敏）。
- 暗色主题，CSS 变量集中在 `styles.css`；无第三方 UI 库（YAGNI）。

## File Structure

```
web/package.json / vite.config.ts / tsconfig.json / index.html
web/src/main.ts / App.vue / styles.css
web/src/ws/WsClient.ts          ← 请求匹配 + 事件 + 重连
web/src/ws/WsClient.test.ts     ← vitest 单测（mock WebSocket）
web/src/components/ActivityBar.vue / TopBar.vue / FileTree.vue
web/src/components/PreviewPane.vue / ChatPanel.vue / SettingsDialog.vue
src/app/MainWindow.h/.cpp       ← QMainWindow + QWebEngineView
src/app/AppSchemeHandler.h/.cpp ← app:// 协议（qrc 供页）
src/main.cpp                    ← GUI 入口（--dev-url 可选）
```

---

### Task 1: Vite 工程 + WsClient（TDD）

**Files:** Create `web/package.json`、`web/vite.config.ts`、`web/tsconfig.json`、`web/index.html`、`web/src/main.ts`、`web/src/ws/WsClient.ts`、`web/src/ws/WsClient.test.ts`。

**Interfaces:**
- `class WsClient { connect(url: string): Promise<void>; request<T>(method: string, params?: object): Promise<T>; on(event: string, cb: (data: any) => void): () => void; }`
- 帧约定（同 P2）：请求带 `id`；响应同 `id`；事件 `{event, data}`。

- [ ] **Step 1: 脚手架**

`web/package.json`：

```json
{
  "name": "synthforge-web",
  "private": true,
  "type": "module",
  "scripts": { "dev": "vite", "build": "vue-tsc -b && vite build", "test": "vitest run" },
  "dependencies": { "vue": "^3.5.0" },
  "devDependencies": {
    "@vitejs/plugin-vue": "^5.1.0", "typescript": "^5.6.0",
    "vite": "^5.4.0", "vitest": "^2.1.0", "vue-tsc": "^2.1.0"
  }
}
```

`web/vite.config.ts`：`plugins: [vue()]`；`test: { environment: "node" }`。

- [ ] **Step 2: 写失败的 WsClient 测试**

`web/src/ws/WsClient.test.ts`：

```ts
import { afterEach, describe, expect, it, vi } from "vitest";
import { WsClient } from "./WsClient";

class MockWebSocket {
  static instances: MockWebSocket[] = [];
  onopen?: () => void;
  onmessage?: (ev: { data: string }) => void;
  onclose?: () => void;
  onerror?: () => void;
  sent: string[] = [];
  constructor(public url: string) { MockWebSocket.instances.push(this); }
  send(data: string) { this.sent.push(data); }
  close() {}
  emit(payload: object) { this.onmessage?.({ data: JSON.stringify(payload) }); }
  open() { this.onopen?.(); }
}

describe("WsClient", () => {
  afterEach(() => { MockWebSocket.instances = []; });

  it("matches responses by id", async () => {
    vi.stubGlobal("WebSocket", MockWebSocket as unknown as typeof WebSocket);
    const client = new WsClient();
    const ready = client.connect("ws://x");
    MockWebSocket.instances[0].open();
    await ready;

    const p = client.request("settings.get");
    const req = JSON.parse(MockWebSocket.instances[0].sent[0]);
    expect(req.method).toBe("settings.get");
    MockWebSocket.instances[0].emit({ id: req.id, result: { ok: true } });
    await expect(p).resolves.toEqual({ ok: true });
  });

  it("dispatches events without id", async () => {
    vi.stubGlobal("WebSocket", MockWebSocket as unknown as typeof WebSocket);
    const client = new WsClient();
    const ready = client.connect("ws://x");
    MockWebSocket.instances[0].open();
    await ready;

    const got: any[] = [];
    client.on("chat.delta", (d) => got.push(d));
    MockWebSocket.instances[0].emit({ event: "chat.delta", data: { text: "hi" } });
    expect(got).toEqual([{ text: "hi" }]);
  });

  it("rejects pending requests on close", async () => {
    vi.stubGlobal("WebSocket", MockWebSocket as unknown as typeof WebSocket);
    const client = new WsClient();
    const ready = client.connect("ws://x");
    MockWebSocket.instances[0].open();
    await ready;

    const p = client.request("slow.call");
    MockWebSocket.instances[0].onclose?.();
    await expect(p).rejects.toThrow("connection closed");
  });
});
```

- [ ] **Step 3: 实现 WsClient**

```ts
type Pending = {
  resolve: (value: unknown) => void;
  reject: (reason: Error) => void;
  timer: ReturnType<typeof setTimeout>;
};

export class WsClient {
  private socket?: WebSocket;
  private pending = new Map<number, Pending>();
  private listeners = new Map<string, Set<(data: unknown) => void>>();
  private nextId = 1;
  private url = "";
  private reconnectDelay = 1000;
  private closedByUser = false;

  connect(url: string): Promise<void> {
    this.url = url;
    this.closedByUser = false;
    return new Promise<void>((resolve, reject) => {
      const socket = new WebSocket(url);
      this.socket = socket;
      socket.onopen = () => { this.reconnectDelay = 1000; resolve(); };
      socket.onerror = () => reject(new Error("ws connect failed"));
      socket.onclose = () => {
        this.failAll("connection closed");
        if (!this.closedByUser) this.scheduleReconnect();
      };
      socket.onmessage = (ev: MessageEvent) => this.onMessage(String(ev.data));
    });
  }

  request<T = unknown>(method: string, params: object = {}): Promise<T> {
    const id = this.nextId++;
    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`timeout: ${method}`));
      }, 30000);
      this.pending.set(id, { resolve: resolve as (v: unknown) => void, reject, timer });
      this.socket?.send(JSON.stringify({ id, method, params }));
    });
  }

  on(event: string, cb: (data: unknown) => void): () => void {
    if (!this.listeners.has(event)) this.listeners.set(event, new Set());
    this.listeners.get(event)!.add(cb);
    return () => { this.listeners.get(event)?.delete(cb); };
  }

  private onMessage(text: string) {
    const frame = JSON.parse(text) as {
      id?: number; result?: unknown; error?: { message: string }; event?: string; data?: unknown;
    };
    if (frame.id !== undefined) {
      const p = this.pending.get(frame.id);
      if (!p) return;
      clearTimeout(p.timer);
      this.pending.delete(frame.id);
      if (frame.error) p.reject(new Error(frame.error.message));
      else p.resolve(frame.result);
      return;
    }
    if (frame.event) this.listeners.get(frame.event)?.forEach((cb) => cb(frame.data));
  }

  private failAll(reason: string) {
    this.pending.forEach((p) => { clearTimeout(p.timer); p.reject(new Error(reason)); });
    this.pending.clear();
  }

  private scheduleReconnect() {
    const delay = this.reconnectDelay;
    this.reconnectDelay = Math.min(this.reconnectDelay * 2, 10000);
    setTimeout(() => {
      this.connect(this.url).catch(() => { /* 下一轮 onclose 继续退避 */ });
    }, delay);
  }
}
```

- [ ] **Step 4: 跑测试**

Run: `cd web; npm install; npm test`
Expected: 3 用例 Passed。

- [ ] **Step 5: Commit**（`feat: Vue 工程脚手架与 WS 客户端（匹配/事件/重连）`）

---

### Task 2: 布局四件套

**Files:** Create `web/src/App.vue`、`web/src/styles.css`、`web/src/components/ActivityBar.vue`、`TopBar.vue`、`FileTree.vue`、`PreviewPane.vue`、`ChatPanel.vue`；Modify `web/src/main.ts`、`web/index.html`。

**Interfaces:**
- `ActivityBar`：props `active: 'blender'|'unity'`；emits `select(area)`、`openSettings()`。
- `TopBar`：props `area`、`blender: LampState`、`unity: LampState`（`'off'|'ready'|'reconnecting'`）、`hints: {blender?: string; unity?: string}`、`importing?: boolean`；emits `importProject()`。
- `ChatPanel`：props `area`、`client: WsClient`；内部订阅 `chat.delta/chat.message/error.card`。

- [ ] **Step 1: 实现布局（按 4.1 规格图；结构代码精简但可运行）**

`App.vue` 核心：

```vue
<script setup lang="ts">
import { ref, onMounted } from "vue";
import ActivityBar from "./components/ActivityBar.vue";
import TopBar from "./components/TopBar.vue";
import FileTree from "./components/FileTree.vue";
import PreviewPane from "./components/PreviewPane.vue";
import ChatPanel from "./components/ChatPanel.vue";
import SettingsDialog from "./components/SettingsDialog.vue";
import { WsClient } from "./ws/WsClient";

const client = new WsClient();
const active = ref<"blender" | "unity">("blender");
const settingsOpen = ref(false);
const lamp = ref({ blender: "off", unity: "off" });

onMounted(async () => {
  const params = new URLSearchParams(location.search);
  await client.connect(params.get("ws") ?? "ws://127.0.0.1:0");
  client.on("conn.status", (d: any) => { lamp.value = d; });
});
</script>

<template>
  <div class="app">
    <ActivityBar :active="active" @select="active = $event as any" @open-settings="settingsOpen = true" />
    <div class="main">
      <TopBar :area="active" :blender="lamp.blender as any" :unity="lamp.unity as any" />
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
```

`styles.css`：`.app{display:flex;height:100vh}` `.main{flex:1;display:flex;flex-direction:column}` `.content{flex:1;display:flex;min-height:0}` 活动栏 48px、聊天面板固定 380px 最右侧、暗色变量 `--bg:#1e1e1e; --panel:#252526; --text:#cccccc; --accent:#0e639c`。

`TopBar.vue` 双灯：

```vue
<script setup lang="ts">
defineProps<{ area: string; blender: string; unity: string; importing?: boolean }>();
const emit = defineEmits<{ (e: "importProject"): void }>();
const color = (s: string) => (s === "ready" ? "#4ec9b0" : s === "reconnecting" ? "#cca700" : "#555");
</script>
<template>
  <header class="topbar">
    <span v-if="area === 'blender'" class="fixed-path">模型库：文档/SynthForgeStudio/ModelLibrary（固定）</span>
    <button v-else class="import" :disabled="importing" @click="emit('importProject')">导入 Unity 工程</button>
    <span class="spacer" />
    <span class="lamp" title="Blender 链路"><i :style="{ background: color(blender) }" />Blender</span>
    <span class="lamp" title="Unity 链路"><i :style="{ background: color(unity) }" />Unity</span>
  </header>
</template>
```

`ChatPanel.vue` 核心逻辑：消息列表 + 输入框；`send()` → `client.request("chat.send", {area, text})`；订阅 `chat.message` append；`chat.delta` 追加到 streaming 气泡；`error.card` 显示红色卡片。`App.vue` 同时挂载两个实例，每个实例只处理 `d.area === props.area` 的事件，因此后台区的事件仍会写入自己的面板。

`FileTree.vue` / `PreviewPane.vue`：P3 先静态骨架（文件树渲染 `client.request("session.load")` 之外的数据留空，P4/P6 填数据源），保持布局位置。

- [ ] **Step 2: 构建验证**

Run: `cd web; npm run build`
Expected: `web/dist` 产出。

- [ ] **Step 3: Commit**（`feat: 侧边导航布局四件套与聊天面板`）

---

### Task 3: 设置对话框（两栏 + 脱敏预览）

**Files:** Create `web/src/components/SettingsDialog.vue`；Modify `App.vue`（已挂载）。

**Interfaces:**
- props `open: boolean`（`v-model:open`）、`client: WsClient`。
- 读写 `settings.get` / `settings.save`；数据形状同 P2 `makeDefaultSettings()`；`providerBaseUrls` 表用于「自定义」以外的隐藏 Base URL。

- [ ] **Step 1: 实现**

核心结构（两组 Agent 配置复用同一子表单，此处用 `v-for` over `['primary','summarizer']` 展开为两个块；每块字段：服务商下拉（openai/doubao/qwen/zhipu/deepseek/custom）、model、apiKey，`provider === 'custom'` 时显示 baseUrl 输入框）：

```vue
<script setup lang="ts">
import { ref, watch } from "vue";
import type { WsClient } from "../ws/WsClient";

const props = defineProps<{ open: boolean; client: WsClient }>();
const emit = defineEmits<{ (e: "update:open", v: boolean): void }>();
const tab = ref<"agent" | "cloud3d">("agent");
const settings = ref<any>(null);
const preview = ref(false);

watch(() => props.open, async (v) => {
  // 每次打开都重拉：后端会写入 unity.lastProject 等新字段，用缓存回存会丢数据
  if (v) settings.value = await props.client.request("settings.get");
});

async function save() {
  await props.client.request("settings.save", settings.value);
}
function masked(key: string) { return key ? "****" : "(未设置)"; }
</script>
```

模板要点：`Agent 模型` 页两个分组卡片（主 Agent / 总结 Agent）+「保存配置」「预览配置」；`3D 模型` 页三服务商选择（tripo: apiKey+version；hunyuan: secretId+secretKey+region+model；custom: baseUrl+token）。预览弹窗用 `JSON.stringify(settings, null, 2)` 并对包含 `apiKey|secretKey|secretId|token` 的字段替换为 `****`。

- [ ] **Step 2: 手工验证**（`npm run dev` + P2 `--headless`：保存后刷新，字段回填；预览中密钥为 `****`）

- [ ] **Step 3: Commit**（`feat: 设置对话框（Agent 双组 / 3D 三服务商 / 脱敏预览）`）

---

### Task 4: C++ GUI 壳（QWebEngineView + app:// 协议）

**Files:** Create `src/app/AppSchemeHandler.h/.cpp`、`src/app/MainWindow.h/.cpp`、`src/main.cpp`；Modify `CMakeLists.txt`（追加 `Qt6::WebEngineWidgets`；资源打包）。

**Interfaces:**
- `AppSchemeHandler : QWebEngineUrlSchemeHandler` — 将 `app://ui/<path>` 映射到 qrc `:/web/<path>`；`/` → `index.html`；按扩展名回 MIME（html/js/css/svg/png）。
- `MainWindow(QUrl uiUrl)` — 内嵌 QWebEngineView 加载 `uiUrl`，窗口标题 `SynthForge Studio`，初始 1440x900。
- `main.cpp`：`--dev-url <http://localhost:5173>` 优先；否则 `app://ui/index.html`；`QWebEngineUrlScheme` 注册必须在 `QApplication` 构造前；创建 `CoreApp({defaultDataDir(), false})` 并 `start()`，再把 `?ws=ws://127.0.0.1:<port>/?token=<token>` 追加到 `uiUrl`（前端 `?ws=` 的唯一来源；此前缺此接线，prod 下拿不到地址）。

- [ ] **Step 1: 实现**

`AppSchemeHandler.cpp` 核心：

```cpp
void AppSchemeHandler::requestStarted(QWebEngineUrlRequestJob* job)
{
    QString path = job->requestUrl().path();
    if (path.isEmpty() || path == "/")
        path = "/index.html";
    const QString resource = QStringLiteral(":/web") + path;
    auto* file = new QFile(resource, job);
    if (!file->open(QIODevice::ReadOnly)) {
        job->fail(QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }
    job->reply(mimeForPath(path), file);
}

QByteArray AppSchemeHandler::mimeForPath(const QString& path)
{
    if (path.endsWith(".js") || path.endsWith(".mjs")) return "text/javascript";
    if (path.endsWith(".css")) return "text/css";
    if (path.endsWith(".svg")) return "image/svg+xml";
    if (path.endsWith(".png")) return "image/png";
    if (path.endsWith(".json")) return "application/json";
    return "text/html";
}
```

`main.cpp` 核心：

```cpp
int main(int argc, char* argv[])
{
    QWebEngineUrlScheme scheme("app");
    scheme.setFlags(QWebEngineUrlScheme::SecureScheme
                    | QWebEngineUrlScheme::LocalAccessAllowed);
    QWebEngineUrlScheme::registerScheme(scheme);   // 必须先于 QApplication

    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("SynthForge");
    QCoreApplication::setApplicationName("SynthForgeStudio");

    const QStringList args = QCoreApplication::arguments();
    const int devIdx = args.indexOf("--dev-url");
    QUrl uiUrl = devIdx >= 0 && devIdx + 1 < args.size()
                     ? QUrl(args.at(devIdx + 1))
                     : QUrl("app://ui/index.html");

    CoreApp core({defaultDataDir(), false});      // GUI 入口同样启动内核
    if (!core.start())
        return 1;
    QUrlQuery query;
    query.addQueryItem("ws", QStringLiteral("ws://127.0.0.1:%1/?token=%2")
                                 .arg(core.ws()->port())
                                 .arg(core.ws()->token()));
    uiUrl.setQuery(query);                        // 前端从 ?ws= 读取完整地址

    auto* handler = new AppSchemeHandler(&app);
    MainWindow window(uiUrl);                     // core 先声明：退出时窗口先销毀
    window.show();
    return app.exec();
}
```

> 需要 `#include "core/coreapp/CoreApp.h"`、`"core/settings/SettingsStore.h"`、`"core/ws/WsServer.h"`（`CoreApp.h` 仅前向声明 `WsServer`）与 `<QUrlQuery>`。

`MainWindow` 构造中：`profile->installUrlSchemeHandler("app", handler)`（QWebEngineProfile::defaultProfile()），`m_view->setUrl(uiUrl)`。

CMake 增量：

```cmake
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Test Sql Network WebSockets WebEngineWidgets)
add_executable(synthforge_app src/main.cpp src/app/MainWindow.cpp src/app/AppSchemeHandler.cpp)
target_link_libraries(synthforge_app PRIVATE synthforge_core Qt6::WebEngineWidgets)
file(GLOB_RECURSE WEB_DIST_FILES CONFIGURE_DEPENDS web/dist/*)
qt_add_resources(synthforge_app "web_assets" PREFIX "/web" BASE "web/dist" FILES ${WEB_DIST_FILES})
```

- [ ] **Step 2: 构建验证**

Run: `cd web; npm run build; cd ..; cmake --build build --config Debug --target synthforge_app`
Expected: `synthforge_app.exe` 启动后窗口内可见前端布局（dev-url 模式先跑 `npm run dev`）。

- [ ] **Step 3: Commit**（`feat: QWebEngineView 壳与 app:// 资源协议`）

---

### Task 5: 端到端冒烟（dev / prod 两模式）

**Files:** 无新增；运行验证。

- [ ] **Step 1: dev 模式**

1. 终端 A：`build\Debug\synthforge_cli.exe --headless` → 记录 `READY ws=ws://127.0.0.1:<port>/?token=<token>`。
2. 终端 B：`cd web; npm run dev` → 打开打印的 `http://localhost:5173/?ws=<READY 里的完整 ws URL>`（URL 编码注意 `?`）。
3. 浏览器验收：双灯灰；打开设置保存 Agent 配置 → 「预览配置」密钥为 `****`；聊天框发送文本（若未配 API Key 应显示红色错误卡）。

- [ ] **Step 2: prod 模式**

Run: `build\Debug\synthforge_app.exe`
Expected: 窗口加载 `app://ui/index.html`，布局与 dev 一致；设置保存后重启仍能回读（`?ws=` 注入生效），未配置 Agent 时聊天返回错误卡。

- [ ] **Step 3: Commit**（仅当有配置修正时；纯验证不提交）

---

### Task 6: 对话区补全 + 文件树右键菜单 + 预览渲染

**Files:**
- Modify: `web/src/components/ChatPanel.vue`（历史加载 / 清空 / chat.image / 模式切换 / 云徽标）
- Modify: `web/src/components/FileTree.vue`（右键菜单五項）
- Modify: `web/src/components/PreviewPane.vue`（文本高亮 / 图片 / 信息卡）
- Modify: `web/src/App.vue`（PreviewPane 两区都显示；FileTree 传入 client；**两个常驻 ChatPanel**）
- Modify: `web/package.json`（新增 `highlight.js`）

**Interfaces:**
- `ChatPanel`：props `area`、`client`；内部 `mode: 'lowpoly' | 'cloud3d'`（blender 区专属，默认 lowpoly）；`chat.send` 参数追加 `mode`；**每区一个常驻实例，仅在挂载时 `session.load` 回填一次，切区不重载**；`chat.image {area, taskId, dataUrl, caption}` 渲染为截图卡；「清空」→ `chat.clear`（二次确认）；任务进行中显示「停止」→ `chat.cancel {area}`（结束时不删除消息）；`task.progress` / `asset.created` 维护本实例的 `busy`（开始显停止、落盘/成功隐藏）。
- `FileTree`：props `area`、`client`；右键菜单五项 → `fs.operate {area, op, path, name?}`，`op ∈ mkdir|rename|delete|reveal|refresh`（C++ handler 在 P4，Unity 区复用 P6）。
- `PreviewPane`：props `area`、`file: {path, kind: 'text'|'image'|'info', content?, dataUrl?, language?} | null`；`null` 时显示概览/信息卡。three.js 模型预览在 P4 接管 `kind === 'model'`。
- 规格映射：4.4 右键菜单、4.5 预览区、4.6 对话区、5.1 模式切换。

- [ ] **Step 1: 安装 highlight.js**

Run: `cd web; npm i highlight.js`

- [ ] **Step 2: 重写 ChatPanel.vue**

```vue
<script setup lang="ts">
import { onMounted, ref } from "vue";
import type { WsClient } from "../ws/WsClient";

interface ChatItem {
  role: "user" | "assistant" | "error" | "image";
  content?: string;
  dataUrl?: string;
  caption?: string;
  streaming?: boolean;
}

const props = defineProps<{ area: "blender" | "unity"; client: WsClient }>();

const messages = ref<ChatItem[]>([]);
const input = ref("");
const mode = ref<"lowpoly" | "cloud3d">("lowpoly");
const cloudConfigured = ref(false);
let streaming: ChatItem | null = null; // 当前流式气泡（非响应式引用）
// 每区一个常驻实例：busy 属于本实例，无需按区索引；切区只 v-show，状态不会串区
const busy = ref(false);

async function refreshCloudBadge() {
  if (props.area !== "blender") return;
  const s: any = await props.client.request("settings.get");
  const c3 = s?.cloud3d ?? {};
  const provider = c3.provider ?? "tripo";
  const conf = c3[provider] ?? {};
  const ready =
    (provider === "tripo" && !!conf.apiKey) ||
    (provider === "hunyuan" && !!conf.secretId && !!conf.secretKey) ||
    (provider === "custom" && !!conf.baseUrl && !!conf.apiToken);
  cloudConfigured.value = ready;
}

async function loadHistory() {
  messages.value = [];
  streaming = null;
  const history: any = await props.client.request("session.load", { area: props.area });
  messages.value = (history as any[]).map((m) => ({ role: m.role, content: m.content }));
  await refreshCloudBadge();
}

async function send() {
  const text = input.value.trim();
  if (!text) return;
  input.value = "";
  const res: any = await props.client.request("chat.send", {
    area: props.area,
    mode: props.area === "blender" ? mode.value : undefined,
    text,
  });
  busy.value = res?.accepted !== false; // 被前置检查拒绝时不进入“进行中”
}

async function cancelTask() {
  await props.client.request("chat.cancel", { area: props.area });
  busy.value = false;
}

async function clearChat() {
  if (!window.confirm("清空本区对话？此操作不可撤销")) return;
  await props.client.request("chat.clear", { area: props.area });
  messages.value = [];
  streaming = null;
  busy.value = false; // chat.clear 会取消本区任务
}

onMounted(() => {
  loadHistory();
  // 每个区一个常驻实例（v-show 切换，不卸载）：订阅无需 off，也不监听 props.area 重载历史
  props.client.on("chat.message", (d: any) => {
    if (d.area !== props.area) return;
    if (d.role === "assistant") busy.value = false;
    if (streaming && d.role === "assistant") {
      streaming.content = d.content; // 流结束：用完整文本覆盖，避免重复气泡
      streaming.streaming = false;
      streaming = null;
      return;
    }
    messages.value.push({ role: d.role, content: d.content });
  });
  props.client.on("chat.delta", (d: any) => {
    if (d.area !== props.area) return;
    if (!streaming) {
      const bubble: ChatItem = { role: "assistant", content: "", streaming: true };
      messages.value.push(bubble);
      streaming = bubble;
    }
    streaming.content += d.text;
  });
  props.client.on("chat.image", (d: any) => {
    if (d.area !== props.area) return;
    messages.value.push({ role: "image", dataUrl: d.dataUrl, caption: d.caption });
  });
  props.client.on("task.progress", (d: any) => {
    if (d.area !== props.area) return;
    // 低模分组进度无 state；云任务 progress 带 state（P4/P5）
    busy.value = d.state !== "success" && d.state !== "failed";
  });
  props.client.on("asset.created", (d: any) => {
    if (d.area === props.area) busy.value = false; // 落盘即任务结束
  });
  props.client.on("error.card", (d: any) => {
    if (d.area !== props.area) return;
    busy.value = false;
    messages.value.push({ role: "error", content: d.reason });
  });
});
</script>

<template>
  <section class="chat">
    <header class="chat-head">
      <span class="area-name">{{ area === "blender" ? "Blender 建模区" : "Unity 游戏区" }}</span>
      <template v-if="area === 'blender'">
        <div class="mode-switch">
          <button :class="{ active: mode === 'lowpoly' }" @click="mode = 'lowpoly'">低模</button>
          <button :class="{ active: mode === 'cloud3d' }" @click="mode = 'cloud3d'">云API</button>
        </div>
        <span class="badge" :class="cloudConfigured ? 'ok' : 'warn'">
          {{ cloudConfigured ? "云API 已配置" : "云API 未配置" }}
        </span>
      </template>
      <span class="spacer" />
      <button class="clear" title="清空本区对话" @click="clearChat">清空</button>
    </header>

    <div class="messages">
      <div v-for="(m, i) in messages" :key="i" class="msg" :class="m.role">
        <img v-if="m.role === 'image'" :src="m.dataUrl" class="shot" :alt="m.caption" />
        <p v-else-if="m.role === 'error'" class="error-card">{{ m.content }}</p>
        <p v-else class="text">{{ m.content }}</p>
        <span v-if="m.role === 'image' && m.caption" class="caption">{{ m.caption }}</span>
      </div>
    </div>

    <footer class="composer">
      <textarea v-model="input" rows="2" placeholder="描述你的需求…"
                @keydown.enter.exact.prevent="send" />
      <button v-if="busy" class="stop" @click="cancelTask">停止</button>
      <button @click="send">发送</button>
    </footer>
  </section>
</template>

<style scoped>
.chat { width: 380px; display: flex; flex-direction: column; background: var(--panel); }
.chat-head { display: flex; align-items: center; gap: 8px; padding: 6px 8px; }
.mode-switch button.active { background: var(--accent); color: #fff; }
.badge.ok { color: #4ec9b0; }
.badge.warn { color: #cca700; }
.messages { flex: 1; overflow-y: auto; padding: 8px; }
.msg.user { text-align: right; }
.msg.user .text { display: inline-block; background: var(--accent); color: #fff; padding: 4px 8px; border-radius: 6px; }
.shot { max-width: 100%; border: 1px solid #3c3c3c; }
.error-card { color: #f48771; border: 1px solid #f48771; padding: 6px; border-radius: 4px; }
.composer { display: flex; gap: 6px; padding: 8px; }
.composer textarea { flex: 1; resize: none; }
</style>
```

- [ ] **Step 3: FileTree.vue 加右键菜单**

在 P3 骨架基础上追加（树节点渲染仍为占位，P4/P6 填数据源；每个节点 `@contextmenu.prevent="openMenu($event, node.path)"`）：

```vue
<script setup lang="ts">
import { ref } from "vue";
import type { WsClient } from "../ws/WsClient";

const props = defineProps<{ area: "blender" | "unity"; client: WsClient }>();
const menu = ref<{ x: number; y: number; path: string } | null>(null);

function openMenu(e: MouseEvent, path: string) {
  menu.value = { x: e.clientX, y: e.clientY, path };
}
function closeMenu() {
  menu.value = null;
}

async function runOp(op: "mkdir" | "rename" | "delete" | "reveal" | "refresh") {
  const target = menu.value!;
  let name: string | undefined;
  if (op === "mkdir" || op === "rename") {
    const input = window.prompt(op === "mkdir" ? "新建文件夹名称" : "重命名为", "");
    if (!input) return closeMenu();
    name = input;
  }
  if (op === "delete" && !window.confirm(`确定删除 ${target.path} ?`))
    return closeMenu();
  await props.client.request("fs.operate", {
    area: props.area, op, path: target.path, name,
  });
  closeMenu();
}
</script>

<template>
  <aside class="filetree" @click="closeMenu">
    <div class="tree-body"><!-- P4/P6 填树节点，节点绑 openMenu($event, node.path) --></div>
    <ul v-if="menu" class="ctx-menu" :style="{ left: menu.x + 'px', top: menu.y + 'px' }">
      <li @click.stop="runOp('mkdir')">新建文件夹</li>
      <li @click.stop="runOp('rename')">重命名</li>
      <li @click.stop="runOp('delete')">删除</li>
      <li @click.stop="runOp('reveal')">在资源管理器中显示</li>
      <li @click.stop="runOp('refresh')">刷新</li>
    </ul>
  </aside>
</template>
```

- [ ] **Step 4: PreviewPane.vue 三类渲染**

```vue
<script setup lang="ts">
import hljs from "highlight.js/lib/core";
import csharp from "highlight.js/lib/languages/csharp";
import json from "highlight.js/lib/languages/json";
import typescript from "highlight.js/lib/languages/typescript";
import lua from "highlight.js/lib/languages/lua";
import "highlight.js/styles/vs2015.css";

hljs.registerLanguage("csharp", csharp);
hljs.registerLanguage("json", json);
hljs.registerLanguage("typescript", typescript);
hljs.registerLanguage("lua", lua);

defineProps<{
  area: "blender" | "unity";
  file: {
    path: string;
    kind: "text" | "image" | "info";
    content?: string;
    dataUrl?: string;
    language?: string;
  } | null;
}>();

function highlighted(content: string, language?: string) {
  const lang = language && hljs.getLanguage(language) ? language : "typescript";
  return hljs.highlight(content, { language: lang }).value;
}
</script>

<template>
  <section class="preview">
    <template v-if="file">
      <img v-if="file.kind === 'image'" :src="file.dataUrl" class="preview-image" :alt="file.path" />
      <pre v-else-if="file.kind === 'text'" class="code"><code v-html="highlighted(file.content ?? '', file.language)" /></pre>
      <div v-else class="info-card">
        <h3>{{ file.path }}</h3>
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
.preview { flex: 1; min-width: 0; display: flex; align-items: stretch; justify-content: stretch; background: #1e1e1e; }
.preview-image { max-width: 100%; max-height: 100%; margin: auto; object-fit: contain; }
.code { flex: 1; overflow: auto; margin: 0; padding: 12px; }
.info-card { margin: auto; text-align: center; color: #999; }
</style>
```

- [ ] **Step 5: App.vue 修正两区预览与 FileTree 接线**

```vue
      <div class="content">
        <FileTree :area="active" :client="client" />
        <PreviewPane :area="active" :file="previewFile" />
        <ChatPanel v-show="active === 'blender'" area="blender" :client="client" />
        <ChatPanel v-show="active === 'unity'" area="unity" :client="client" />
      </div>
```

`script` 中追加（`previewFile` 由 P4 的 asset.created / file.read 填充）：

```ts
import { ref } from "vue";
const previewFile = ref<null | {
  path: string;
  kind: "text" | "image" | "info";
  content?: string;
  dataUrl?: string;
  language?: string;
}>(null);
```

> 原 Task 2 的 `<PreviewPane v-if="active === 'blender'" />` 必须删掉 `v-if`：规格 4.5 两区都有预览区。两个 `ChatPanel` 必须用 `v-show`（不是 `v-if`）且 `area` 传静态值：`v-if` 会销毁/重建实例，导致消息、流式气泡、busy 和卡片丢失；`v-show` 让两个区在后台继续接收并保存自己的事件。

- [ ] **Step 6: 构建验证**

Run: `cd web; npm run build`
Expected: 无 TS 错误；`web/dist` 更新。

- [ ] **Step 7: 手工验证（dev 模式）**

1. 两个区来回切换：首次挂载各自加载历史；之后切区不重建、不重载、不清空消息。先在两区各发一句后重启 `--headless` 再刷新页面，历史仍互不串区。
2. blender 区发起长任务后立即切到 unity 区：blender 面板的停止按钮与流式输出继续更新；unity 面板不出现 blender 的消息，切回后内容完整。
3. 「清空」弹二次确认，确认后本区清空、另一区不变。
4. blender 区切换低模/云API：模式按钮高亮；未配置云API 时徽标为黄色「未配置」，在设置中填入 tripo apiKey 后刷新为绿色「已配置」（常驻实例不会因切区而重建，设置保存后需重新拉取徽标状态）。
5. 文件树右键弹出五項菜单；重命名/删除均弹输入或确认框（fs.operate 在 P4 接入前预期返回错误帧，不崩溃即可）。
6. 手动构造预览数据（DevTools 中调用组件方法或临时默认值）验证 C# 高亮、图片、信息卡三类渲染。

- [ ] **Step 8: Commit**

```bash
git add web/package.json web/package-lock.json web/src/App.vue web/src/components/ChatPanel.vue web/src/components/FileTree.vue web/src/components/PreviewPane.vue
git commit -m "feat: 对话区历史/清空/截图卡/模式切换，文件树右键菜单与三类预览渲染"
```

---

## P3 完成标准

- [ ] `npm test` 3 用例通过；`npm run build` 无 TS 错误
- [ ] `synthforge_app.exe` 直接启动可见完整布局，且内置 WS 已连通（`?ws=` 注入生效：设置保存回读、未配置 Agent 时聊天返回错误卡）
- [ ] dev 模式下浏览器可完成：设置保存回读、聊天 send → error.card（未配置）/ 文本流（已配置）
- [ ] 活动栏切换 blender/unity 时，顶部栏导入按钮/固定路径正确切换
- [ ] 两个 ChatPanel 常驻：切区只 `v-show`，不重建、不重载历史；消息、流式、busy、草稿/计划/错误卡互不串区，后台区事件不丢失
- [ ] 每个区首次挂载时各自 `session.load`；「清空」二次确认后仅清本区
- [ ] blender 区模式切换 `[低模|云API]` 与云配置徽标可见；`chat.send` 携带 `mode`
- [ ] 文件树右键菜单五項（新建/重命名/删除/资源管理器/刷新）均可触发 `fs.operate`
- [ ] 预览区支持代码高亮/图片/信息卡三类渲染，且 Unity 区也有预览区
