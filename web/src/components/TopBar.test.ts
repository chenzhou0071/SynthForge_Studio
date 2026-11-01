// @vitest-environment jsdom
import { mount } from "@vue/test-utils";
import { describe, expect, it } from "vitest";
import TopBar from "./TopBar.vue";

function mountBar(props: Record<string, unknown> = {}) {
  return mount(TopBar, { props: { area: "blender", blender: "off", unity: "off", ...props } });
}

describe("TopBar", () => {
  // 三种状态必须肉眼可辨：都灰就等于没有状态指示
  it("paints the lamp differently for off, reconnecting and ready", () => {
    const seen = new Set<string>();
    for (const state of ["off", "reconnecting", "ready"]) {
      const style = mountBar({ blender: state }).find(".lamp i").attributes("style") ?? "";
      expect(style, state).toContain("rgb(");
      seen.add(style);
    }
    expect(seen.size).toBe(3);
  });

  // 后端 hints 带「第 N 次重连」，比前端自造的「重连中」更能说明现在到底怎么了
  it("prefers the backend hint over the locally invented label", () => {
    const wrapper = mountBar({ blender: "reconnecting", hints: { blender: "Blender：第 3 次重连" } });
    expect(wrapper.find(".lamp").attributes("title")).toBe("Blender：第 3 次重连");
  });

  it("falls back to a readable label when the backend has no hint", () => {
    const wrapper = mountBar({ blender: "ready" });
    expect(wrapper.find(".lamp").attributes("title")).toBe("Blender：就绪");
  });

  it("opens the diagnostics drawer on demand", async () => {
    const wrapper = mountBar();
    const button = wrapper.findAll("button").find((item) => item.text() === "日志");
    expect(button).toBeTruthy();
    await button!.trigger("click");
    expect(wrapper.emitted("openLogs")).toHaveLength(1);
  });
});
