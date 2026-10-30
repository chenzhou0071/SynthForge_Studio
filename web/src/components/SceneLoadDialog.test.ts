// @vitest-environment jsdom
import { describe, expect, it, vi } from "vitest";
import { flushPromises, mount } from "@vue/test-utils";
import SceneLoadDialog from "./SceneLoadDialog.vue";
import { FakeClient } from "../testing/fakeClient";

const payload = {
  area: "blender",
  requestId: "load-1",
  file: "desk",
  path: "d:/lib/Props/desk.blend",
};

describe("SceneLoadDialog", () => {
  it("keeps the dialog open with no timer until the user decides", async () => {
    const client = new FakeClient();
    const wrapper = mount(SceneLoadDialog, { props: { client: client.asClient(), load: payload } });
    await flushPromises();

    expect(wrapper.text()).toContain("desk.blend");
    expect(wrapper.text()).toContain("会替换 Blender 当前场景");
    expect(wrapper.text()).toContain("把当前场景保存好");
    // 无超时：推进两分钟虚拟时间，弹窗必须还在、还得能点
    vi.useFakeTimers();
    vi.advanceTimersByTime(120_000);
    vi.useRealTimers();
    await flushPromises();
    expect(wrapper.text()).toContain("会替换 Blender 当前场景");
    expect(wrapper.find(".primary").exists()).toBe(true);
  });

  it("sends accept on confirm and decline on cancel", async () => {
    const client = new FakeClient();
    const wrapper = mount(SceneLoadDialog, { props: { client: client.asClient(), load: payload } });
    await wrapper.find(".primary").trigger("click");
    await flushPromises();
    expect(client.callsOf("sceneLoad.accept").at(-1)!.params).toEqual({ requestId: "load-1" });

    await wrapper.setProps({ load: { ...payload, requestId: "load-2" } });
    await wrapper.find(".ghost").trigger("click");
    await flushPromises();
    expect(client.callsOf("sceneLoad.decline").at(-1)!.params).toEqual({ requestId: "load-2" });
  });
});
