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
  taskId?: string;
  mode?: "lowpoly" | "cloud3d";
  plan?: PlanData;
  // 分组分隔条：每组建模开始前插一条，截图与文字就不会堆成一团
  groupIndex?: number;
  groupCount?: number;
  title?: string;
  // 步数耗尽失败卡：可点「继续工作」接着跑，不清场
  resumable?: boolean;
}

// session.load 返回 {messages, hasMore, nextCursor}；界面只认能渲染的两种角色
export function toChatItems(page: unknown): ChatItem[] {
  const messages = (page as { messages?: unknown } | null | undefined)?.messages;
  if (!Array.isArray(messages)) return [];
  const items: ChatItem[] = [];
  for (const raw of messages) {
    const message = raw as { role?: unknown; content?: unknown } | null;
    if (message?.role !== "user" && message?.role !== "assistant") continue;
    items.push({
      role: message.role,
      content: typeof message.content === "string" ? message.content : "",
    });
  }
  return items;
}
