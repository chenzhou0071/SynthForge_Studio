// @vitest-environment jsdom
import { flushPromises, mount } from "@vue/test-utils";
import { afterEach, describe, expect, it, vi } from "vitest";
import ChatPanel from "./ChatPanel.vue";
import { FakeClient } from "../testing/fakeClient";

function mountPanel(
  area: "blender" | "unity" = "blender",
  setup?: (client: FakeClient) => void,
  props: Record<string, unknown> = {},
) {
  const client = new FakeClient();
  setup?.(client);
  const wrapper = mount(ChatPanel, { props: { area, client: client.asClient(), ...props } });
  return { client, wrapper };
}

// jsdom 不做布局，scrollTop 要自己接管，才能验证“跟随 / 不跟随”
function makeScrollable(el: HTMLElement, scrollHeight: number, clientHeight: number, top: number) {
  const state = { top, height: scrollHeight };
  Object.defineProperty(el, "scrollHeight", { configurable: true, get: () => state.height });
  Object.defineProperty(el, "clientHeight", { configurable: true, get: () => clientHeight });
  Object.defineProperty(el, "scrollTop", {
    configurable: true,
    get: () => state.top,
    set: (value: number) => {
      state.top = value;
    },
  });
  return state;
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
    expect(loads[0].params).toMatchObject({ area: "blender", limit: 30 });
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

  it("renders streaming text literally until the turn is finalized", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();

    client.emit("chat.delta", { area: "blender", text: "**粗体**" });
    await flushPromises();
    // 流式期间不做 markdown 解析：每个 token 全量 parse + sanitize 会把渲染线程压死
    expect(wrapper.find(".bubble.streaming .md").text()).toContain("**粗体**");
    expect(wrapper.find(".bubble.streaming .md strong").exists()).toBe(false);

    client.emit("chat.turn", { area: "blender", content: "**粗体**" });
    await flushPromises();
    expect(wrapper.find(".bubble .md strong").text()).toBe("粗体");
  });

  it("stops the streaming caret even when busy was already false", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();

    client.emit("chat.delta", { area: "blender", text: "最后一段" });
    await flushPromises();
    // 产物先落盘，busy 已经变 false；此时最后一段文字可能还在流
    client.emit("asset.created", { area: "blender" });
    await flushPromises();
    expect(wrapper.find(".bubble.streaming").exists()).toBe(true);

    client.emit("chat.message", { area: "blender", role: "assistant", content: "最后一段" });
    await flushPromises();
    expect(wrapper.find(".bubble.streaming").exists()).toBe(false);
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

  it("shows a pending bubble until the first token arrives", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();

    await wrapper.find("textarea").setValue("做一把椅子");
    await wrapper.find("button.primary").trigger("click");
    await flushPromises();
    expect(wrapper.find(".bubble.pending").exists()).toBe(true);

    client.emit("chat.delta", { area: "blender", text: "好" });
    await flushPromises();
    expect(wrapper.find(".bubble.pending").exists()).toBe(false);
  });

  it("closes the current bubble when a turn ends and starts a new one for the next", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();
    client.results.set("chat.send", { accepted: true });
    await wrapper.find("textarea").setValue("做便利店");
    await wrapper.find("button.primary").trigger("click");
    await flushPromises();

    client.emit("chat.delta", { area: "blender", text: "第一组完成" });
    client.emit("chat.turn", { area: "blender", content: "第一组完成" });
    client.emit("chat.delta", { area: "blender", text: "第二组开始" });
    await flushPromises();

    const bubbles = wrapper
      .findAll(".bubble.assistant")
      .filter((bubble) => !bubble.classes("pending"));
    expect(bubbles).toHaveLength(2);
    expect(bubbles[0].text()).toContain("第一组完成");
    expect(bubbles[1].text()).toContain("第二组开始");
    expect(wrapper.find("button.stop").exists()).toBe(true); // 任务还在跑，别清掉 busy
    expect(wrapper.find(".bubble.streaming").text()).toContain("第二组开始");
  });

  it("shows pending dots as soon as the folder picker is confirmed", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();
    client.results.set("folderPick.submit", { ok: true });

    client.emit("folderPick.open", {
      area: "blender",
      requestId: "pick-start",
      rootName: "ModelLibrary",
      tree: { name: "ModelLibrary", path: "", children: [] },
    });
    await flushPromises();

    const confirm = wrapper.findAll("button").find((button) => button.text() === "确认");
    await confirm!.trigger("click");
    await flushPromises();

    expect(wrapper.find(".folder-picker").exists()).toBe(false);
    expect(wrapper.find(".bubble.pending").exists()).toBe(true); // 清场/首次请求期间也不能空着
  });

  it("clears the streaming caret after a cancel so the next send shows pending", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();
    client.results.set("chat.send", { accepted: true });
    await wrapper.find("textarea").setValue("长任务");
    await wrapper.find("button.primary").trigger("click");
    await flushPromises();

    client.emit("chat.delta", { area: "blender", text: "半截" });
    await flushPromises();
    await wrapper.find("button.stop").trigger("click");
    await flushPromises();
    expect(wrapper.find(".bubble.streaming").exists()).toBe(false);

    await wrapper.find("textarea").setValue("继续做");
    await wrapper.find("button.primary").trigger("click");
    await flushPromises();
    expect(wrapper.find(".bubble.pending").exists()).toBe(true);
  });

  it("keeps the stop button hidden when a late delta arrives after cancel", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();
    client.results.set("chat.send", { accepted: true });
    await wrapper.find("textarea").setValue("长任务");
    await wrapper.find("button.primary").trigger("click");
    await flushPromises();

    client.emit("chat.delta", { area: "blender", text: "半截" });
    await flushPromises();
    await wrapper.find("button.stop").trigger("click");
    await flushPromises();

    // abort 已经在路上，但取消前发出的帧可能晚到；不能把 busy/气泡重新拉起来
    client.emit("chat.delta", { area: "blender", text: "迟到" });
    await flushPromises();
    expect(wrapper.find("button.stop").exists()).toBe(false);
    expect(wrapper.find(".bubble.streaming").exists()).toBe(false);
  });

  it("inserts a divider for every build group", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();

    client.emit("task.progress", {
      area: "blender",
      groupIndex: 1,
      groupCount: 4,
      title: "底座",
    });
    await flushPromises();

    const divider = wrapper.find(".group-divider");
    expect(divider.exists()).toBe(true);
    expect(divider.text()).toContain("第 1/4 组");
    expect(divider.text()).toContain("底座");
  });

  it("renders assistant markdown tables", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();

    client.emit("chat.message", {
      area: "blender",
      role: "assistant",
      content: "| 物体 | 尺寸 |\n|---|---|\n| base | 22 |",
    });
    await flushPromises();

    expect(wrapper.find(".bubble table").exists()).toBe(true);
    expect(wrapper.text()).toContain("base");
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

  it("shows the modify target chip and clears it on request", async () => {
    const { wrapper } = mountPanel("blender", undefined, {
      selectedAsset: "Props/chair/chair.blend",
    });
    await flushPromises();

    const chip = wrapper.find(".asset-chip");
    expect(chip.exists()).toBe(true);
    expect(chip.text()).toContain("chair");
    await chip.find("button").trigger("click");

    expect(wrapper.emitted("clearAsset")).toHaveLength(1);
  });

  it("sends modify mode with the blend asset and clears the chip after acceptance", async () => {
    const { client, wrapper } = mountPanel(
      "blender",
      (fake) => {
        fake.results.set("chat.send", { accepted: true });
      },
      { selectedAsset: "Props/chair/chair.blend" },
    );
    await flushPromises();

    await wrapper.find("textarea").setValue("把椅子改短一点");
    await wrapper.find("button.primary").trigger("click");
    await flushPromises();

    expect(client.callsOf("chat.send")[0].params).toMatchObject({
      area: "blender",
      mode: "modify",
      asset: "Props/chair/chair.blend",
      text: "把椅子改短一点",
    });
    expect(wrapper.emitted("clearAsset")).toHaveLength(1); // 一次修改一条指令
  });

  it("alerts and clears the target when the blend source disappeared", async () => {
    const alert = vi.spyOn(window, "alert").mockImplementation(() => {});
    const { client, wrapper } = mountPanel(
      "blender",
      (fake) => {
        fake.results.set("chat.send", { accepted: false, error: "no-blend-source" });
      },
      { selectedAsset: "Props/chair/chair.blend" },
    );
    await flushPromises();

    await wrapper.find("textarea").setValue("改短一点");
    await wrapper.find("button.primary").trigger("click");
    await flushPromises();

    expect(alert).toHaveBeenCalled();
    expect(wrapper.emitted("clearAsset")).toHaveLength(1);
    expect(wrapper.find("button.stop").exists()).toBe(false); // 被拒绝，不进入进行中
    expect(client.callsOf("chat.send")).toHaveLength(1);
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

  it("offers to resume work after the step budget runs out", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();
    client.results.set("chat.continue", { ok: true, taskId: "resume-1" });

    client.emit("error.card", {
      area: "blender",
      reason: "max-steps-exceeded",
      resumable: true,
      steps: 100,
      maxSteps: 100,
    });
    await flushPromises();

    const card = wrapper.find(".error-card");
    expect(card.text()).toContain("步数");
    expect(card.text()).toContain("100");
    const resume = card.findAll("button").find((button) => button.text() === "继续工作");
    expect(resume).toBeTruthy();

    await resume!.trigger("click");
    await flushPromises();

    expect(client.callsOf("chat.continue")[0].params).toMatchObject({ area: "blender" });
    expect(wrapper.find(".error-card button").exists()).toBe(false); // 已接续，别重复点
    expect(wrapper.find(".bubble.pending").exists()).toBe(true);
  });

  it("opens the folder picker and submits the selected path", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();
    client.results.set("folderPick.submit", { ok: true, taskId: "build-1" });

    client.emit("folderPick.open", {
      area: "blender",
      requestId: "pick-1",
      rootName: "ModelLibrary",
      tree: {
        name: "ModelLibrary",
        path: "",
        children: [{ name: "Props", path: "Props", children: [] }],
      },
    });
    await flushPromises();
    expect(wrapper.find(".folder-picker").exists()).toBe(true);
    expect(wrapper.text()).toContain("Props");

    await wrapper.findAll(".folder-row")[1].trigger("click");
    const confirm = wrapper.findAll("button").find((button) => button.text() === "确认");
    await confirm!.trigger("click");
    await flushPromises();

    expect(client.callsOf("folderPick.submit")[0].params).toMatchObject({
      requestId: "pick-1",
      relativePath: "Props",
    });
    expect(wrapper.find(".folder-picker").exists()).toBe(false);
  });

  it("keeps the folder picker open when the backend rejects the path", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();
    client.results.set("folderPick.submit", { ok: false, error: "blender-busy" });

    client.emit("folderPick.open", {
      area: "blender",
      requestId: "pick-2",
      rootName: "ModelLibrary",
      tree: { name: "ModelLibrary", path: "", children: [] },
    });
    await flushPromises();

    const confirm = wrapper.findAll("button").find((button) => button.text() === "确认");
    await confirm!.trigger("click");
    await flushPromises();

    expect(wrapper.find(".folder-picker").exists()).toBe(true);
    expect(wrapper.find(".pick-error").text()).toContain("忙");
  });

  it("stops following new output once the reader scrolls up", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();
    const log = wrapper.find(".chat-log");
    const state = makeScrollable(log.element as HTMLElement, 1000, 400, 120);
    await log.trigger("scroll");
    expect(wrapper.find(".jump-bottom").exists()).toBe(true);

    client.emit("chat.delta", { area: "blender", text: "新的一段" });
    await flushPromises();
    expect(state.top).toBe(120); // 视图没被拽走
    expect(wrapper.find(".jump-bottom").exists()).toBe(true);
  });

  it("jumps to the bottom and resumes following when the arrow is clicked", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();
    const log = wrapper.find(".chat-log");
    const state = makeScrollable(log.element as HTMLElement, 1000, 400, 120);
    await log.trigger("scroll");

    await wrapper.find(".jump-bottom").trigger("click");
    await flushPromises();
    expect(state.top).toBe(1000);
    expect(wrapper.find(".jump-bottom").exists()).toBe(false);

    client.emit("chat.delta", { area: "blender", text: "后续" });
    await flushPromises();
    expect(state.top).toBe(1000); // 恢复跟随
  });

  it("pins to the bottom when the user sends a message", async () => {
    const { wrapper } = mountPanel("blender");
    await flushPromises();
    const log = wrapper.find(".chat-log");
    const state = makeScrollable(log.element as HTMLElement, 1000, 400, 120);
    await log.trigger("scroll");
    expect(wrapper.find(".jump-bottom").exists()).toBe(true);

    await wrapper.find("textarea").setValue("再来一版");
    await wrapper.find("button.primary").trigger("click");
    await flushPromises();
    expect(state.top).toBe(1000);
    expect(wrapper.find(".jump-bottom").exists()).toBe(false);
  });

  it("loads an earlier page when the reader reaches the top", async () => {
    const client = new FakeClient();
    let release!: (page: unknown) => void;
    const pending = new Promise<unknown>((resolve) => {
      release = resolve;
    });
    client.results.set("session.load", (params: { beforeId?: number }) =>
      params.beforeId
        ? pending
        : {
            messages: [{ id: 51, role: "assistant", content: "当前页" }],
            hasMore: true,
            nextCursor: 51,
          },
    );
    const wrapper = mount(ChatPanel, { props: { area: "blender", client: client.asClient() } });
    await flushPromises();

    const log = wrapper.find(".chat-log");
    const state = makeScrollable(log.element as HTMLElement, 1000, 400, 0);
    await log.trigger("scroll");
    expect(wrapper.find(".older-loading").exists()).toBe(true);

    const olderCalls = () =>
      client.calls.filter(
        (call) => call.method === "session.load" && Boolean(call.params.beforeId),
      );
    expect(olderCalls()).toHaveLength(1);
    expect(olderCalls()[0].params).toMatchObject({ area: "blender", limit: 30, beforeId: 51 });

    // 请求未返回时重复触顶，不能叠出第二次请求
    await log.trigger("scroll");
    expect(olderCalls()).toHaveLength(1);

    state.height = 1500; // 更早的消息插入后内容变高
    release({
      messages: [{ id: 1, role: "user", content: "更早的一条" }],
      hasMore: false,
      nextCursor: 1,
    });
    await flushPromises();

    expect(wrapper.find(".older-loading").exists()).toBe(false);
    expect(wrapper.text().indexOf("更早的一条")).toBeLessThan(wrapper.text().indexOf("当前页"));
    expect(state.top).toBe(500); // 补回高度差，视图不跳

    await log.trigger("scroll"); // 没有更早的了，不再请求
    expect(olderCalls()).toHaveLength(1);
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
