export type ChatRole = "user" | "assistant" | "error" | "image" | "draft" | "plan" | "group";

export interface PlanPlacement {
  name: string;
  where: string;
}

export interface PlanGroup {
  title: string;
  items: string[];
  placements: PlanPlacement[];
  acceptance: string;
}

export interface PlanData {
  taskId: string;
  rules: string[];
  groups: PlanGroup[];
  totalSteps: number;
}

export interface ChatItem {
  role: ChatRole;
  content?: string;
  dataUrl?: string;
  caption?: string;
  streaming?: boolean;
  plan?: PlanData;
  // 卡片行号：draft/plan 动作要靠它回写对应消息行
  id?: number;
  state?: "pending" | "started" | "cancelled";
  // 草稿来源：讨论产出还是摘要模板产出，决定卡片提示与重生成模板
  origin?: "discussion" | "generated";
  // 分组分隔条：每组建模开始前插一条，截图与文字就不会堆成一团
  groupIndex?: number;
  groupCount?: number;
  title?: string;
  // 步数耗尽失败卡：可点「继续工作」接着跑，不清场
  resumable?: boolean;
}

// session.load 返回 {messages, hasMore, nextCursor}；界面只认能渲染的角色
export function toChatItems(page: unknown): ChatItem[] {
  const messages = (page as { messages?: unknown } | null | undefined)?.messages;
  if (!Array.isArray(messages)) return [];
  const items: ChatItem[] = [];
  for (const raw of messages) {
    const message = raw as {
      id?: number;
      role?: unknown;
      content?: unknown;
      meta?: Record<string, unknown>;
    } | null;
    const content = typeof message?.content === "string" ? message.content : "";
    if (message?.role === "plan") {
      let plan: PlanData;
      try {
        plan = JSON.parse(content) as PlanData;
      } catch {
        continue; // 坏 JSON 不拖垮整页历史
      }
      if (!plan?.taskId || !Array.isArray(plan.groups)) continue;
      const state = (message.meta ?? {}).state;
      items.push({
        role: "plan",
        plan,
        id: message.id,
        state: state === "started" || state === "cancelled" ? state : "pending",
      });
      continue;
    }
    if (message?.role === "draft") {
      const meta = message.meta ?? {};
      items.push({
        role: "draft",
        id: message.id,
        content,
        origin: meta.origin === "discussion" ? "discussion" : "generated",
        state: meta.state === "cancelled" ? "cancelled" : "pending",
      });
      continue;
    }
    if (message?.role !== "user" && message?.role !== "assistant") continue;
    items.push({ role: message.role, content });
  }
  return items;
}
