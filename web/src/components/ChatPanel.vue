<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, reactive, ref, watch } from "vue";
import { isCloudConfigured } from "../chat/cloud";
import { toChatItems } from "../chat/history";
import type { AssetData, ChatItem, PlanGroup } from "../chat/history";
import type { WsClient } from "../ws/WsClient";
import ChatMessage from "./ChatMessage.vue";
import AssetPicker from "./AssetPicker.vue";
import FolderPicker from "./FolderPicker.vue";
import type { FolderNode } from "./FolderPicker.vue";
import SceneLoadDialog from "./SceneLoadDialog.vue";

type Area = "blender" | "unity";
type ChatMode = "lowpoly" | "cloud3d";

const props = defineProps<{
  area: Area;
  client: WsClient;
  settingsVersion?: number;
  selectedAsset?: string;
  selectedAssetShape?: "prop" | "diorama" | "open";
}>();
// [修改此资产] / 右键菜单设定的目标；提交一条修改指令后清空
const emit = defineEmits<{
  clearAsset: [];
  preview: [path: string];
  openSettings: [target: "agent" | "cloud"];
}>();

const messages = ref<ChatItem[]>([]);
const draft = ref("");
// unity 区「添加资产」：待发送的附件（相对模型库路径）+ 选择器开关
const attachments = ref<string[]>([]);
const pickerOpen = ref(false);
const mode = ref<ChatMode>("lowpoly");
// 讨论只聊方案、不碰 Blender；执行先出草稿卡，确认后才动手
const chatMode = ref<"ask" | "exec">("exec");
// 产出形态三档：道具/盒式舞台/开放场景。默认盒式舞台——等价于改造前的方形微缩行为
type Shape = "prop" | "diorama" | "open";
const shape = ref<Shape>("diorama");
const shapeLabels: Record<Shape, string> = { prop: "道具", diorama: "盒式舞台", open: "开放场景" };
const shapeHint: Record<Shape, string> = {
  prop: "独立单体资产（书桌、招牌、路灯），无底座无场景布局，可直接进引擎复用",
  diorama: "方形底盘的微缩舞台场景，构图不出框，适合展示与收藏",
  open: "无方形边界的大空间（街区、公园、车站），支持长轨道、跨区域与可选动画",
};
// 选中带档位的资产时锁定：有效档位以资产为准，用户不能改成别的模式去破坏它
const shapeLocked = computed(() => !!props.selectedAssetShape);
const effectiveShape = computed<Shape>(() => props.selectedAssetShape ?? shape.value);
// 云API 只走“执行”：没有讨论链路，切过去时立刻退回执行态
watch(mode, (value) => {
  if (value === "cloud3d" && chatMode.value === "ask") chatMode.value = "exec";
  // 有修改目标时不允许停在云API（含 loadUiPrefs 恢复出 cloud3d 的情况）：切回低模
  if (value === "cloud3d" && props.selectedAsset) mode.value = "lowpoly";
});
// 选中资产（修改/讨论目标）就是低模链路：云 API 没有 .blend 可改，
// 从云API 区点低模资产的「修改此资产」必须自动切回低模，否则停在云API 上语义全错
watch(
  () => props.selectedAsset,
  (asset) => {
    if (asset && mode.value === "cloud3d") mode.value = "lowpoly";
  },
);
// 两个开关存后端 ui.ini（uiState.get/set）：Qt WebEngine 的 localStorage 在 app:// 下重启就丢
const prefsReady = ref(false);
async function loadUiPrefs() {
  try {
    const prefs = await props.client.request<{
      blenderMode?: string;
      chatMode?: string;
      shape?: string;
    }>("uiState.get");
    if (prefs?.blenderMode === "cloud3d" || prefs?.blenderMode === "lowpoly") {
      mode.value = prefs.blenderMode;
    }
    if (prefs?.chatMode === "ask" || prefs?.chatMode === "exec") chatMode.value = prefs.chatMode;
    if (prefs?.shape === "prop" || prefs?.shape === "diorama" || prefs?.shape === "open") {
      shape.value = prefs.shape;
    }
    // 旧版本可能存过 cloud3d + ask 的组合：恢复时纠正，别停在不可用的讨论态
    if (mode.value === "cloud3d" && chatMode.value === "ask") chatMode.value = "exec";
  } catch {
    // 读不到偏好就用默认值，不影响对话
  } finally {
    await nextTick(); // 回填触发的 watcher 先跑完，别把刚读到的值再写回去
    prefsReady.value = true;
  }
}
// 用户改一下记一下；执行链路自动切到「执行」也算最近一次选择
watch([mode, chatMode, shape], () => {
  if (!prefsReady.value) return;
  void props.client
    .request("uiState.set", {
      blenderMode: mode.value,
      chatMode: chatMode.value,
      shape: shape.value,
    })
    .catch(() => {
      // 记忆失败不影响切换
    });
});
const cloudConfigured = ref(false);
// 目标资产 chip：只显示文件名，去掉目录与 .blend 后缀
const assetName = computed(
  () => (props.selectedAsset ?? "").split("/").pop()?.replace(/\.blend$/i, "") ?? "",
);
// 每区一个常驻实例：busy 属于本实例，切区只 v-show，状态不会串区
const busy = ref(false);
// 云任务百分比：有值才显示输入框下的细进度条；低模分组进度不经过这里
const cloudPercent = ref<number | null>(null);
const cloudState = ref("");
const busyLabel = computed(() => {
  if (cloudPercent.value === null) return "生成中…";
  if (cloudState.value === "queued") return "排队中…";
  return `生成中 ${cloudPercent.value}%`;
});
// 讨论按钮在云API下置灰，提示文案要跟着解释原因
const chatHint = computed(() => {
  if (mode.value === "cloud3d") return "云API 只走执行";
  if (chatMode.value === "exec") return "先出提示词草稿，确认后才动手";
  // 讨论态：让用户一眼看出这轮能不能真的「看见」Blender 场景（只读，不改）
  if (loadedScene.value) return `讨论中 · 已读取 Blender：${loadedScene.value}（只读，不会改动场景）`;
  if (props.selectedAsset) return "讨论中 · 将先确认载入资产，只读查看，不改动场景";
  return "讨论中 · 不改模型；连上 Blender 后可只读查看场景与参数";
});
const scroller = ref<HTMLElement | null>(null);
// 只有贴底时才跟随新输出；用户往上翻后不再抢视图
const atBottom = ref(true);
const STICK_THRESHOLD = 32; // 距底 32px 内都算贴底，避免像素级抖动
const TOP_THRESHOLD = 40; // 距顶 40px 内触发向前翻页
const PAGE_SIZE = 30; // 每页消息行数，首屏与上翻一致
const hasMore = ref(false);
const nextCursor = ref(0);
const loadingOlder = ref(false); // 翻页请求在飞，防止连续触顶叠请求
// 等第一段文字（或工具结果）之前先给个转圈，别让人盯着空列表
const showPending = computed(() => busy.value && !messages.value.some((item) => item.streaming));
// 计划已确认、等选存放位置：folderPick.open 下发根目录树
const pick = ref<{ requestId: string; rootName: string; tree: FolderNode } | null>(null);
// 讨论前载入资产：后端下发 confirm，弹窗无超时，用户决定后才有「看得见场景」的讨论
const pendingLoad = ref<{
  area: string;
  requestId: string;
  file: string;
  path: string;
} | null>(null);
const loadedScene = ref("");
// 讨论闸门拦下的那句话：载入成功或用户取消后自动补发，省得手动点两次
const pendingAsk = ref("");
// 已就这个资产弹过一次载入确认：取消后不再重复弹，允许纯文字继续
const loadAskedFor = ref("");
const unsubscribers: Array<() => void> = [];
let streaming: ChatItem | null = null; // 指向 messages 里的流式气泡；必须是 reactive 代理，否则收口不触发重渲染
// 用户已点停止：abort 前发出的帧可能晚到，不能让它们把 busy/气泡重新拉起来
let cancelRequested = false;

function reasonText(reason: string, steps?: number, maxSteps?: number) {
  if (reason === "agent-not-configured") {
    return "未配置 Agent：请先在设置中填写服务商、模型与密钥。";
  }
  if (reason === "cloud3d-not-configured") {
    return "未配置云 API：请先在设置中填写 3D 服务商与密钥。";
  }
  if (reason === "max-steps-exceeded") {
    const usage =
      typeof steps === "number" && typeof maxSteps === "number"
        ? `（已用 ${steps} / 上限 ${maxSteps} 步）`
        : "";
    return `步数预算用完了${usage}。前面的成果还在 Blender 场景里，点「继续工作」接着干，不会重新清场。`;
  }
  if (reason === "blender-busy") return "Blender 正忙，等当前任务结束后再试一次。";
  if (reason === "nothing-to-resume") {
    return "这份任务的现场已经不在了（可能已有新任务开始），请重新发指令。";
  }
  return reason;
}

function onScroll() {
  const el = scroller.value;
  if (!el) return;
  atBottom.value = el.scrollHeight - el.scrollTop - el.clientHeight <= STICK_THRESHOLD;
  if (el.scrollTop <= TOP_THRESHOLD) void loadOlder();
}

function scrollToBottom(force = false) {
  if (!force && !atBottom.value) return;
  atBottom.value = true;
  void nextTick(() => {
    if (scroller.value) scroller.value.scrollTop = scroller.value.scrollHeight;
  });
}

function jumpToBottom() {
  scrollToBottom(true);
}

function pushError(
  reason: string,
  options: { resumable?: boolean; message?: string; steps?: number; maxSteps?: number } = {},
) {
  const cloud = reason.startsWith("cloud3d-");
  // 未配置类错误直达设置：Agent 页 / 3D 模型页
  const settings =
    reason === "agent-not-configured"
      ? "agent"
      : reason === "cloud3d-not-configured"
        ? "cloud"
        : undefined;
  messages.value.push({
    role: "error",
    // 云失败透传服务端原因；本地化文案只用于已知的低模失败
    content:
      cloud && options.message
        ? options.message
        : reasonText(reason, options.steps, options.maxSteps),
    resumable: options.resumable === true,
    // 未配置是设置问题，重试没意义；其余云失败可续轮询/重下
    retryable: cloud && reason !== "cloud3d-not-configured",
    settings,
  });
}

async function refreshCloudBadge() {
  if (props.area !== "blender") return;
  try {
    const settings = await props.client.request<unknown>("settings.get");
    cloudConfigured.value = isCloudConfigured(settings);
  } catch {
    cloudConfigured.value = false; // 读不到设置就按未配置提示，别把徽标卡在旧状态
  }
}

function pageMeta(page: unknown) {
  const raw = page as { hasMore?: unknown; nextCursor?: unknown } | null | undefined;
  return {
    hasMore: raw?.hasMore === true,
    nextCursor: typeof raw?.nextCursor === "number" ? raw.nextCursor : 0,
  };
}

async function loadHistory() {
  messages.value = [];
  streaming = null;
  hasMore.value = false;
  nextCursor.value = 0;
  try {
    // 先用一页覆盖常见会话；更早的往上翻再拉
    const page = await props.client.request<unknown>("session.load", {
      area: props.area,
      limit: PAGE_SIZE,
    });
    messages.value = toChatItems(page);
    const meta = pageMeta(page);
    hasMore.value = meta.hasMore;
    nextCursor.value = meta.nextCursor;
  } catch (error) {
    pushError(error instanceof Error ? error.message : "读取历史失败");
  }
  scrollToBottom();
}

// 触顶向前翻页：插到列表头部后补回高度差，当前看的这段不会跳
async function loadOlder() {
  const el = scroller.value;
  if (!el || !hasMore.value || loadingOlder.value) return;
  const beforeHeight = el.scrollHeight;
  const beforeTop = el.scrollTop;
  loadingOlder.value = true;
  try {
    const page = await props.client.request<unknown>("session.load", {
      area: props.area,
      limit: PAGE_SIZE,
      beforeId: nextCursor.value,
    });
    const meta = pageMeta(page);
    hasMore.value = meta.hasMore;
    if (meta.nextCursor > 0) nextCursor.value = meta.nextCursor;
    const older = toChatItems(page);
    if (older.length === 0) return;
    messages.value = [...older, ...messages.value];
    await nextTick();
    const node = scroller.value;
    if (!node) return;
    const added = node.scrollHeight - beforeHeight;
    if (added > 0) node.scrollTop = beforeTop + added;
  } catch (error) {
    pushError(error instanceof Error ? error.message : "读取更早消息失败");
  } finally {
    loadingOlder.value = false;
  }
}

onMounted(() => {
  unsubscribers.push(
    props.client.on("chat.message", (data) => {
      const msg = data as { area?: string; role?: string; content?: string };
      if (msg.area !== props.area || !msg.role) return;
      const content = msg.content ?? "";
      if (msg.role === "assistant") {
        busy.value = false;
        if (streaming) {
          // 流结束：用完整文本覆盖气泡，避免多出一条重复消息
          streaming.content = content;
          streaming.streaming = false;
          streaming = null;
          scrollToBottom();
          return;
        }
      }
      messages.value.push({ role: msg.role === "user" ? "user" : "assistant", content });
      scrollToBottom();
    }),
    props.client.on("chat.delta", (data) => {
      const delta = data as { area?: string; text?: string };
      if (delta.area !== props.area) return;
      if (cancelRequested) return; // 取消后不再显示迟到片段
      if (!streaming) {
        // 裸对象改 streaming 不会触发渲染，光标会留在气泡里；必须持有 reactive 代理
        const item = reactive<ChatItem>({ role: "assistant", content: "", streaming: true });
        messages.value.push(item);
        streaming = item;
      }
      streaming.content += delta.text ?? "";
      busy.value = true; // 重试等非 send 路径也要能显示“停止”
      scrollToBottom();
    }),
    props.client.on("chat.turn", (data) => {
      // 一轮说完：收口当前气泡但任务还在跑，下一段 delta 会另起气泡
      const turn = data as { area?: string; content?: string };
      if (turn.area !== props.area) return;
      const content = turn.content ?? "";
      if (streaming) {
        streaming.content = content;
        streaming.streaming = false;
        streaming = null;
      } else {
        messages.value.push({ role: "assistant", content });
      }
      scrollToBottom();
    }),
    props.client.on("chat.image", (data) => {
      const image = data as { area?: string; dataUrl?: string; caption?: string };
      if (image.area !== props.area) return;
      messages.value.push({ role: "image", dataUrl: image.dataUrl ?? "", caption: image.caption });
      scrollToBottom();
    }),
    props.client.on("task.progress", (data) => {
      const progress = data as {
        area?: string;
        state?: string;
        percent?: number;
        groupIndex?: number;
        groupCount?: number;
        title?: string;
      };
      if (progress.area !== props.area) return;
      // 低模分组进度没有 state；云任务 progress 带终态
      if (typeof progress.groupIndex === "number" && typeof progress.groupCount === "number") {
        messages.value.push({
          role: "group",
          groupIndex: progress.groupIndex,
          groupCount: progress.groupCount,
          title: progress.title ?? "",
        });
        scrollToBottom();
      } else if (typeof progress.percent === "number") {
        if (progress.state === "success" || progress.state === "failed") {
          cloudPercent.value = null;
          cloudState.value = "";
        } else {
          cloudPercent.value = progress.percent;
          cloudState.value = progress.state ?? "";
        }
      }
      busy.value = progress.state !== "success" && progress.state !== "failed";
    }),
    props.client.on("asset.created", (data) => {
      const event = data as { area?: string; id?: number; card?: AssetData };
      if (event.area !== props.area) return;
      busy.value = false; // 落盘即任务结束
      // 后端已先落库再广播：这里补一张卡，刷新后从 session.load 恢复同一行
      if (event.card && typeof event.id === "number") {
        messages.value.push({ role: "asset", id: event.id, asset: event.card });
        scrollToBottom();
      }
    }),
    props.client.on("draft.created", (data) => {
      const d = data as {
        area?: string;
        id?: number;
        prompt?: string;
        origin?: string;
        shape?: "prop" | "diorama" | "open";
      };
      if (d.area !== props.area) return;
      // 后端已停止生成，等用户编辑确认；执行后才进入规划/修改
      busy.value = false;
      messages.value.push({
        role: "draft",
        id: d.id,
        content: d.prompt ?? "",
        origin: d.origin === "discussion" ? "discussion" : "generated",
        shape: d.shape,
        state: "pending",
      });
      scrollToBottom();
    }),
    props.client.on("draft.updated", (data) => {
      const d = data as { area?: string; id?: number; prompt?: string };
      if (d.area !== props.area) return;
      const card = messages.value.find((m) => m.role === "draft" && m.id === d.id);
      if (card) card.content = d.prompt ?? "";
    }),
    props.client.on("draft.done", (data) => {
      const d = data as { area?: string; id?: number; cancelled?: boolean };
      if (d.area !== props.area) return;
      const card = messages.value.find((m) => m.role === "draft" && m.id === d.id);
      if (!card) return;
      if (d.cancelled) {
        card.state = "cancelled"; // 灰卡只读
        return;
      }
      // 草稿已转正：后端马上开始规划/修改。卡片会被删掉，DraftCard 的 sent
      // 可能晚于这个事件才回到前端，所以这里就接管 busy，否则规划期没有转圈。
      cancelRequested = false;
      if (streaming) {
        streaming.streaming = false;
        streaming = null;
      }
      chatMode.value = "exec";
      busy.value = true;
      // 已转成 user 气泡，删卡避免同一段文本出现两次
      messages.value = messages.value.filter((m) => m !== card);
      scrollToBottom(true);
    }),
    props.client.on("plan.created", (data) => {
      const payload = data as {
        area?: string;
        id?: number;
        taskId?: string;
        shape?: "prop" | "diorama" | "open";
        rules?: string[];
        zones?: string[];
        motions?: string[];
        groups?: PlanGroup[];
        totalSteps?: number;
      };
      if (payload.area !== props.area) return;
      // 计划已就绪，后端不再生成；等用户点开始才选存放位置
      busy.value = false;
      messages.value.push({
        role: "plan",
        id: payload.id,
        state: "pending",
        plan: {
          taskId: payload.taskId ?? "",
          shape: payload.shape,
          rules: payload.rules ?? [],
          zones: payload.zones ?? [],
          motions: payload.motions ?? [],
          groups: payload.groups ?? [],
          totalSteps: payload.totalSteps ?? 0,
        },
      });
      scrollToBottom();
    }),
    props.client.on("folderPick.open", (data) => {
      const payload = data as {
        area?: string;
        requestId?: string;
        rootName?: string;
        tree?: FolderNode;
      };
      if (payload.area !== props.area || !payload.requestId || !payload.tree) return;
      busy.value = false; // 计划已确认，选完目录才真正开始建模
      pick.value = {
        requestId: payload.requestId,
        rootName: payload.rootName ?? "ModelLibrary",
        tree: payload.tree,
      };
      // 进入选目录阶段：把这张计划卡标成 started，取消弹窗时再退回 pending（见 onPickClosed）
      setPlanCardState("pending", "started");
    }),
    props.client.on("sceneLoad.confirm", (data) => {
      const payload = data as {
        area?: string;
        requestId?: string;
        file?: string;
        path?: string;
      };
      if (payload?.area !== props.area || !payload.requestId) return;
      pendingLoad.value = {
        area: props.area,
        requestId: payload.requestId,
        file: payload.file ?? "",
        path: payload.path ?? "",
      };
    }),
    props.client.on("sceneLoad.loaded", (data) => {
      const payload = data as { area?: string; file?: string };
      if (payload?.area !== props.area) return;
      loadedScene.value = payload.file ?? "";
      flushPendingAsk(); // 载入好了，把刚才那句被闸门拦下的问题自动发出去
    }),
    props.client.on("sceneLoad.notice", (data) => {
      const payload = data as { area?: string; reason?: string };
      if (payload?.area !== props.area) return;
      // 没连上 / 载入失败：这一轮发不出去，得给个人看得见的理由（无源文件的情况前端入口已封死）
      window.alert(
        payload.reason === "blender-offline"
          ? "未连接 Blender，本轮只能凭文字讨论。"
          : "载入该资产的 .blend 失败，本轮只能凭文字讨论。",
      );
      flushPendingAsk(); // 看不见也得把话说出去，退化成纯文字讨论
    }),
    props.client.on("error.card", (data) => {
      const card = data as {
        area?: string;
        reason?: string;
        resumable?: boolean;
        message?: string;
        steps?: number;
        maxSteps?: number;
      };
      if (card.area !== props.area) return;
      if (card.reason === "user-cancelled") {
        // 用户主动取消（计划卡/选目录弹窗）不是错误，别再弹失败卡
        busy.value = false;
        return;
      }
      if (streaming) {
        // 保留已经收到的半截内容，只是不再显示光标
        streaming.streaming = false;
        streaming = null;
      }
      pushError(card.reason ?? "unknown", {
        resumable: card.resumable,
        message: card.message,
        steps: card.steps,
        maxSteps: card.maxSteps,
      });
      busy.value = false;
      scrollToBottom();
    }),
  );

  if (props.area === "blender") void loadUiPrefs();
  void loadHistory();
  void refreshCloudBadge();
});

// 设置保存后重新拉取徽标状态（对话框在本页内，不会触发重新挂载）
watch(
  () => props.settingsVersion,
  () => void refreshCloudBadge(),
);

onBeforeUnmount(() => {
  unsubscribers.forEach((off) => off());
});

function onAssetsPicked(paths: string[]) {
  for (const path of paths) if (!attachments.value.includes(path)) attachments.value.push(path);
  pickerOpen.value = false;
}

function removeAttachment(path: string) {
  attachments.value = attachments.value.filter((item) => item !== path);
}

async function send() {
  if (busy.value) return;
  const text = draft.value.trim();
  const sendAttachments = props.area === "unity" ? [...attachments.value] : [];
  if (!text && !sendAttachments.length) return;
  cancelRequested = false;
  cloudPercent.value = null; // 新一轮任务从零开始，不沿用上一轮的百分比
  cloudState.value = "";
  draft.value = "";
  attachments.value = []; // 附件随本条消息发出后清空
  busy.value = true;
  // modifyTarget 只由 [修改此资产] / 右键菜单设定，点文件不会触发修改
  const modify = props.area === "blender" && !!props.selectedAsset;
  try {
    const params: Record<string, unknown> = { area: props.area, text };
    if (sendAttachments.length) params.attachments = sendAttachments;
    if (props.area === "blender" && chatMode.value === "ask") {
      // 讨论不检查 Blender；带上当前资产只让模型知道在聊哪个文件
      params.mode = "ask";
      params.shape = effectiveShape.value; // 讨论也要知道在聊哪一档
      if (props.selectedAsset) {
        params.asset = props.selectedAsset;
        if (!loadedScene.value && loadAskedFor.value !== props.selectedAsset) {
          // 第一次讨论这个未载入的资产：先弹无超时确认，扣住这句话；
          // 载入成功或用户取消后由 flushPendingAsk 自动补发，不用手点两次
          loadAskedFor.value = props.selectedAsset;
          pendingAsk.value = text;
          void props.client.request("sceneLoad.request", {
            area: props.area,
            asset: props.selectedAsset,
          });
          draft.value = text; // 载入确认弹窗不该吞掉用户刚写的这轮问题
          busy.value = false;
          return;
        }
        // 已弹过一次（取消后重来 → 纯文字）或场景已载入（→ 带只读工具）：正常发
      }
    } else if (modify) {
      params.mode = "modify";
      params.asset = props.selectedAsset;
    } else if (props.area === "blender") {
      params.mode = mode.value;
      params.shape = effectiveShape.value;
    }
    const result = await props.client.request<{ accepted?: boolean; error?: string }>(
      "chat.send",
      params,
    );
    if (result?.error === "no-blend-source") {
      window.alert("无法修改此资产：源文件已不存在，请重新选择。");
      emit("clearAsset");
    } else if (result?.accepted !== false && modify && chatMode.value !== "ask") {
      emit("clearAsset"); // 一次修改一条指令
    }
    if (result?.accepted === false) busy.value = false;
  } catch (error) {
    busy.value = false;
    pushError(error instanceof Error ? error.message : "发送失败");
  }
  scrollToBottom(true);
}

// 载入确认结束后，把被闸门扣住的那句话补发出去（loaded / notice / 取消都会走到这里）
function flushPendingAsk() {
  if (!pendingAsk.value) return;
  const question = pendingAsk.value;
  pendingAsk.value = "";
  draft.value = question;
  void send();
}

// 草稿已转成正式指令：规划/修改在跑，显示停止与转圈
function onDraftSent() {
  cancelRequested = false;
  // 讨论的流式气泡可能还挂着光标：先收口，否则 showPending 会一直为 false
  if (streaming) {
    streaming.streaming = false;
    streaming = null;
  }
  chatMode.value = "exec"; // 已进入执行链路，底部开关跟着切
  busy.value = true;
  scrollToBottom(true);
}

async function cancelTask() {
  cancelRequested = true;
  try {
    await props.client.request("chat.cancel", { area: props.area });
  } catch (error) {
    cancelRequested = false; // 取消没送达：任务可能还在跑，别把后续输出吞掉
    pushError(error instanceof Error ? error.message : "停止失败");
  } finally {
    // 取消后不收口会留下永久 streaming 气泡，下一次发送就看不到转圈
    if (streaming) {
      streaming.streaming = false;
      streaming = null;
    }
    busy.value = false;
  }
}

// 选完目录已开工：清场/首次请求期间也要转圈，不能等 task.progress 才有反馈
function onPickClosed(started?: boolean) {
  pick.value = null;
  if (started) {
    cancelRequested = false;
    busy.value = true;
    return;
  }
  // × 掉选目录 = 没开工：计划卡从 started 退回 pending，「开始生成」按钮回来，可重选位置
  setPlanCardState("started", "pending");
}

// 把最近一张处于 from 状态的计划卡改成 to（PlanCard 监听 props.state 会跟着变）
function setPlanCardState(from: "pending" | "started", to: "pending" | "started") {
  for (let i = messages.value.length - 1; i >= 0; --i) {
    const item = messages.value[i];
    if (item.role === "plan" && item.state === from) {
      item.state = to;
      return;
    }
  }
}

// 云任务失败后重试：有 taskId 续轮询/重下，不重复提交；没有才重新提交
async function retryCloudTask(item: ChatItem) {
  if (busy.value) return;
  try {
    const result = await props.client.request<{ accepted?: boolean; error?: string }>(
      "chat.retry",
      { area: props.area },
    );
    if (result?.accepted) {
      item.retryable = false; // 已接续，同一张卡不再重复点击
      busy.value = true;
      scrollToBottom(true);
      return;
    }
    item.content =
      result?.error === "no-task-to-retry"
        ? "没有可重试的任务，请重新发送指令。"
        : result?.error ?? "重试失败";
  } catch (error) {
    item.content = error instanceof Error ? error.message : "重试失败";
  }
}

// 步数耗尽的失败卡：不清场续跑，旧会话与分组位置都在后端
async function resumeWork(item: ChatItem) {
  if (busy.value) return;
  cancelRequested = false;
  busy.value = true;
  try {
    const result = await props.client.request<{ ok?: boolean; error?: string }>("chat.continue", {
      area: props.area,
    });
    if (result?.ok) {
      item.resumable = false; // 已接续，同一张卡不再重复点击
      scrollToBottom(true);
      return;
    }
    busy.value = false;
    const reason = result?.error ?? "nothing-to-resume";
    item.content = reasonText(reason);
    if (reason !== "blender-busy") item.resumable = false; // 现场没了，再点也没用
  } catch (error) {
    busy.value = false;
    item.content = error instanceof Error ? error.message : "继续失败";
  }
}

async function clearChat() {
  if (!window.confirm("清空本区对话？此操作不可撤销")) return;
  try {
    await props.client.request("chat.clear", { area: props.area });
  } catch (error) {
    pushError(error instanceof Error ? error.message : "清空失败");
    return;
  }
  messages.value = [];
  streaming = null;
  hasMore.value = false;
  nextCursor.value = 0;
  busy.value = false; // chat.clear 会取消本区任务
}

function onKeydown(event: KeyboardEvent) {
  if (event.key === "Enter" && !event.shiftKey) {
    event.preventDefault();
    void send();
  }
}
</script>

<template>
  <section class="chat">
    <header class="panel-head chat-head">
      <span>AI 对话</span>
      <span class="chat-area">{{ area === "blender" ? "Blender" : "Unity" }}</span>
      <span class="spacer" />
      <button class="ghost" title="清空本区对话" @click="clearChat">清空</button>
    </header>

    <div v-if="area === 'blender'" class="chat-tools">
      <div class="mode-switch">
        <button :class="{ active: mode === 'lowpoly' }" @click="mode = 'lowpoly'">低模</button>
        <button :class="{ active: mode === 'cloud3d' }" @click="mode = 'cloud3d'">云API</button>
      </div>
      <div v-if="mode === 'lowpoly'" class="shape-switch" :class="{ locked: shapeLocked }">
        <button
          v-for="(label, key) in shapeLabels"
          :key="key"
          :class="{ active: effectiveShape === key }"
          :disabled="shapeLocked"
          :title="shapeLocked ? '已锁定为该资产的模式，改档需先取消选中' : shapeHint[key]"
          @click="shape = key as Shape"
        >
          {{ label }}
        </button>
      </div>
      <span class="badge" :class="cloudConfigured ? 'ok' : 'warn'">
        {{ cloudConfigured ? "云API 已配置" : "云API 未配置" }}
      </span>
    </div>

    <div class="chat-body">
      <div ref="scroller" class="chat-log" @scroll="onScroll">
        <div v-if="loadingOlder" class="older-loading">加载更早的消息…</div>
        <p v-if="messages.length === 0" class="chat-empty">描述你要生成的资产或要做的修改。</p>

        <!-- 每条消息交给子组件渲染：流式时只有这一条因 item.content 变化而重渲，不牵动整个列表 -->
        <ChatMessage
          v-for="(message, index) in messages"
          :key="index"
          :item="message"
          :area="area"
          :client="client"
          :busy="busy"
          @preview="(path: string) => emit('preview', path)"
          @open-settings="(target: 'agent' | 'cloud') => emit('openSettings', target)"
          @resume="resumeWork"
          @retry="retryCloudTask"
          @draft-sent="onDraftSent"
        />

        <div v-if="showPending" class="bubble assistant pending">
          <span class="dot" /><span class="dot" /><span class="dot" />
        </div>
      </div>

      <button v-if="!atBottom" class="jump-bottom" title="回到底部" @click="jumpToBottom">
        <svg
          width="14"
          height="14"
          viewBox="0 0 24 24"
          fill="none"
          stroke="currentColor"
          stroke-width="2.5"
          stroke-linecap="round"
          stroke-linejoin="round"
        >
          <path d="M12 5v14M5 12l7 7 7-7" />
        </svg>
      </button>
    </div>

    <footer class="chat-input">
      <div v-if="area === 'blender'" class="chat-mode">
        <div class="mode-switch">
          <button
            :class="{ active: chatMode === 'ask' }"
            :disabled="mode === 'cloud3d'"
            :title="mode === 'cloud3d' ? '云API 模式下不提供讨论，直接出云提示词草稿' : ''"
            @click="chatMode = 'ask'"
          >
            讨论
          </button>
          <button :class="{ active: chatMode === 'exec' }" @click="chatMode = 'exec'">执行</button>
        </div>
        <span class="chat-mode-hint">{{ chatHint }}</span>
      </div>
      <div v-if="selectedAsset" class="asset-chip">
        <span>目标资产：{{ assetName }}</span>
        <button type="button" title="取消修改目标" @click="emit('clearAsset')">✕</button>
      </div>
      <div v-if="area === 'unity' && attachments.length" class="attach-chips">
        <span v-for="path in attachments" :key="path" class="chip">
          {{ path.split("/").pop() }}
          <button type="button" title="移除附件" @click="removeAttachment(path)">✕</button>
        </span>
      </div>
      <textarea
        v-model="draft"
        rows="3"
        :placeholder="
          selectedAsset
            ? '描述要怎么修改这个资产，Enter 发送'
            : '输入消息，Enter 发送，Shift+Enter 换行'
        "
        @keydown="onKeydown"
      />
      <div v-if="busy && cloudPercent !== null" class="cloud-progress">
        <div class="fill" :style="{ width: `${cloudPercent}%` }"></div>
      </div>
      <div class="chat-actions">
        <button
          v-if="area === 'unity'"
          class="add-asset"
          :disabled="busy"
          @click="pickerOpen = true"
        >
          添加资产
        </button>
        <button v-if="busy" class="stop" @click="cancelTask">停止</button>
        <button
          class="primary"
          :disabled="busy || (!draft.trim() && !(area === 'unity' && attachments.length))"
          @click="send"
        >
          {{ busy ? busyLabel : "发送" }}
        </button>
      </div>
    </footer>

    <FolderPicker
      v-if="pick"
      :client="client"
      :area="area"
      :request-id="pick.requestId"
      :root-name="pick.rootName"
      :tree="pick.tree"
      @close="onPickClosed"
    />

    <SceneLoadDialog
      v-if="pendingLoad"
      :client="client"
      :load="pendingLoad"
      @accepted="pendingLoad = null"
      @declined="
        pendingLoad = null;
        flushPendingAsk();
      "
    />

    <AssetPicker
      v-if="pickerOpen"
      :client="client"
      @close="pickerOpen = false"
      @confirm="onAssetsPicked"
    />
  </section>
</template>

<style scoped>
.chat-body {
  position: relative;
  flex: 1;
  min-height: 0;
  display: flex;
}

/* 往上翻后才出现的回底按钮，悬浮在聊天区底部正中心 */
.jump-bottom {
  position: absolute;
  left: 50%;
  bottom: 12px;
  transform: translateX(-50%);
  display: grid;
  place-items: center;
  width: 32px;
  height: 32px;
  padding: 0;
  border-radius: 50%;
  border: 1px solid var(--border);
  background: var(--panel);
  color: var(--muted);
  box-shadow: 0 2px 10px rgba(0, 0, 0, 0.35);
  cursor: pointer;
}

.jump-bottom:hover {
  color: var(--accent);
  border-color: var(--accent);
}

.older-loading {
  text-align: center;
  font-size: 12px;
  color: var(--muted);
}

.chat-head {
  gap: 6px;
}

.spacer {
  flex: 1;
}

.ghost {
  background: transparent;
  border: 1px solid var(--border);
  color: var(--muted);
  padding: 2px 8px;
  border-radius: 3px;
  font-size: 12px;
}

.ghost:hover {
  color: var(--text);
  border-color: var(--muted);
}

.chat-tools {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: 8px;
  padding: 0 12px 8px;
}

/* 输入区左上的讨论/执行开关 */
.chat-mode {
  display: flex;
  align-items: center;
  gap: 8px;
}

.chat-mode-hint {
  color: var(--muted);
  font-size: 12px;
  min-width: 0; /* 弹提示换行，不要把开关挤成竖排 */
}

.mode-switch {
  display: inline-flex;
  flex: none; /* 不被旁边的提示文案压缩 */
  border: 1px solid var(--border);
  border-radius: 4px;
  overflow: hidden;
}

.mode-switch button {
  border: 0;
  border-radius: 0;
  padding: 3px 10px;
  font-size: 12px;
  white-space: nowrap;
  background: transparent;
  color: var(--muted);
}

.mode-switch button.active {
  background: var(--accent);
  color: #fff;
}

.mode-switch button:disabled {
  opacity: 0.35;
  background: transparent;
  cursor: not-allowed;
}

/* 三档产出形态开关：观感与模式开关一致，只是多一档 */
.shape-switch {
  display: inline-flex;
  border: 1px solid var(--border);
  border-radius: 4px;
  overflow: hidden;
}

.shape-switch button {
  border: 0;
  border-radius: 0;
  padding: 3px 10px;
  font-size: 12px;
  white-space: nowrap;
  background: transparent;
  color: var(--muted);
  cursor: pointer;
}

.shape-switch button.active {
  background: var(--accent);
  color: #fff;
}

.shape-switch.locked button {
  cursor: not-allowed;
}

.shape-switch.locked button:not(.active) {
  opacity: 0.4;
}

.badge {
  font-size: 12px;
  color: var(--muted);
  white-space: nowrap;
}

.badge.ok {
  color: #4ec9b0;
}

.badge.warn {
  color: #cca700;
}

.cloud-progress {
  height: 2px;
  margin-top: 2px;
  background: rgba(255, 255, 255, 0.08);
  overflow: hidden;
}

.cloud-progress .fill {
  height: 100%;
  background: var(--accent);
  transition: width 0.3s ease;
}

.chat-actions {
  display: flex;
  justify-content: flex-end;
  gap: 8px;
}

/* 输入框上方的修改目标，提交成功后由 App 清空 */
.asset-chip {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 4px 8px;
  border: 1px solid var(--border);
  border-radius: 4px;
  background: var(--bg);
  color: var(--muted);
  font-size: 12px;
}

.asset-chip button {
  display: grid;
  place-items: center;
  width: 18px;
  height: 18px;
  padding: 0;
  border: 1px solid var(--border);
  border-radius: 50%;
  background: transparent;
  color: var(--muted);
  font-size: 11px;
  line-height: 1;
  cursor: pointer;
}

.asset-chip button:hover {
  color: var(--text);
  border-color: var(--muted);
}

/* 附件 chip：unity 区待发送的模型库资产 */
.attach-chips {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin: 4px 0;
}

.attach-chips .chip {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  padding: 2px 8px;
  border: 1px solid var(--border);
  border-radius: 10px;
  background: var(--bg);
  color: var(--muted);
  font-size: 12px;
}

.attach-chips .chip button {
  border: 0;
  background: transparent;
  color: var(--muted);
  cursor: pointer;
  font-size: 11px;
  line-height: 1;
}

.add-asset {
  margin-right: auto;
  border: 1px solid var(--border);
  border-radius: 3px;
  background: transparent;
  color: var(--muted);
  padding: 4px 12px;
  font-size: 12px;
  cursor: pointer;
}

.add-asset:hover {
  color: var(--text);
  border-color: var(--muted);
}

.add-asset:disabled {
  opacity: 0.5;
  cursor: default;
}

.pending {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  padding: 10px;
}

.pending .dot {
  width: 5px;
  height: 5px;
  border-radius: 50%;
  background: var(--muted);
  animation: pending-dot 1s ease-in-out infinite;
}

.pending .dot:nth-child(2) {
  animation-delay: 0.15s;
}

.pending .dot:nth-child(3) {
  animation-delay: 0.3s;
}

@keyframes pending-dot {
  0%,
  80%,
  100% {
    opacity: 0.25;
  }
  40% {
    opacity: 1;
  }
}

.chat-actions .primary {
  align-self: auto;
}

.stop {
  background: transparent;
  border: 1px solid var(--danger-border);
  color: #f48771;
  border-radius: 3px;
  padding: 6px 14px;
}
</style>
