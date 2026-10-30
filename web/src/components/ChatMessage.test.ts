// @vitest-environment jsdom
import { flushPromises, mount } from "@vue/test-utils";
import { describe, expect, it } from "vitest";
import ChatMessage from "./ChatMessage.vue";
import type { ChatItem } from "../chat/history";
import { FakeClient } from "../testing/fakeClient";

// 只测「单条消息」子组件：父级传一条 item，子级按 role 渲染并把交互事件回抛。
// 拆分的目的就是让流式只重渲这一条，所以这里锁住它的渲染契约与 emit 契约。
function mountMessage(item: ChatItem, props: Record<string, unknown> = {}) {
  const client = new FakeClient();
  const wrapper = mount(ChatMessage, {
    props: { item, area: "blender", client: client.asClient(), busy: false, ...props },
  });
  return { client, wrapper };
}

describe("ChatMessage", () => {
  it("renders an assistant bubble through markdown", () => {
    const { wrapper } = mountMessage({ role: "assistant", content: "**粗体**回答" });
    const bubble = wrapper.find(".bubble.assistant .md");
    expect(bubble.exists()).toBe(true);
    expect(bubble.find("strong").text()).toBe("粗体");
  });

  it("shows the streaming caret only while the item is streaming", () => {
    const { wrapper } = mountMessage({ role: "assistant", content: "半截", streaming: true });
    expect(wrapper.find(".bubble.streaming").exists()).toBe(true);
    expect(wrapper.find(".caret").exists()).toBe(true);
  });

  it("renders an image bubble with its screenshot", () => {
    const { wrapper } = mountMessage({
      role: "image",
      dataUrl: "data:image/png;base64,AAA",
      caption: "生成结果",
    });
    expect(wrapper.find("img.shot").attributes("src")).toBe("data:image/png;base64,AAA");
    expect(wrapper.find(".caption").text()).toBe("生成结果");
  });

  it("emits resume when a resumable error card is clicked", async () => {
    const item: ChatItem = { role: "error", content: "步数用完", resumable: true };
    const { wrapper } = mountMessage(item);
    const button = wrapper.findAll(".error-card button").find((b) => b.text() === "继续工作");
    expect(button).toBeTruthy();
    await button!.trigger("click");
    expect(wrapper.emitted("resume")?.[0]).toEqual([item]);
  });

  it("emits retry when a retryable error card is clicked", async () => {
    const item: ChatItem = { role: "error", content: "云失败", retryable: true };
    const { wrapper } = mountMessage(item);
    const button = wrapper.findAll(".error-card button").find((b) => b.text() === "重试");
    expect(button).toBeTruthy();
    await button!.trigger("click");
    expect(wrapper.emitted("retry")?.[0]).toEqual([item]);
  });

  it("emits openSettings when the not-configured card is clicked", async () => {
    const { wrapper } = mountMessage({
      role: "error",
      content: "未配置",
      settings: "agent",
    });
    const button = wrapper.findAll(".error-card button").find((b) => b.text() === "去设置");
    expect(button).toBeTruthy();
    await button!.trigger("click");
    expect(wrapper.emitted("openSettings")?.[0]).toEqual(["agent"]);
  });

  it("disables the resume button while the panel is busy", () => {
    const { wrapper } = mountMessage(
      { role: "error", content: "步数用完", resumable: true },
      { busy: true },
    );
    const button = wrapper.findAll(".error-card button").find((b) => b.text() === "继续工作");
    expect((button!.element as HTMLButtonElement).disabled).toBe(true);
  });

  it("re-emits preview when the asset card is opened", async () => {
    const { wrapper } = mountMessage({
      role: "asset",
      asset: {
        name: "chair_01",
        source: "lowpoly",
        files: [{ suffix: "glb", path: "props/chair_01.glb" }],
      },
    });
    expect(wrapper.find(".asset-card").exists()).toBe(true);
    await wrapper.find(".asset-card button.view").trigger("click");
    expect(wrapper.emitted("preview")?.[0]).toEqual(["props/chair_01.glb"]);
  });

  it("renders a group divider with its index and title", () => {
    const { wrapper } = mountMessage({
      role: "group",
      groupIndex: 2,
      groupCount: 4,
      title: "主体",
    });
    const divider = wrapper.find(".group-divider");
    expect(divider.text()).toContain("第 2/4 组");
    expect(divider.text()).toContain("主体");
  });

  it("renders a plan card for a plan item", () => {
    const { wrapper } = mountMessage({
      role: "plan",
      id: 5,
      state: "pending",
      plan: {
        taskId: "t1",
        rules: ["留出通道"],
        groups: [{ title: "底座", items: ["base"], placements: [], acceptance: "宽度 22" }],
        totalSteps: 4,
      },
    });
    expect(wrapper.find(".plan-card").exists()).toBe(true);
    expect(wrapper.find(".plan-card").text()).toContain("底座");
  });

  // draft 卡片在子级里被点「执行」后，应把 draftSent 回抛给父级去收敛
  it("re-emits draftSent once the draft card is executed", async () => {
    const { client, wrapper } = mountMessage({ role: "draft", id: 24, content: "方案" });
    client.results.set("draft.send", { accepted: true });
    const run = wrapper.findAll(".draft-card button").find((b) => b.text() === "执行");
    expect(run).toBeTruthy();
    await run!.trigger("click");
    await flushPromises();
    expect(wrapper.emitted("draftSent")).toHaveLength(1);
  });
});
