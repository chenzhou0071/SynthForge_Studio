export type ChatRole = "user" | "assistant" | "error" | "image";

export interface ChatItem {
  role: ChatRole;
  content?: string;
  dataUrl?: string;
  caption?: string;
  streaming?: boolean;
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
