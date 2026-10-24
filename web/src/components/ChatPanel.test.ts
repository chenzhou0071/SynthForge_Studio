// @vitest-environment jsdom
import { flushPromises, mount } from "@vue/test-utils";
import { afterEach, describe, expect, it, vi } from "vitest";
import ChatPanel from "./ChatPanel.vue";
import { FakeClient } from "../testing/fakeClient";

function mountPanel(area: "blender" | "unity" = "blender", setup?: (client: FakeClient) => void) {
  const client = new FakeClient();
  setup?.(client);
  const wrapper = mount(ChatPanel, { props: { area, client: client.asClient() } });
  return { client, wrapper };
}

afterEach(() => {
  vi.restoreAllMocks();
});

describe("ChatPanel", () => {
  it("loads its own history once on mount", async () => {
    const { client, wrapper } = mountPanel("blender", (fake) => {
      fake.results.set("session.load", {
        messages: [
          { id: 1, role: "user", content: "旧问题" },
          { id: 2, role: "assistant", content: "旧回答" },
        ],
        hasMore: false,
        nextCursor: 1,
      });
    });
    await flushPromises();

    expect(wrapper.text()).toContain("旧问题");
    expect(wrapper.text()).toContain("旧回答");
    const loads = client.callsOf("session.load");
    expect(loads).toHaveLength(1);
    expect(loads[0].params).toMatchObject({ area: "blender" });
  });

  it("streams deltas into one bubble and replaces it with the final message", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();

    client.emit("chat.delta", { area: "blender", text: "你" });
    client.emit("chat.delta", { area: "blender", text: "好" });
    await flushPromises();
    expect(wrapper.text()).toContain("你好");
    expect(wrapper.find(".bubble.streaming").exists()).toBe(true);

    client.emit("chat.message", { area: "blender", role: "assistant", content: "你好呀" });
    await flushPromises();
    expect(wrapper.find(".bubble.streaming").exists()).toBe(false);
    expect(wrapper.text()).toContain("你好呀");
    expect(wrapper.text().match(/你好呀/g)).toHaveLength(1);
  });

  it("ignores events addressed to the other area", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();

    client.emit("chat.delta", { area: "unity", text: "不该出现" });
    client.emit("chat.message", { area: "unity", role: "assistant", content: "也不该出现" });
    await flushPromises();

    expect(wrapper.text()).not.toContain("不该出现");
    expect(wrapper.find(".bubble.streaming").exists()).toBe(false);
  });

  it("sends the blender mode and omits it for unity", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();

    await wrapper.find("textarea").setValue("做一把椅子");
    await wrapper.find("button.primary").trigger("click");
    await flushPromises();
    expect(client.callsOf("chat.send")[0].params).toMatchObject({
      area: "blender",
      text: "做一把椅子",
      mode: "lowpoly",
    });

    // 任务结束后再发第二次，切到云API 后 mode 应随之变化
    client.emit("chat.message", { area: "blender", role: "assistant", content: "ok" });
    await flushPromises();
    await wrapper.findAll(".mode-switch button")[1].trigger("click");
    await wrapper.find("textarea").setValue("再来一个");
    await wrapper.find("button.primary").trigger("click");
    await flushPromises();
    expect(client.callsOf("chat.send")[1].params).toMatchObject({ mode: "cloud3d" });

    const unity = mountPanel("unity");
    await flushPromises();
    await unity.wrapper.find("textarea").setValue("Unity 任务");
    await unity.wrapper.find("button.primary").trigger("click");
    await flushPromises();
    expect(unity.client.callsOf("chat.send")[0].params).not.toHaveProperty("mode");
    expect(unity.wrapper.find(".mode-switch").exists()).toBe(false);

    wrapper.unmount();
    unity.wrapper.unmount();
  });

  it("shows a stop button while busy and cancels through chat.cancel", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();
    client.results.set("chat.send", { accepted: true });

    await wrapper.find("textarea").setValue("长任务");
    await wrapper.find("button.primary").trigger("click");
    await flushPromises();
    expect(wrapper.find("button.stop").exists()).toBe(true);

    await wrapper.find("button.stop").trigger("click");
    await flushPromises();
    expect(client.callsOf("chat.cancel")[0].params).toMatchObject({ area: "blender" });
    expect(wrapper.find("button.stop").exists()).toBe(false);
  });

  it("asks for confirmation before clearing only this area", async () => {
    const { client, wrapper } = mountPanel("blender", (fake) => {
      fake.results.set("session.load", { messages: [{ role: "user", content: "要清掉" }] });
      fake.results.set("chat.clear", { ok: true });
    });
    await flushPromises();
    expect(wrapper.text()).toContain("要清掉");

    vi.spyOn(window, "confirm").mockReturnValue(true);
    const clear = wrapper.findAll("button").find((button) => button.text() === "清空");
    expect(clear).toBeTruthy();
    await clear!.trigger("click");
    await flushPromises();

    expect(client.callsOf("chat.clear")[0].params).toMatchObject({ area: "blender" });
    expect(wrapper.text()).not.toContain("要清掉");
  });

  it("renders screenshot cards and localized error cards", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();

    client.emit("chat.image", {
      area: "blender",
      taskId: "t1",
      dataUrl: "data:image/png;base64,AAA",
      caption: "生成结果",
    });
    client.emit("error.card", { area: "blender", reason: "agent-not-configured" });
    await flushPromises();

    expect(wrapper.find("img.shot").attributes("src")).toBe("data:image/png;base64,AAA");
    expect(wrapper.find(".error-card").text()).toContain("未配置");
  });

  it("refreshes the cloud badge when settings are saved", async () => {
    const { client, wrapper } = mountPanel("blender", (fake) => {
      fake.results.set("settings.get", { cloud3d: { provider: "tripo", tripo: { apiKey: "" } } });
    });
    await flushPromises();
    expect(wrapper.find(".badge").text()).toContain("未配置");

    client.results.set("settings.get", { cloud3d: { provider: "tripo", tripo: { apiKey: "t-key" } } });
    await wrapper.setProps({ settingsVersion: 1 });
    await flushPromises();
    expect(wrapper.find(".badge").text()).toContain("已配置");
  });
});
