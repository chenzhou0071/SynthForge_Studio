// @vitest-environment jsdom
import { mount } from "@vue/test-utils";
import { describe, expect, it } from "vitest";
import AssetCard from "./AssetCard.vue";

const lowpolyAsset = {
  name: "chair_01",
  source: "lowpoly" as const,
  prompt: "一把木质餐椅，四条腿略微外八…",
  files: [
    { suffix: "glb", path: "props/chair_01.glb" },
    { suffix: "fbx", path: "props/chair_01.fbx" },
    { suffix: "blend", path: "props/chair_01.blend" },
  ],
  dir: "props",
  createdAt: "2026-10-29 14:00",
};

describe("AssetCard", () => {
  it("shows parameters and the file list without face count", () => {
    const wrapper = mount(AssetCard, { props: { asset: lowpolyAsset } });

    expect(wrapper.text()).toContain("chair_01");
    expect(wrapper.text()).toContain("低模建模");
    expect(wrapper.text()).toContain(lowpolyAsset.prompt);
    expect(wrapper.text()).toContain("props");
    expect(wrapper.text()).toContain("2026-10-29 14:00");
    for (const suffix of ["GLB", "FBX", "BLEND"]) expect(wrapper.text()).toContain(suffix);
    expect(wrapper.text()).not.toContain("面数");
  });

  it("shows the cloud provider and model", () => {
    const wrapper = mount(AssetCard, {
      props: {
        asset: {
          ...lowpolyAsset,
          source: "cloud3d" as const,
          provider: "tripo",
          model: "v3.1-20260211",
        },
      },
    });

    expect(wrapper.text()).toContain("云API");
    expect(wrapper.text()).toContain("tripo");
    expect(wrapper.text()).toContain("v3.1-20260211");
  });

  it("emits the glb path when view is clicked", async () => {
    const wrapper = mount(AssetCard, { props: { asset: lowpolyAsset } });

    await wrapper.find("button.view").trigger("click");

    expect(wrapper.emitted("preview")?.[0]).toEqual(["props/chair_01.glb"]);
  });

  it("disables view when the card has no glb", () => {
    const wrapper = mount(AssetCard, {
      props: {
        asset: { ...lowpolyAsset, files: [{ suffix: "blend", path: "props/chair_01.blend" }] },
      },
    });

    expect(wrapper.find("button.view").attributes("disabled")).toBeDefined();
  });
});
