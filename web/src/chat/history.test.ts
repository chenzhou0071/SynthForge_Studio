import { describe, expect, it } from "vitest";
import { toChatItems } from "./history";

describe("toChatItems", () => {
  it("keeps stored messages in their original order", () => {
    const page = {
      messages: [
        { id: 1, role: "user", content: "第一句" },
        { id: 2, role: "assistant", content: "第二句" },
      ],
      hasMore: false,
      nextCursor: 1,
    };

    expect(toChatItems(page)).toEqual([
      { role: "user", content: "第一句" },
      { role: "assistant", content: "第二句" },
    ]);
  });

  it("drops roles the chat panel cannot render", () => {
    const page = {
      messages: [
        { role: "system", content: "内部提示" },
        { role: "tool", content: "工具结果" },
        { role: "user", content: "保留我" },
      ],
    };

    expect(toChatItems(page)).toEqual([{ role: "user", content: "保留我" }]);
  });

  it("treats a missing or malformed page as empty history", () => {
    expect(toChatItems(undefined)).toEqual([]);
    expect(toChatItems(null)).toEqual([]);
    expect(toChatItems({})).toEqual([]);
    expect(toChatItems({ messages: "oops" })).toEqual([]);
  });

  it("falls back to an empty string for non-string content", () => {
    expect(toChatItems({ messages: [{ role: "assistant", content: 42 }] })).toEqual([
      { role: "assistant", content: "" },
    ]);
  });
});
