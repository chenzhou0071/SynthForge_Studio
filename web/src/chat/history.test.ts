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

  it("restores plan cards with their stored state", () => {
    const plan = { taskId: "t1", rules: [], groups: [], totalSteps: 3 };
    const content = JSON.stringify(plan);
    const page = {
      messages: [
        { id: 5, role: "plan", content, meta: { state: "started" } },
        { id: 6, role: "plan", content, meta: { state: "cancelled" } },
        { id: 7, role: "plan", content, meta: { state: "pending" } },
        { id: 8, role: "plan", content },
      ],
    };

    expect(toChatItems(page)).toEqual([
      { role: "plan", id: 5, plan, state: "started" },
      { role: "plan", id: 6, plan, state: "cancelled" },
      { role: "plan", id: 7, plan, state: "pending" },
      { role: "plan", id: 8, plan, state: "pending" },
    ]);
  });

  it("skips malformed plan rows without losing the rest of the page", () => {
    const page = {
      messages: [
        { id: 1, role: "plan", content: "{oops" },
        { id: 2, role: "plan", content: JSON.stringify({ taskId: "", groups: [] }) },
        { id: 3, role: "user", content: "还在" },
      ],
    };

    expect(toChatItems(page)).toEqual([{ role: "user", content: "还在" }]);
  });

  it("restores draft cards with origin and state", () => {
    const page = {
      messages: [
        {
          id: 11,
          role: "draft",
          content: "方案 A",
          meta: { state: "pending", origin: "discussion" },
        },
        {
          id: 12,
          role: "draft",
          content: "方案 B",
          meta: { state: "cancelled", origin: "generated" },
        },
      ],
    };

    expect(toChatItems(page)).toEqual([
      { role: "draft", id: 11, content: "方案 A", origin: "discussion", state: "pending" },
      { role: "draft", id: 12, content: "方案 B", origin: "generated", state: "cancelled" },
    ]);
  });
});
