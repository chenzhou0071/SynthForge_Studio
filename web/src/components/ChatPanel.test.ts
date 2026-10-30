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

  it("renders markdown while streaming so ** does not leak into the bubble", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();

    client.emit("chat.delta", { area: "blender", text: "**粗体**" });
    await flushPromises();
    expect(wrapper.find(".bubble.streaming .md strong").text()).toBe("粗体");
    expect(wrapper.find(".bubble.streaming .md").text()).not.toContain("**");

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

  // 两个开关的偏好由后端 ui.ini 保存：Qt WebEngine 的 localStorage 在 app:// 下重启就丢
  it("restores the saved modes from the backend", async () => {
    const { wrapper } = mountPanel("blender", (fake) => {
      fake.results.set("uiState.get", { blenderMode: "lowpoly", chatMode: "ask" });
    });
    await flushPromises();

    const low = wrapper
      .findAll(".chat-tools .mode-switch button")
      .find((button) => button.text() === "低模");
    expect(low!.classes()).toContain("active"); // 上次退出时的选择

    const ask = wrapper
      .findAll(".chat-mode .mode-switch button")
      .find((button) => button.text() === "讨论");
    expect(ask!.classes()).toContain("active");
  });

  // 云API 只走执行：先出云提示词草稿，没有“讨论后直接云生成”这条链路
  it("disables discuss mode when cloud api is selected", async () => {
    const { wrapper } = mountPanel("blender");
    await flushPromises();

    const cloud = wrapper
      .findAll(".chat-tools .mode-switch button")
      .find((button) => button.text() === "云API");
    await cloud!.trigger("click");
    await flushPromises();

    const ask = wrapper
      .findAll(".chat-mode .mode-switch button")
      .find((button) => button.text() === "讨论");
    expect(ask!.attributes("disabled")).toBeDefined();
    expect(wrapper.find(".chat-mode-hint").text()).toContain("云API");
  });

  it("leaves discuss mode when the user switches to cloud api", async () => {
    const { wrapper } = mountPanel("blender");
    await flushPromises();

    const ask = wrapper
      .findAll(".chat-mode .mode-switch button")
      .find((button) => button.text() === "讨论");
    await ask!.trigger("click");
    await flushPromises();
    expect(ask!.classes()).toContain("active");

    const cloud = wrapper
      .findAll(".chat-tools .mode-switch button")
      .find((button) => button.text() === "云API");
    await cloud!.trigger("click");
    await flushPromises();

    const exec = wrapper
      .findAll(".chat-mode .mode-switch button")
      .find((button) => button.text() === "执行");
    expect(exec!.classes()).toContain("active");
    expect(ask!.attributes("disabled")).toBeDefined();
  });

  it("clamps a stored ask choice when cloud api is selected", async () => {
    const { wrapper } = mountPanel("blender", (fake) => {
      fake.results.set("uiState.get", { blenderMode: "cloud3d", chatMode: "ask" });
    });
    await flushPromises();

    // 旧版本可能存下这个组合，恢复时不能停在不可用的讨论态
    const exec = wrapper
      .findAll(".chat-mode .mode-switch button")
      .find((button) => button.text() === "执行");
    expect(exec!.classes()).toContain("active");
  });

  it("saves the cloud mode choice to the backend", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();
    client.calls.length = 0; // 只看用户这次改动

    const cloud = wrapper
      .findAll(".chat-tools .mode-switch button")
      .find((button) => button.text() === "云API");
    await cloud!.trigger("click");
    await flushPromises();

    expect(client.callsOf("uiState.set").at(-1)!.params).toMatchObject({ blenderMode: "cloud3d" });
  });

  it("saves the discuss mode choice to the backend", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();
    client.calls.length = 0;

    const ask = wrapper
      .findAll(".chat-mode .mode-switch button")
      .find((button) => button.text() === "讨论");
    await ask!.trigger("click");
    await flushPromises();

    expect(client.callsOf("uiState.set").at(-1)!.params).toMatchObject({ chatMode: "ask" });
  });

  it("shows cloud progress percent while generating", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();
    client.results.set("chat.send", { accepted: true });

    await wrapper.find("textarea").setValue("一棵树");
    await wrapper.find("button.primary").trigger("click");
    await flushPromises();

    client.emit("task.progress", { area: "blender", state: "queued", percent: 0 });
    await flushPromises();
    expect(wrapper.find(".cloud-progress").exists()).toBe(true);
    expect(wrapper.find("button.primary").text()).toContain("排队中");

    client.emit("task.progress", { area: "blender", state: "running", percent: 45 });
    await flushPromises();
    expect(wrapper.find(".cloud-progress .fill").attributes("style")).toContain("45%");
    expect(wrapper.find("button.primary").text()).toContain("生成中 45%");
  });

  it("clears cloud progress when the task finishes", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();
    client.results.set("chat.send", { accepted: true });

    await wrapper.find("textarea").setValue("一棵树");
    await wrapper.find("button.primary").trigger("click");
    await flushPromises();

    client.emit("task.progress", { area: "blender", state: "running", percent: 60 });
    await flushPromises();
    expect(wrapper.find(".cloud-progress").exists()).toBe(true);

    client.emit("task.progress", { area: "blender", state: "success", percent: 100 });
    await flushPromises();
    expect(wrapper.find(".cloud-progress").exists()).toBe(false);
    expect(wrapper.find("button.primary").text()).toContain("发送");
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

  it("shows an asset card when generation finishes and opens it for preview", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();

    const card = {
      name: "chair_01",
      source: "lowpoly",
      prompt: "一把木质餐椅…",
      files: [{ suffix: "glb", path: "props/chair_01.glb" }],
      dir: "props",
      createdAt: "2026-10-29 14:00",
    };
    client.emit("asset.created", {
      area: "blender",
      id: 9,
      path: "props/chair_01.glb",
      card,
    });
    await flushPromises();

    const asset = wrapper.find(".asset-card");
    expect(asset.exists()).toBe(true);
    expect(asset.text()).toContain("chair_01");
    expect(asset.text()).toContain("一把木质餐椅…");

    await asset.find("button.view").trigger("click");
    expect(wrapper.emitted("preview")?.[0]).toEqual(["props/chair_01.glb"]);
  });

  it("offers a settings shortcut when the agent is not configured", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();

    client.emit("error.card", { area: "blender", reason: "agent-not-configured" });
    await flushPromises();

    const button = wrapper.findAll(".error-card button").find((item) => item.text() === "去设置");
    expect(button).toBeTruthy();
    await button!.trigger("click");
    expect(wrapper.emitted("openSettings")?.[0]).toEqual(["agent"]);
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

  it("offers retry on cloud failure and continues through chat.retry", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();
    client.results.set("chat.send", { accepted: true });
    client.results.set("chat.retry", { accepted: true });

    await wrapper.find("textarea").setValue("一棵树");
    await wrapper.find("button.primary").trigger("click");
    await flushPromises();

    client.emit("error.card", {
      area: "blender",
      reason: "cloud3d-failed",
      message: "网络连接被拒绝",
    });
    await flushPromises();

    const card = wrapper.find(".error-card");
    expect(card.text()).toContain("网络连接被拒绝"); // 透传服务端原因，不只显示错误码
    const retry = card.findAll("button").find((button) => button.text() === "重试");
    expect(retry).toBeTruthy();

    await retry!.trigger("click");
    await flushPromises();

    expect(client.callsOf("chat.retry")[0].params).toMatchObject({ area: "blender" });
    expect(wrapper.find(".error-card button").exists()).toBe(false); // 已接续，别重复点
    expect(wrapper.find("button.stop").exists()).toBe(true); // 续轮询期间保持进行中
  });

  it("opens the cloud settings instead of retrying when cloud is not configured", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();

    client.emit("error.card", { area: "blender", reason: "cloud3d-not-configured" });
    await flushPromises();

    expect(wrapper.find(".error-card").text()).toContain("未配置云 API");
    const button = wrapper.findAll(".error-card button").find((item) => item.text() === "去设置");
    expect(button).toBeTruthy();
    expect(wrapper.findAll(".error-card button").find((item) => item.text() === "重试")).toBeUndefined();
    await button!.trigger("click");
    expect(wrapper.emitted("openSettings")?.[0]).toEqual(["cloud"]);
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

  it("only closes the folder picker on explicit cancel", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();

    client.emit("folderPick.open", {
      area: "blender",
      requestId: "pick-mask",
      rootName: "ModelLibrary",
      tree: { name: "ModelLibrary", path: "", children: [] },
    });
    await flushPromises();

    // 点窗口外再点回时，激活窗口的那一下会落在全屏遮罩上：不能当取消
    await wrapper.find(".dialog-mask").trigger("click");
    await flushPromises();
    expect(wrapper.find(".folder-picker").exists()).toBe(true);
    expect(client.callsOf("folderPick.cancel")).toHaveLength(0);

    // 显式 Esc 才关闭，并真正取消后端待办
    window.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape" }));
    await flushPromises();
    expect(wrapper.find(".folder-picker").exists()).toBe(false);
    expect(client.callsOf("folderPick.cancel")).toHaveLength(1);
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

  it("restores a started plan card without action buttons", async () => {
    const plan = {
      taskId: "task-plan",
      rules: ["留出通道"],
      groups: [{ title: "底座", items: ["base"], placements: [], acceptance: "宽度 22" }],
      totalSteps: 4,
    };
    const { wrapper } = mountPanel("blender", (fake) => {
      fake.results.set("session.load", {
        messages: [
          { id: 7, role: "plan", content: JSON.stringify(plan), meta: { state: "started" } },
        ],
      });
    });
    await flushPromises();

    expect(wrapper.find(".plan-card").exists()).toBe(true);
    expect(wrapper.find(".plan-card").text()).toContain("底座");
    expect(wrapper.findAll("button").some((button) => button.text() === "开始生成")).toBe(false);
    expect(wrapper.find(".plan-state").text()).toContain("已确认");
  });

  it("confirms a restored pending plan with its row id", async () => {
    const plan = {
      taskId: "task-plan",
      rules: ["留出通道"],
      groups: [{ title: "底座", items: ["base"], placements: [], acceptance: "宽度 22" }],
      totalSteps: 4,
    };
    const { client, wrapper } = mountPanel("blender", (fake) => {
      fake.results.set("session.load", {
        messages: [
          { id: 9, role: "plan", content: JSON.stringify(plan), meta: { state: "pending" } },
        ],
      });
      fake.results.set("plan.confirm", { ok: true });
    });
    await flushPromises();

    const start = wrapper.findAll("button").find((button) => button.text() === "开始生成");
    expect(start).toBeTruthy();
    await start!.trigger("click");
    await flushPromises();

    expect(client.callsOf("plan.confirm")[0].params).toMatchObject({
      area: "blender",
      taskId: "task-plan",
      id: 9,
    });
  });

  it("sends ask mode from the discussion switch and shows the draft card", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();

    await wrapper.findAll(".chat-mode button")[0].trigger("click");
    await wrapper.find("textarea").setValue("这个招牌怎么改才好看");
    await wrapper.find("button.primary").trigger("click");
    await flushPromises();

    expect(client.callsOf("chat.send")[0].params).toMatchObject({
      area: "blender",
      mode: "ask",
      text: "这个招牌怎么改才好看",
    });

    client.emit("draft.created", {
      area: "blender",
      id: 21,
      prompt: "把招牌改旧一些",
      origin: "discussion",
    });
    await flushPromises();

    const card = wrapper.find(".draft-card");
    expect(card.text()).toContain("讨论整理出的草稿");
    expect(card.find("pre").text()).toContain("把招牌改旧一些");
    expect(card.findAll("button").map((button) => button.text())).toEqual([
      "执行",
      "重新生成",
      "取消",
    ]);
  });

  it("saves an edited draft when the textarea loses focus", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();
    client.emit("draft.created", { area: "blender", id: 22, prompt: "初稿" });
    await flushPromises();

    await wrapper.find(".draft-card pre").trigger("dblclick");
    const editor = wrapper.find(".draft-card textarea");
    await editor.setValue("改过的方案");
    await editor.trigger("blur");
    await flushPromises();

    expect(client.callsOf("draft.save")[0].params).toMatchObject({
      id: 22,
      content: "改过的方案",
    });
  });

  it("regenerates and cancels a draft by id", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();
    client.emit("draft.created", { area: "blender", id: 23, prompt: "初稿" });
    await flushPromises();

    const regenerate = wrapper.findAll("button").find((button) => button.text() === "重新生成");
    await regenerate!.trigger("click");
    await flushPromises();
    expect(client.callsOf("draft.regenerate")[0].params).toMatchObject({ id: 23 });

    client.emit("draft.updated", { area: "blender", id: 23, prompt: "重写后的方案" });
    await flushPromises();
    expect(wrapper.find(".draft-card pre").text()).toContain("重写后的方案");

    const cancel = wrapper.findAll("button").find((button) => button.text() === "取消");
    await cancel!.trigger("click");
    await flushPromises();
    expect(client.callsOf("draft.cancel")[0].params).toMatchObject({ id: 23 });

    client.emit("draft.done", { area: "blender", id: 23, cancelled: true });
    await flushPromises();
    expect(wrapper.find(".draft-card").text()).toContain("已取消");
    expect(wrapper.find(".draft-card button").exists()).toBe(false); // 灰卡只读
  });

  it("executes a draft by id and removes the card once it becomes a user message", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();
    client.results.set("draft.send", { accepted: true });
    client.emit("draft.created", { area: "blender", id: 24, prompt: "方案", origin: "discussion" });
    await flushPromises();

    const run = wrapper.findAll("button").find((button) => button.text() === "执行");
    await run!.trigger("click");
    await flushPromises();
    expect(client.callsOf("draft.send")[0].params).toMatchObject({ id: 24, prompt: "方案" });
    expect(wrapper.find("button.stop").exists()).toBe(true); // 已进入规划，可停止

    client.emit("draft.done", { area: "blender", id: 24 });
    client.emit("chat.message", { area: "blender", role: "user", content: "方案" });
    await flushPromises();

    expect(wrapper.find(".draft-card").exists()).toBe(false);
    expect(wrapper.text().match(/方案/g)).toHaveLength(1); // 转正后历史不重复
  });

  it("keeps the draft card with an error when the target has no blend source", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();
    client.results.set("draft.send", { accepted: false, error: "no-blend-source" });
    client.emit("draft.created", { area: "blender", id: 25, prompt: "改旧一点" });
    await flushPromises();

    const run = wrapper.findAll("button").find((button) => button.text() === "执行");
    await run!.trigger("click");
    await flushPromises();

    expect(wrapper.find(".draft-card").exists()).toBe(true);
    expect(wrapper.find(".draft-error").text()).toContain("源文件");
    expect(client.callsOf("draft.send")).toHaveLength(1);
  });

  it("restores a cancelled draft as a grey read-only card", async () => {
    const { wrapper } = mountPanel("blender", (fake) => {
      fake.results.set("session.load", {
        messages: [
          {
            id: 26,
            role: "draft",
            content: "旧方案",
            meta: { state: "cancelled", origin: "generated" },
          },
        ],
      });
    });
    await flushPromises();

    expect(wrapper.find(".draft-card").exists()).toBe(true);
    expect(wrapper.find(".draft-card").text()).toContain("旧方案");
    expect(wrapper.find(".draft-card").text()).toContain("已取消");
    expect(wrapper.find(".draft-card button").exists()).toBe(false);
  });

  it("switches to exec mode after executing a discussion draft", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();
    client.results.set("draft.send", { accepted: true });
    await wrapper.findAll(".chat-mode button")[0].trigger("click"); // 讨论
    client.emit("draft.created", { area: "blender", id: 27, prompt: "方案" });
    await flushPromises();

    await wrapper.findAll(".draft-card button")[0].trigger("click");
    await flushPromises();

    const active = wrapper.findAll(".chat-mode button").find((button) => button.classes("active"));
    expect(active?.text()).toBe("执行");
  });

  it("shows planning dots after executing a draft even if a stream was left open", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();
    client.results.set("draft.send", { accepted: true });
    client.emit("chat.delta", { area: "blender", text: "半截回答" });
    await flushPromises();
    client.emit("draft.created", { area: "blender", id: 28, prompt: "方案" });
    await flushPromises();

    await wrapper.findAll(".draft-card button")[0].trigger("click");
    await flushPromises();

    expect(wrapper.find(".bubble.streaming").exists()).toBe(false);
    expect(wrapper.find(".bubble.pending").exists()).toBe(true);
  });

  it("keeps the planning dots when draft.done arrives before the send response", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();
    let resolveSend!: (value: unknown) => void;
    client.results.set(
      "draft.send",
      () => new Promise((resolve) => {
        resolveSend = resolve;
      }),
    );
    client.emit("draft.created", { area: "blender", id: 31, prompt: "方案" });
    await flushPromises();

    await wrapper.findAll(".draft-card button")[0].trigger("click");

    // 真实后端在请求返回前就广播了转正事件：卡片会先消失
    client.emit("draft.done", { area: "blender", id: 31 });
    client.emit("chat.message", { area: "blender", role: "user", content: "方案" });
    await flushPromises();

    expect(wrapper.find(".bubble.streaming").exists()).toBe(false);
    expect(wrapper.find(".bubble.pending").exists()).toBe(true);

    resolveSend({ accepted: true });
    await flushPromises();
    expect(wrapper.find(".bubble.pending").exists()).toBe(true);
  });

  it("disables draft actions and shows a spinner while regenerating", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();
    client.results.set("draft.regenerate", () => new Promise(() => {}));
    client.emit("draft.created", { area: "blender", id: 29, prompt: "初稿" });
    await flushPromises();

    await wrapper.findAll(".draft-card button")[1].trigger("click");
    await flushPromises();
    expect(wrapper.find(".draft-card .spinner").exists()).toBe(true);
    const buttons = wrapper.findAll(".draft-card button");
    expect(buttons.every((button) => (button.element as HTMLButtonElement).disabled)).toBe(true);

    // 重复点击不能叠第二次重新生成请求
    await buttons[1].trigger("click");
    expect(client.callsOf("draft.regenerate")).toHaveLength(1);

    client.emit("draft.updated", { area: "blender", id: 29, prompt: "新版" });
    await flushPromises();
    expect(wrapper.find(".draft-card .spinner").exists()).toBe(false);
    expect(wrapper.find(".draft-card pre").text()).toContain("新版");
  });

  it("clears the regenerate spinner when draft.failed arrives", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();
    client.results.set("draft.regenerate", { accepted: true });
    client.emit("draft.created", { area: "blender", id: 30, prompt: "初稿" });
    await flushPromises();

    await wrapper.findAll(".draft-card button")[1].trigger("click");
    await flushPromises();
    expect(wrapper.find(".draft-card .spinner").exists()).toBe(true);

    client.emit("draft.failed", { area: "blender", id: 30, reason: "http-500" });
    await flushPromises();
    expect(wrapper.find(".draft-card .spinner").exists()).toBe(false);
    expect(wrapper.find(".draft-error").text()).toContain("重新生成失败");
    expect((wrapper.findAll(".draft-card button")[1].element as HTMLButtonElement).disabled).toBe(
      false,
    );
  });

  // 三档产出形态：默认盒式舞台不破旧行为，选了要写回 ui.ini，云API 下整块隐藏
  it("persists the output shape switch to ui.ini and hides it for cloud api", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();

    const shapeButtons = wrapper.findAll(".shape-switch button");
    expect(shapeButtons.map((button) => button.text())).toEqual(["道具", "盒式舞台", "开放场景"]);
    expect(shapeButtons[1]!.classes()).toContain("active"); // 默认盒式舞台

    await shapeButtons[0]!.trigger("click"); // 道具
    await flushPromises();
    const saved = client.callsOf("uiState.set").at(-1)!.params as Record<string, unknown>;
    expect(saved.shape).toBe("prop");

    const cloud = wrapper
      .findAll(".chat-tools .mode-switch button")
      .find((button) => button.text() === "云API")!;
    await cloud.trigger("click");
    await flushPromises();
    // 云端只产单体，三档开关这条链路不存在
    expect(wrapper.find(".shape-switch").exists()).toBe(false);
  });

  it("sends the current shape with the lowpoly request", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();
    client.results.set("chat.send", { accepted: true });

    await wrapper.find("textarea").setValue("一张书桌，上面有电脑和杯子");
    await wrapper.find("button.primary").trigger("click");
    await flushPromises();

    const sent = client.callsOf("chat.send").at(-1)!.params as Record<string, unknown>;
    // 后端 ui.ini 是唯一真相，但请求自带档位可躲开读写竞态
    expect(sent.shape).toBe("diorama");
  });

  // 选中一个带档位的资产后，三档开关自动跳到该档并锁死：不能用开放场景提示词改一把道具椅子
  it("locks the shape switch to the selected asset's own mode", async () => {
    const { wrapper } = mountPanel("blender", undefined, {
      selectedAsset: "Props/desk/desk.blend",
      selectedAssetShape: "prop",
    });
    await flushPromises();

    const buttons = wrapper.findAll(".shape-switch button");
    expect(buttons).toHaveLength(3);
    expect(buttons[0]!.classes()).toContain("active"); // 自动落到道具
    // 锁死：三个按钮全禁用，点开放场景也不生效
    expect(buttons.every((b) => (b.element as HTMLButtonElement).disabled)).toBe(true);
    await buttons[2]!.trigger("click");
    await flushPromises();
    expect(buttons[0]!.classes()).toContain("active");
    expect(buttons[2]!.classes()).not.toContain("active");
  });

  // 讨论一个已选中的资产前，必须先无超时确认载入 .blend，本轮不发消息
  it("requests a scene load before discussing a selected asset, and defers the send", async () => {
    const { client, wrapper } = mountPanel("blender", undefined, {
      selectedAsset: "Props/desk/desk.blend",
    });
    await flushPromises();
    client.results.set("chat.send", { accepted: true });

    await wrapper.findAll(".chat-mode button")[0]!.trigger("click"); // 讨论
    await wrapper.find("textarea").setValue("这个桌面材质怎么调更旧");
    await wrapper.find("button.primary").trigger("click");
    await flushPromises();

    // 先发起载入确认，且这一轮没有把讨论消息发出去
    const req = client.callsOf("sceneLoad.request").at(-1)?.params as Record<string, unknown>;
    expect(req.asset).toBe("Props/desk/desk.blend");
    expect(client.callsOf("chat.send")).toHaveLength(0);
    // 用户刚写的这轮问题不能被吞掉
    expect((wrapper.find("textarea").element as HTMLTextAreaElement).value).toBe(
      "这个桌面材质怎么调更旧",
    );
  });

  it("shows the load confirmation dialog when the backend broadcasts sceneLoad.confirm", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();

    client.emit("sceneLoad.confirm", {
      area: "blender",
      requestId: "load-9",
      file: "desk",
      path: "d:/lib/Props/desk.blend",
    });
    await flushPromises();

    expect(wrapper.find(".dialog").exists()).toBe(true);
    expect(wrapper.text()).toContain("会替换 Blender 当前场景");
  });

  // 计划卡要把后端 plan.created 带来的档位、分区、动线显示出来
  it("shows the output shape, zones and motions on the plan card", async () => {
    const { client, wrapper } = mountPanel("blender");
    await flushPromises();

    client.emit("plan.created", {
      area: "blender",
      id: 5,
      taskId: "t-open",
      shape: "open",
      rules: ["留出通道"],
      zones: ["主轴：轨道沿 +Y 至 120 米"],
      motions: ["列车由南向北进出站"],
      groups: [{ title: "路基", items: [], placements: [], acceptance: "达到 120 米" }],
      totalSteps: 8,
    });
    await flushPromises();

    const card = wrapper.find(".plan-card");
    expect(card.exists()).toBe(true);
    expect(card.find(".shape-tag").text()).toBe("开放场景");
    expect(card.text()).toContain("主轴：轨道沿 +Y 至 120 米");
    expect(card.text()).toContain("列车由南向北进出站");
  });
});
