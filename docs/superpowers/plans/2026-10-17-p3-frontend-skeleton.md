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
- `TopBar`：props `area`、`blender: LampState`、`unity: LampState`（`'off'|'ready'|'reconnecting'`）、`hints: {blender?: string; unity?: string}`；emits `importProject()`。
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
        <ChatPanel :area="active" :client="client" />
      </div>
    </div>
    <SettingsDialog v-model:open="settingsOpen" :client="client" />
  </div>
</template>
```

`styles.css`：`.app{display:flex;height:100vh}` `.main{flex:1;display:flex;flex-direction:column}` `.content{flex:1;display:flex;min-height:0}` 活动栏 48px、聊天面板固定 380px 最右侧、暗色变量 `--bg:#1e1e1e; --panel:#252526; --text:#cccccc; --accent:#0e639c`。

`TopBar.vue` 双灯：

```vue
<script setup lang="ts">
defineProps<{ area: string; blender: string; unity: string }>();
const color = (s: string) => (s === "ready" ? "#4ec9b0" : s === "reconnecting" ? "#cca700" : "#555");
</script>
<template>
  <header class="topbar">
    <span v-if="area === 'blender'" class="fixed-path">模型库：文档/SynthForgeStudio/ModelLibrary（固定）</span>
    <button v-else class="import">导入 Unity 工程</button>
    <span class="spacer" />
    <span class="lamp" title="Blender 链路"><i :style="{ background: color(blender) }" />Blender</span>
    <span class="lamp" title="Unity 链路"><i :style="{ background: color(unity) }" />Unity</span>
  </header>
</template>
```

`ChatPanel.vue` 核心逻辑：消息列表 + 输入框；`send()` → `client.request("chat.send", {area, text})`；订阅 `chat.message` append；`chat.delta` 追加到 streaming 气泡；`error.card` 显示红色卡片。

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
  if (v && !settings.value) settings.value = await props.client.request("settings.get");
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
- `main.cpp`：`--dev-url <http://localhost:5173>` 优先；否则 `app://ui/index.html`；`QWebEngineUrlScheme` 注册必须在 `QApplication` 构造前。

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
    const QUrl uiUrl = devIdx >= 0 && devIdx + 1 < args.size()
                           ? QUrl(args.at(devIdx + 1))
                           : QUrl("app://ui/index.html");

    auto* handler = new AppSchemeHandler(&app);
    MainWindow window(uiUrl);
    window.show();
    return app.exec();
}
```

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
Expected: 窗口加载 `app://ui/index.html`，布局与 dev 一致（无网络资源 404）。

- [ ] **Step 3: Commit**（仅当有配置修正时；纯验证不提交）

---

## P3 完成标准

- [ ] `npm test` 3 用例通过；`npm run build` 无 TS 错误
- [ ] `synthforge_app.exe` 直接启动可见完整布局（prod 模式）
- [ ] dev 模式下浏览器可完成：设置保存回读、聊天 send → error.card（未配置）/ 文本流（已配置）
- [ ] 活动栏切换 blender/unity 时，顶部栏导入按钮/固定路径正确切换
