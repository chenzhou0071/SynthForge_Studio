// @vitest-environment jsdom
import { mount } from "@vue/test-utils";
import { describe, expect, it } from "vitest";
import PreviewPane from "./PreviewPane.vue";

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

  it("falls back to an info card for unsupported kinds", () => {
    const wrapper = mount(PreviewPane, {
      props: { area: "blender", file: { path: "chair.blend", kind: "info" } },
    });

    expect(wrapper.find(".info-card").text()).toContain("chair.blend");
    expect(wrapper.find(".info-card").text()).toContain("暂无可用预览");
  });
});
