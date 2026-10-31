// @vitest-environment jsdom
import { flushPromises, mount } from "@vue/test-utils";
import { describe, expect, it } from "vitest";
import AssetPicker from "./AssetPicker.vue";
import { FakeClient } from "../testing/fakeClient";

function mountPicker(items: unknown[]) {
  const client = new FakeClient();
  client.results.set("assets.list", items);
  const wrapper = mount(AssetPicker, { props: { client: client.asClient() } });
  return { client, wrapper };
}

describe("AssetPicker", () => {
  it("lists only glb and fbx assets from the library", async () => {
    const { wrapper } = mountPicker([
      { relativePath: "props/chair.glb", name: "chair", kind: "glb" },
      { relativePath: "props/table.fbx", name: "table", kind: "fbx" },
      { relativePath: "props/chair.blend", name: "chair", kind: "blend" },
      { relativePath: "notes.json", name: "notes", kind: "json" },
    ]);
    await flushPromises();
    expect(wrapper.text()).toContain("props/chair.glb");
    expect(wrapper.text()).toContain("props/table.fbx");
    expect(wrapper.text()).not.toContain("chair.blend");
    expect(wrapper.text()).not.toContain("notes.json");
  });

  it("multi-selects and emits confirm with the chosen paths", async () => {
    const { wrapper } = mountPicker([
      { relativePath: "a.glb", name: "a", kind: "glb" },
      { relativePath: "b.fbx", name: "b", kind: "fbx" },
    ]);
    await flushPromises();
    const items = wrapper.findAll("li");
    await items[0].trigger("click");
    await items[1].trigger("click");
    const confirm = wrapper.findAll("button").find((b) => b.text() === "添加");
    expect(confirm).toBeTruthy();
    await confirm!.trigger("click");
    expect(wrapper.emitted("confirm")?.[0]).toEqual([["a.glb", "b.fbx"]]);
  });

  it("disables 添加 until something is selected", async () => {
    const { wrapper } = mountPicker([{ relativePath: "a.glb", name: "a", kind: "glb" }]);
    await flushPromises();
    const confirm = wrapper.findAll("button").find((b) => b.text() === "添加");
    expect((confirm!.element as HTMLButtonElement).disabled).toBe(true);
  });
});
