// @vitest-environment jsdom
import { mount } from "@vue/test-utils";
import { describe, expect, it } from "vitest";
import ErrorCard from "./ErrorCard.vue";

// 后端 error.card 实际会发出的 reason 全集（对齐 CoreApp / AgentOrchestrator / DiscussionAgent / AssetImporter）。
// 新增 reason 不进字典，第一条用例会红——这就是它存在的意义：别再让英文错误码直接甩给用户。
const KNOWN_REASONS = [
  "blender-not-ready",
  "unity-not-ready",
  "agent-not-configured",
  "cloud3d-not-configured",
  "llm-request-failed",
  "cloud3d-failed",
  "bridge-crashed",
  "bridge-not-registered",
  "blender-busy",
  "blender-convert-timeout",
  "blender-convert-failed",
  "import-failed",
  "invalid-attachment",
  "invalid-project",
  "project-not-active",
  "empty-plan",
  "clear-scene-failed",
  "group-stalled",
  "max-steps-exceeded",
  "user-cancelled",
  "nothing-to-resume",
  "no-task-to-retry",
];

function mountCard(props: Record<string, unknown> = {}) {
  return mount(ErrorCard, { props: { reason: "blender-not-ready", ...props } });
}

function buttonByText(wrapper: ReturnType<typeof mountCard>, text: string) {
  return wrapper.findAll("button").find((b) => b.text() === text);
}

describe("ErrorCard", () => {
  it("renders Chinese copy for every known reason, never the raw code", () => {
    for (const reason of KNOWN_REASONS) {
      const wrapper = mountCard({ reason });
      const text = wrapper.find(".error-text").text();
      expect(text, reason).not.toBe("");
      expect(text, reason).toMatch(/[\u4e00-\u9fa5]/);
      expect(text, reason).not.toContain(reason); // 内部错误码不得出现在用户眼前
    }
  });

  it("falls back to the server message when the reason is unknown", () => {
    const wrapper = mountCard({ reason: "brand-new-code", message: "服务端原文" });
    expect(wrapper.find(".error-text").text()).toContain("服务端原文");
    expect(wrapper.find(".error-text").text()).not.toContain("brand-new-code");
  });

  it("still shows the raw reason if nothing better is available", () => {
    // 兜底也不能出现空白卡：宁可露出原文，也不能让用户面对一个空盒子
    const wrapper = mountCard({ reason: "brand-new-code" });
    expect(wrapper.find(".error-text").text()).toBe("brand-new-code");
  });

  it("keeps the detail line for reasons that carry a server message", () => {
    const wrapper = mountCard({ reason: "llm-request-failed", message: "HTTP 401 unauthorized" });
    expect(wrapper.find(".error-detail").text()).toContain("HTTP 401");
  });

  it("offers retry and bridge restart for a dead blender link", async () => {
    const wrapper = mountCard({ reason: "blender-not-ready" });
    await buttonByText(wrapper, "重试")!.trigger("click");
    expect(wrapper.emitted("retry")).toHaveLength(1);
    await buttonByText(wrapper, "重启桥接")!.trigger("click");
    expect(wrapper.emitted("restartBridge")).toHaveLength(1);
  });

  it("routes unconfigured agent to the agent page of settings", async () => {
    const wrapper = mountCard({ reason: "agent-not-configured" });
    await buttonByText(wrapper, "打开设置")!.trigger("click");
    expect(wrapper.emitted("openSettings")?.[0]).toEqual(["agent"]);
    expect(buttonByText(wrapper, "重试")).toBeUndefined(); // 没配好之前重试没意义
  });

  it("offers no action for cancelled or unusable project states", () => {
    for (const reason of ["user-cancelled", "invalid-project", "project-not-active"]) {
      const wrapper = mountCard({ reason });
      expect(wrapper.findAll("button"), reason).toHaveLength(0);
    }
  });

  it("shows the continue-work button only when the task is resumable", async () => {
    const wrapper = mountCard({ reason: "max-steps-exceeded", resumable: true });
    const cont = buttonByText(wrapper, "继续工作");
    expect(cont).toBeTruthy();
    await cont!.trigger("click");
    expect(wrapper.emitted("continueWork")).toHaveLength(1);

    const notResumable = mountCard({ reason: "max-steps-exceeded", resumable: false });
    expect(buttonByText(notResumable, "继续工作")).toBeUndefined();
  });

  it("hides every action once the card has been acted on", () => {
    // 旧写法是把 retryable 改 false；动作交给字典后必须有个显式的“已接续”标记，否则按钮会重复点
    const wrapper = mountCard({ reason: "cloud3d-failed", resolved: true });
    expect(wrapper.findAll("button")).toHaveLength(0);
    expect(wrapper.find(".error-text").text()).not.toBe(""); // 文案得留着，不能整卡消失
  });

  it("reports step usage in the budget text", () => {
    const wrapper = mountCard({ reason: "max-steps-exceeded", steps: 100, maxSteps: 180 });
    const text = wrapper.find(".error-text").text();
    expect(text).toContain("100");
    expect(text).toContain("180");
  });
});
