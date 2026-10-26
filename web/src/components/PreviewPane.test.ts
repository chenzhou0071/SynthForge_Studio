// @vitest-environment jsdom
import { mount } from "@vue/test-utils";
import { nextTick } from "vue";
import { beforeEach, describe, expect, it, vi } from "vitest";

// 只替掉需要真实 WebGL 上下文的渲染器；three 的数学类与 GLTFLoader 仍用真实现
const { sizeCalls } = vi.hoisted(() => ({ sizeCalls: [] as Array<[number, number]> }));
vi.mock("three", async (importOriginal) => {
  const actual = await importOriginal<typeof import("three")>();
  class FakeRenderer {
    domElement = document.createElement("canvas");
    setSize(width: number, height: number) {
      sizeCalls.push([width, height]);
    }
    setPixelRatio() {}
    render() {}
    dispose() {}
  }
  return { ...actual, WebGLRenderer: FakeRenderer };
});

import PreviewPane from "./PreviewPane.vue";

beforeEach(() => {
  sizeCalls.length = 0;
});

describe("PreviewPane", () => {
  it("shows an area-specific overview when nothing is selected", () => {
    const blender = mount(PreviewPane, { props: { area: "blender", file: null } });
    expect(blender.text()).toContain("资产库概览");
    expect(blender.text()).toContain("选中资产后在此预览");

    const unity = mount(PreviewPane, { props: { area: "unity", file: null } });
    expect(unity.text()).toContain("工程信息");
    expect(unity.text()).toContain("选中文件后在此预览");
  });

  it("highlights source code using the language of the file", () => {
    const wrapper = mount(PreviewPane, {
      props: {
        area: "unity",
        file: { path: "Assets/Player.cs", kind: "text", content: "class Player { }" },
      },
    });

    const html = wrapper.find("pre.code").html();
    expect(html).toContain("hljs-keyword");
    expect(wrapper.text()).toContain("class Player { }");
  });

  it("infers the language from the extension when none is provided", () => {
    const wrapper = mount(PreviewPane, {
      props: {
        area: "unity",
        file: { path: "config.json", kind: "text", content: '{"name": "forge"}' },
      },
    });

    expect(wrapper.find("pre.code").html()).toContain("hljs-attr");
  });

  it("does not blow up on files without a known highlighter", () => {
    const wrapper = mount(PreviewPane, {
      props: {
        area: "unity",
        file: { path: "notes.txt", kind: "text", content: "纯文本 <tag>" },
      },
    });

    expect(wrapper.find("pre.code").text()).toContain("纯文本 <tag>");
  });

  it("renders images with their data url", () => {
    const wrapper = mount(PreviewPane, {
      props: {
        area: "blender",
        file: { path: "preview.png", kind: "image", dataUrl: "data:image/png;base64,AAA" },
      },
    });

    expect(wrapper.find("img.preview-image").attributes("src")).toBe("data:image/png;base64,AAA");
  });

  it("shows the file info card for fbx and blend files", () => {
    const wrapper = mount(PreviewPane, {
      props: {
        area: "blender",
        file: {
          path: "Props/chair.blend",
          kind: "info",
          size: 2048,
          modified: "2026-10-25T10:00:00Z",
          blendPath: "Props/chair.blend",
        },
      },
    });

    const card = wrapper.find(".file-info");
    expect(card.text()).toContain("Blender 源文件");
    expect(card.text()).toContain("2 KB");
    expect(card.text()).toContain("是 · 已找到同名的 .blend 源文件");
    expect(card.text()).not.toContain("暂无可用预览");
  });

  it("explains that fbx files without a blend cannot be modified", () => {
    const wrapper = mount(PreviewPane, {
      props: {
        area: "blender",
        file: { path: "Props/table.fbx", kind: "info", size: 1024, blendPath: "" },
      },
    });

    expect(wrapper.find(".file-info").text()).toContain("否 · 没有 .blend 源文件");
    expect(wrapper.find(".file-info-hint").text()).toContain("只能重新生成");
  });

  it("offers the modify entry for Blender models", async () => {
    const wrapper = mount(PreviewPane, {
      props: { area: "blender", file: null },
    });
    await wrapper.setProps({
      file: { path: "Props/chair.glb", kind: "model", dataUrl: "file:///chair.glb" },
    });

    const button = wrapper.find(".modify-btn");
    expect(button.exists()).toBe(true);
    await button.trigger("click");
    expect(wrapper.emitted("modify")?.[0]).toEqual(["Props/chair.glb"]);
  });

  it("does not offer the modify entry outside the Blender library", () => {
    const wrapper = mount(PreviewPane, {
      props: {
        area: "unity",
        file: { path: "Assets/table.fbx", kind: "info", size: 10, blendPath: "Assets/table.blend" },
      },
    });

    expect(wrapper.find(".modify-btn").exists()).toBe(false);
  });

  it("tells the user to open Blender for full materials and effects", async () => {
    const wrapper = mount(PreviewPane, {
      props: { area: "blender", file: null },
    });
    await wrapper.setProps({
      file: { path: "Props/chair.glb", kind: "model", dataUrl: "file:///chair.glb" },
    });

    expect(wrapper.text()).toContain("完整材质、动画与特效请在 Blender 中查看");
  });

  it("closes the preview of the selected file", async () => {
    const wrapper = mount(PreviewPane, {
      props: { area: "blender", file: null },
    });
    expect(wrapper.find(".close-btn").exists()).toBe(false);

    await wrapper.setProps({
      file: { path: "Props/chair.glb", kind: "model", dataUrl: "file:///chair.glb" },
    });
    const close = wrapper.find(".close-btn");
    expect(close.exists()).toBe(true);
    await close.trigger("click");

    expect(wrapper.emitted("close")).toHaveLength(1);
  });

  it("shows an export notice instead of loading the model being written", async () => {
    const wrapper = mount(PreviewPane, {
      props: { area: "blender", exporting: true, file: null },
    });

    expect(wrapper.find(".export-banner").text()).toContain("正在导出");
    expect(wrapper.find(".export-banner").text()).toContain("完成后自动预览");
  });

  it("hides the export notice once exporting finishes", async () => {
    const wrapper = mount(PreviewPane, {
      props: { area: "blender", exporting: true, file: null },
    });
    expect(wrapper.find(".export-banner").exists()).toBe(true);

    await wrapper.setProps({ exporting: false });
    expect(wrapper.find(".export-banner").exists()).toBe(false);
  });

  it("sizes the 3D canvas only after the host becomes visible", async () => {
    const wrapper = mount(PreviewPane, {
      props: {
        area: "blender",
        file: { path: "shot.png", kind: "image", dataUrl: "data:image/png;base64,AAA" },
      },
    });
    // 模拟真实布局：画布容器隐藏时没有尺寸，切到模型后 DOM 更新完才拿到尺寸
    const host = wrapper.find(".canvas-host").element as HTMLElement;
    Object.defineProperty(host, "clientWidth", {
      get: () => (host.classList.contains("hidden") ? 0 : 800),
    });
    Object.defineProperty(host, "clientHeight", {
      get: () => (host.classList.contains("hidden") ? 0 : 400),
    });

    await wrapper.setProps({
      file: { path: "Props/chair.glb", kind: "model", dataUrl: "file:///chair.glb" },
    });
    await nextTick();

    expect(sizeCalls).toContainEqual([800, 400]);
    expect(sizeCalls).not.toContainEqual([0, 0]);
  });
});
