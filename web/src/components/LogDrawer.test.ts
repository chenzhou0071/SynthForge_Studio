// @vitest-environment jsdom
import { flushPromises, mount } from "@vue/test-utils";
import { describe, expect, it } from "vitest";
import LogDrawer from "./LogDrawer.vue";
import { FakeClient } from "../testing/fakeClient";

function mountDrawer(props: Record<string, unknown> = {}) {
  const client = new FakeClient();
  const wrapper = mount(LogDrawer, { props: { client: client.asClient(), open: true, ...props } });
  return { client, wrapper };
}

describe("LogDrawer", () => {
  it("stays closed until it is opened", () => {
    const { wrapper } = mountDrawer({ open: false });
    expect(wrapper.find(".log-drawer").exists()).toBe(false);
  });

  it("lists warning and error entries newest first with time and category", async () => {
    const { client, wrapper } = mountDrawer();
    client.emit("log.append", {
      level: "warning",
      category: "mcp",
      message: "桥接断了",
      time: "10:00:00",
    });
    client.emit("log.append", { level: "error", category: "app", message: "任务失败", time: "10:00:05" });
    await flushPromises();

    const lines = wrapper.findAll(".log-line");
    expect(lines).toHaveLength(2);
    expect(lines[0].text()).toContain("任务失败"); // 最新在前
    expect(lines[1].text()).toContain("桥接断了");
    expect(lines[1].text()).toContain("10:00:00");
    expect(lines[1].text()).toContain("mcp");
  });

  // 环形缓冲：日志风暴不能把页面吃光
  it("keeps at most 200 entries", async () => {
    const { client, wrapper } = mountDrawer();
    for (let i = 0; i < 260; ++i)
      client.emit("log.append", {
        level: "warning",
        category: "app",
        message: `#${i}`,
        time: "10:00:00",
      });
    await flushPromises();

    const lines = wrapper.findAll(".log-line");
    expect(lines).toHaveLength(200);
    expect(lines[0].text()).toContain("#259");
    expect(lines[199].text()).toContain("#60");
  });

  it("shows the three environment checks once they report in", async () => {
    const { client, wrapper } = mountDrawer();
    client.emit("env.status", {
      uvx: { state: "ready", detail: "uvx 0.7.2" },
      blender: { state: "ready", detail: "已就绪 · 9 个工具" },
      unity: { state: "failed", detail: "unity-not-ready" },
    });
    await flushPromises();

    const env = wrapper.find(".env").text();
    expect(env).toContain("uvx 0.7.2");
    expect(env).toContain("9 个工具");
    expect(env).toContain("未就绪");
    expect(env).not.toContain("unity-not-ready"); // 内部错误码不贴到面板上
  });

  it("prompts for a check while none has reported yet", () => {
    const { wrapper } = mountDrawer();
    expect(wrapper.find(".env").text()).toContain("未检查");
  });

  it("re-runs the checks when the user asks", async () => {
    const { client, wrapper } = mountDrawer();
    await wrapper.find("button.recheck").trigger("click");
    await flushPromises();
    expect(client.callsOf("env.check")).toHaveLength(1);
  });

  // 刚看过的告警不能因为关掉面板就消失：环形上限已经够挡住刷屏
  it("keeps the entries it already showed across close and reopen", async () => {
    const { client, wrapper } = mountDrawer();
    client.emit("log.append", { level: "warning", category: "app", message: "刚才那条", time: "10:00:00" });
    await flushPromises();
    expect(wrapper.findAll(".log-line")).toHaveLength(1);

    await wrapper.setProps({ open: false });
    await wrapper.setProps({ open: true });
    expect(wrapper.findAll(".log-line")).toHaveLength(1);
    expect(wrapper.find(".log-line").text()).toContain("刚才那条");
  });

  it("clears the list when the user asks", async () => {
    const { client, wrapper } = mountDrawer();
    client.emit("log.append", { level: "warning", category: "app", message: "看完了", time: "10:00:00" });
    await flushPromises();
    expect(wrapper.findAll(".log-line")).toHaveLength(1);

    await wrapper.find("button.clear").trigger("click");
    expect(wrapper.findAll(".log-line")).toHaveLength(0);

    // 清空不是关掉：以后新到的告警还得能看见
    client.emit("log.append", { level: "warning", category: "app", message: "清空后又有", time: "10:00:09" });
    await flushPromises();
    expect(wrapper.find(".log-line").text()).toContain("清空后又有");
  });

  it("still keeps the newest entries when the list is refilled while closed", async () => {
    const { client, wrapper } = mountDrawer();
    await wrapper.setProps({ open: false });
    for (let i = 0; i < 3; ++i)
      client.emit("log.append", { level: "warning", category: "app", message: `#${i}`, time: "10:00:00" });
    await wrapper.setProps({ open: true });
    await flushPromises();

    const lines = wrapper.findAll(".log-line");
    expect(lines).toHaveLength(3); // 抽屉关着时后端事件也不能丢
    expect(lines[0].text()).toContain("#2");
  });
});
