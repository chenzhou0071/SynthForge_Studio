# SynthForge Studio

一个 Windows 桌面的 **AI 建模工作台**：用自然语言驱动 Blender / Unity，把「说一句话 → 拿到可用资产」这条链路真正跑通并能自己调试。

它不是聊天机器人演示，而是把 MCP 客户端、Agent 编排、资产落盘、Unity 工程联动、日志与打包做成一套完整产品的实践项目。

```
你说「做一个雨夜便利店」
  → 摘要 Agent 产出规格草稿 → 你确认
  → 规划 Agent 拆成 4 组建造计划 → 你选存放目录
  → 执行 Agent 通过 Blender MCP 逐组建模、截图自检、迭代
  → 系统统一导出 glb / fbx / blend（fbx 内嵌贴图）
  → Unity 区导入工程，直接把资产摆进场景
```

---

## 目录

- [它能做什么](#它能做什么)
- [技术栈](#技术栈)
- [架构](#架构)
- [构建与运行](#构建与运行)
- [打包与分发](#打包与分发)
- [测试](#测试)
- [已知缺陷与限制](#已知缺陷与限制)
- [致谢](#致谢)
- [目录结构](#目录结构)
- [许可](#许可)

---

## 它能做什么

**两个工作区，同一个核心进程**

| 区域 | 能力 |
|---|---|
| Blender 区 | 低模建模全链（草稿 → 计划 → 逐组执行 → 自检 → 导出）、云 API 生成（只有 **Tripo** 这一条路径用真账号跑通过，其余见[已知缺陷](#已知缺陷与限制)）、修改已有资产、讨论模式（只读问答，模型不能改场景） |
| Unity 区 | 工程导入与互斥激活、Agent 直接编辑场景/脚本/组件、Console 回读自修编译错误、文件树双源同步、添加资产（glb→fbx 转换后导入实例化） |

**界面**：左侧活动栏切区、中部文件树 + 3D 预览、右侧对话区（流式气泡、计划卡、进度卡、截图、错误卡）、顶栏双灯与诊断抽屉。

**工程化配套**：按天轮转的文本日志、启动环境自检（uvx / 两条 MCP 链路）、22 条错误码的用户可读文案与动作按钮、Release 打包脚本、无 Qt 环境的 CI 冒烟。

---

## 技术栈

| 层 | 选型 | 说明 |
|---|---|---|
| 语言 | C++20（MSVC） | `CMAKE_CXX_STANDARD 20` |
| 桌面框架 | Qt 6（要求 ≥6.5，开发用 6.10.3 msvc2022_64） | Core / Gui / Widgets / Sql / Network / WebSockets / **WebEngineWidgets** |
| 界面 | Vue 3.5 + TypeScript 5.6 + Vite 5.4 | 编成 `web/dist` 后打进 Qt qrc，由 `app://ui/` 提供 |
| 3D 预览 | three.js 0.169 | glb 在 WebEngine 内预览 |
| 富文本 | marked 18 + DOMPurify 3.4 + highlight.js 11 | 流式 markdown 渲染与安全过滤 |
| 模型接入 | OpenAI 兼容 `/chat/completions` + SSE | 自研 `LlmGateway`（真实 HTTP 流式、可取消、按区分流） |
| 工具协议 | Model Context Protocol（`2025-06-18`） | 自研 stdio 客户端，两区各一条独立链路 |
| 外部执行 | Blender + `mcp-for-blender`、Unity + `MCP for Unity` | 由 `uvx` 拉起 Python 侧 server |
| 测试 | QtTest + CTest / Vitest 2 + jsdom + @vue/test-utils | 全程 TDD |
| 打包 | CMake install-free（脚本 Copy + windeployqt）+ GitHub Actions | 产物 ~299 MB |

---

## 架构

```
┌──────────────────────── QApplication（GUI 壳）────────────────────────┐
│ QWebEngineView ── app://ui/index.html ──► Vue SPA（两个 ChatPanel 常驻）│
│        ▲                                                            │
│        │ 自定义 scheme handler + qrc 内嵌 dist                       │
│MainWindow                                                          │
└────────┬────────────────────────────────────────────────────────────┘
         │ 进程内
┌────────▼───────────────── synthforge_core（静态库）──────────────────┐
│ WsServer（127.0.0.1 + token）⇄ Vue 侧 WebSocket 客户端              │
│ ProtocolRouter ── ~30 个方法：chat.send / settings.get / tree.refresh … │
│                                                                     │
│ SummarizerAgent ┐                                                   │
│ DiscussionAgent ├─ LlmGateway（HTTP + SSE，可按 tag abort）          │
│ AgentOrchestrator ┘  ↑                                              │
│        │            ToolBridge：OpenAI tools ⇄ MCP tools 双向转换    │
│        ▼                                                            │
│ McpRuntime(blender) ── McpClient ── StdioTransport ── uvx mcp-for-blender │
│ McpRuntime(unity)  ── McpClient ── StdioTransport ── uvx mcp-for-unity    │
│                                                                     │
│ BlenderGate（同一把 Blender 互斥锁）· TaskManager · SessionStore      │
│ AssetLibrary · Cloud3dService · ProjectService · DirectoryWatcher    │
│ AssetImporter · LogService · EnvChecker                              │
└─────────────────────────────────────────────────────────────────────┘
```

几个刻意的设计决定：

- **只有 C++ 侧是权威**：前端不直连 Blender/Unity，全部经本地 WS，保证断线可控、任务可取消。
- **两条 MCP 链路彼此独立**：Blender 桥崩了不会让 Unity 桥变灰，反之亦然。
- **导出由系统统一执行**：建造/自检阶段模型自己 `export_scene` 会被硬拦，否则撞名生成 `_2`。
- **文件是最终事实**：glb→fbx 转换的 MCP 回执超时不等于失败——命令可能仍在 Blender 端排队，所以以「fbx 是否出现在磁盘上」为准，配合宽限轮询。

---

## 构建与运行

### 前置

| 依赖 | 要求 |
|---|---|
| Windows | 10 / 11 x64（MSVC 工具链，VS2022） |
| Qt | 6.5+ 且含 **WebEngine** 组件（开发用 6.10.3 `msvc2022_64`） |
| CMake | ≥ 3.21 |
| Node.js | 20+ |
| uv / uvx | 必需——两个 MCP server 都由 `uvx` 拉起 |
| Blender | 3.x + `mcp-for-blender` 插件（在编辑器里启用并开服务端） |
| Unity | 工程内安装 `MCP for Unity` 包并让窗口在运行 |
| 模型服务 | 任一 OpenAI 兼容端点 + key；云 3D 另配 Tripo / 混元 / TC3 |

### 开发构建

```powershell
cd web ; npm install ; npm run build          # 先出 dist，会被编进 qrc
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="C:\Qt\6.10.3\msvc2022_64"
cmake --build build --config Debug
build\Debug\synthforge_app.exe                # 双击即可，无黑框
```

前端热更新（推荐做界面时用）：

```powershell
cd web ; npm run dev                          # 起 vite
build\Debug\synthforge_app.exe --dev-url http://127.0.0.1:5173
```

无头模式（无 GUI，只有核心服务，便于脚本与调试）：

```powershell
build\Debug\synthforge_cli.exe --headless --data-dir <目录>
# stderr 打印 READY ws=ws://127.0.0.1:<port>/?token=...
```

### 日志与现场

exe 是 GUI 子系统，**从终端启动不再回显日志**（代价见[已知缺陷](#已知缺陷与限制)）。运行痕迹一律看文件：

```
%APPDATA%\SynthForgeStudio\logs\yyyy-MM-dd.log          按天一个，超 50MB 轮转 -1/-2，启动时清掉 7 天前
%APPDATA%\SynthForgeStudio\settings.json                Agent / 云 API / uvx 等配置
%APPDATA%\SynthForgeStudio\library.db                  资产库索引（Qt SQL）
```

```powershell
Get-Content "$env:APPDATA\SynthForgeStudio\logs\2026-11-01.log" -Wait -Tail 30 -Encoding UTF8
```

日志行格式：`[HH:mm:ss.zzz] [D|I|W|E] category: message`。其中 `错误卡 / 工具失败 / 桥接重连中 / 桥接就绪` 这几行是刻意埋的点位——很多错误只在前端一闪而过，不写盘就永远查不到。

---

## 打包与分发

```powershell
powershell -File scripts\package.ps1 -QtKit C:\Qt\6.10.3\msvc2022_64
# [1/4] 前端构建 → [2/4] CMake Release → [3/4] windeployqt → [4/4] 依赖自检
# 产物：dist\SynthForgeStudio\   （约 299 MB）
```

`check-deps.ps1` 不是装饰，它做三件事：

1. 从 `synthforge_app.exe` 出发**递归解析导入表**（`dumpbin /dependents`），每个依赖必须「在包内」或「Windows 系统自带」，否则判失败——这是「目标机没装 Qt 能不能跑」的静态替身。
2. 强制校核 Qt 的插件与资源是否到位：`platforms\qwindows.dll`、`Qt6WebEngineCore.dll`、`QtWebEngineProcess.exe`、`resources\`、`translations\qtwebengine_locales\`。这些缺一个就是白屏或起不来。
3. 校验 PE **子系统必须是 `Windows GUI`**——退回控制台子系统就会双击弹黑框，而这种事不亲自双击根本发现不了。

已知放行规则：`api-ms-win-*` / `ext-ms-*` 是 Windows 的 API Set，磁盘上没有对应文件，由加载器内联解析，不算缺失依赖。

CI（`.github\workflows\package-smoke.yml`）在**从未装过 Qt 的 runner** 上：构建 Release 无头 CLI → `windeployqt` → 静态自检 → 把 `PATH` 缩到只剩 `C:\Windows\System32;C:\Windows` 起服务，必须在 60 秒内看到 `READY ws=`。GUI 那半边不做真启动（runner 没有桌面会话），靠上面的静态扫描兜。

**目标机需要**：VC++ 2015-2022 x64 可再发行组件。开发机装了 VS 所以本机测不出这个分支，是唯一没被自动化覆盖的装机风险。

---

## 测试

```powershell
ctest --test-dir build -C Debug                 # 36 个 C++ 测试可执行
cd web ; npx vitest run                          # 22 个测试文件
cmake --build build --config Debug ; ctest --test-dir build -C Debug   # 提交前跑全量
```

项目里几条测试纪律，都是踩出来的：

- **LLM 测试必须走真实 HTTP + SSE**。不允许把 `chatStream` 改成可注入回调来绕过网络；由 `MockLlmServer`（`QTcpServer`）在 TCP 上回真实的分块 SSE 字节。判据：把 `LlmGateway.cpp` 里 `readyRead` / `finished` 任一分支删掉，测试必须变红。
- **契约靠测试强制同步**。`ErrorCard.test.ts` 会遍历后端实际会发出的全部错误码，断言每条都有中文文案、且不把码本身显示给用户——新增错误码忘了配文案就会红。
- **改完先怀疑自己的测试，再怀疑实现**。反例：`reply.value("accepted").toBool() == false` 看着通过，其实响应帧是 `{id, result}`，字段取不到时默认就是 false——假通过。
- **变异校验**：轮转、级别过滤、防递归这几条防护，都故意破坏过一次，确认对应用例真的会红。

---

## 已知缺陷与限制

### 平台与环境

- **仅支持 Windows**。MSVC 工具链、`windeployqt`、Qt WebEngine、控制台本地码页输出、路径分隔符假设，都没在 Linux/macOS 上验证过。
- **依赖本机 PATH 里有 `uvx`**。GUI 子系统启动后看不到终端输出，装没装 uv 的报错只能从日志文件里翻。
- **控制台跟随本地码页、日志文件固定 UTF-8**。在非 GBK 的中文环境（或代码页被改过）没有验证。
- **WebEngine 对 GPU 敏感的机器可能在缩放窗口时闪蓝块**，兜底是 `--software-render` 或 `SYNTHFORGE_SOFTWARE_RENDER=1`。
- 产物 ~299 MB 未做裁剪（WebEngine 全家桶 + locales），资产库路径固定为 `文档/SynthForgeStudio/ModelLibrary`。

### 并发与链路

- **Blender 是单会话**：建模任务与 glb→fbx 转换共用一把互斥锁，撞上了会看到「Blender 正忙」，必须排队。这是 Blender 侧限制，不是实现偷懒。
- **Unity 编辑器 domain reload 会把桥打成「未找到实例」**。连接类错误会自动重试 2 次（2.5s / 5s），仍失败就停下来要求人工在 MCP for Unity 窗口点重连。
- **MCP 客户端实现是子集**：`initialize` / `tools/list` / `tools/call` / `ping` 心跳 / `progressToken` 透传 / stdio 传输。没有 resources、prompts、sampling、elicitation，也没有 Streamable HTTP 传输与请求取消通知。
- 桥重连的退避到 30 秒封顶后无限重试，但**首次启动失败不自动重试**（顶栏保持灰，发送时由前置检查报错）。

### 云 3D 生成

- **混元 3D / TC3 从未用真实账号跑通过**。只有签名与请求构造的实现，以及 mock 层的协议测试。密钥格式、region、模型名、线上实际返回结构是否与代码里的假设一致，全部未验证——把它当“已实现待验证”，而不是“可用功能”。
- **“自定义服务商”不是通用适配层**。它复用了 Tripo 那条链路的**响应形状假设**：状态字段只认 `queued` / `success` / `failed` / `cancelled`，进度读 `progress`，产物从 `output` 里取，认证固定 `Authorization: Bearer <token>`（提交与查询的 URL 是 `POST {baseUrl}/task` 与 `GET {baseUrl}/task/{id}`，与 Tripo 官方的 `/v3/...` 路径本来就不同）。字段名或结构不同的服务接不上，需要改代码而不是改配置。
- **未知状态一律当“继续轮询”**：`mapState` 把不认识的状态归为 `running`。好处是兼容中间态，代价是对方字段名不一样时会闷头轮到超时，而不是当场告诉你“这个服务我看不懂”。排查时看日志里 `cloud3d` 相关的行。

### 模型行为

- **模型自报的进度不可信**：它会说「已进入第 6 组」而系统其实还在第 4 组。现在靠提示词声明「组号以系统指令为准」，仍属对抗而非根除。
- **空转护栏有阈值**：建造轮一次工具都没调会先催建，连续 2 次空转才判 `group-stalled` 失败。阈值是权衡（太灵敏会误杀正常的纯文本轮）。
- **步数预算用完必须人工点「继续工作」**。刻意不做自动扩预算——失控循环烧钱的代价比多点一次按钮大。
- 自检轮只截一张静图，时序类内容（火车进站内是否穿模）需要人工看动图；动画档虽会按帧截图，但仍不覆盖全部时间线。
- 云 3D 失败原因直接透传服务端文本，措辞与错误码各家不一致。

### 工程与可观测性

- 从终端启动 GUI 版**看不到 stderr**（改成 GUI 子系统消除黑框的代价），要看日志请用文件、`--headless` CLI 或应用内诊断抽屉。
- 诊断抽屉只保留**连接后**的 200 条 warning/error，不回放历史；全量真相只在日志文件里，且只保 7 天。
- **没有崩溃转储、没有遥测**：出问题时得靠用户把日志文件发回来。
- 前端只有组件级测试（jsdom），**没有端到端测试**；Release 与 Debug 的差异（qrc 资源、优化、WebEngine 行为）也没有自动化覆盖，靠打包版本机手测。
- CI 无法验证 GUI 真实启动与 VC++ 运行库缺失分支（见[打包与分发](#打包与分发)）。

---

## 致谢

这个项目的可行性建立在下面这些开源工作之上，特别是最后两类——没有它们，「让模型操作 3D 编辑器」这件事我自己在 Blender 里写不出接口层。

- **[Model Context Protocol](https://modelcontextprotocol.io)** — 开放的工具/上下文协议。本项目是它的客户端实现者之一，规范里 JSON-RPC 的克制设计让一个 400 行的 stdio 客户端就够用。
- **[ahujasid/mcp-for-blender](https://github.com/ahujasid/mcp-for-blender)**（原名 `blender-mcp`）— Blender 侧的 MCP 插件与 stdio server。本项目通过 `uvx mcp-for-blender` 拉起它。
- **[CoplayDev/unity-mcp](https://github.com/CoplayDev/unity-mcp)**（MCP for Unity，PyPI `mcpforunityserver`）— Unity 编辑器侧的 MCP 包。`manage_gameobject` / `read_console` / `execute_code` 等工具都来自它；本项目的工具白名单是在它的接口上裁剪出来的。
- **[astral-sh/uv](https://github.com/astral-sh/uv)** — `uvx` 让 Python 侧 server 无需预装环境，是「双击就能用」链条上的一环。
- **[Qt](https://www.qt.io)**（Qt 6，LGPLv3）与 **Qt WebEngine / Chromium** — 混合架构的另一半；`windeployqt` 顺手解决了 WebEngine 那堆资源的部署难题。
- **[Vue.js](https://vuejs.org)**、**[Vite](https://vitejs.dev)**、**[Vitest](https://vitest.dev)**、**[three.js](https://threejs.org)**、**[marked](https://marked.js.org)**、**[DOMPurify](https://github.com/cure53/DOMPurify)**、**[highlight.js](https://highlightjs.org)** — 界面层。
- **[Blender](https://www.blender.org)** 与 **[Unity](https://unity.com)** 基金会/官方文档与 Python / Editor API。
- 各家提供 OpenAI 兼容接口的模型服务商，以及 **[Tripo](https://tripo3d.ai)**、**腾讯混元 3D** 的云生成服务。

一处需要澄清：Blender 官方也有自己的 [MCP server](https://www.blender.org/lab/mcp-server/)，本项目用的是上面那个社区版本（工具面更贴合建模脚本流），两者不通用。

---

## 目录结构

```
.
├── CMakeLists.txt                 # synthforge_core（静态库）+ app / cli + 36 个测试目标
├── src/
│   ├── main.cpp                   # GUI 壳入口（日志安装、scheme 注册、目录选择注入）
│   ├── cli/main.cpp               # --headless 无头核心 / MCP 冒烟命令
│   ├── app/                       # MainWindow、AppSchemeHandler
│   └── core/
│       ├── mcp/                   # 客户端、stdio 传输、JSON-RPC 编解码、BlenderGate
│       ├── llm/                   # LlmGateway、SseParser、ToolBridge、ToolMessage
│       ├── agent/                 # Summarizer、Discussion、AgentOrchestrator、ShapePolicy
│       ├── ws/                    # WsServer、ProtocolRouter（token 与协议路由）
│       ├── unity/                 # ProjectService、DirectoryWatcher、UnityTools、AssetImporter
│       ├── cloud3d/               # Tripo / 混元 / TC3 / 自定义
│       ├── assets/ store/ tasks/ settings/ preflight/ coreapp/ logging/ env/
│       └── resources/prompts/     # 8 份提示词模板
├── web/
│   └── src/
│       ├── App.vue                # 双区常驻、conn.status / env.status / log.append 接线
│       ├── components/            # ChatPanel ChatMessage ErrorCard LogDrawer TopBar
│       │                          # PlanCard DraftCard AssetCard AssetPicker
│       │                          # FileTree PreviewPane SettingsDialog SceneLoadDialog
│       ├── chat/ ws/ preview/ ui/ # 历史与 markdown、WS 客户端、three.js 预览、状态持久化
│       └── testing/fakeClient.ts  # 组件测试用的 WS 客户端替身
├── tests/                         # 43 个文件：QtTest 用例 + mock/（假 LLM、假 MCP server）
├── scripts/
│   ├── package.ps1                # 打包：前端 → Release → windeployqt → 自检
│   └── check-deps.ps1             # 依赖 / 必需成员 / PE 子系统 三项静态校验
├── .github/workflows/package-smoke.yml
└── docs/superpowers/
    ├── specs/                     # 设计规格
    └── plans/                     # P1~P7 实施计划与验收清单（P1~P6 已勾完，P7 等 CI 与装机验收通过）
```

---

## 许可

**当前仓库未附 `LICENSE` 文件**，因此在法律意义上保留所有权利（not "open source" yet）。

如果要以开源方式发布，需要先做两件事：

1. 选定项目本身的许可证（MIT / Apache-2.0 / GPL-3.0 都有可能，但要看 Qt 的牵连）。
2. 核对 Qt 义务：Qt6 是 **LGPLv3 / GPL 双许可**（商业许可除外），Qt WebEngine 内含 Chromium 等第三方组件。用**动态链接**（本项目即 `Qt6*.dll`）+ 随包提供 Qt 的许可与源码获取声明，是 LGPL 分发的常见合规路径；`translations`、`resources`、`qtwebengine_locales` 这些随包文件不要裁剪掉。

上面的致谢列出的第三方项目各有自己的许可证（例如 `mcp-for-blender`、`MCP for Unity`、`uv`、Vue/three.js/marked/DOMPurify/highlight.js），分发前请逐个核对。
