// @vitest-environment jsdom
import { flushPromises, mount } from "@vue/test-utils";
import { afterEach, describe, expect, it, vi } from "vitest";
import FileTree from "./FileTree.vue";
import { FakeClient } from "../testing/fakeClient";

function mountTree(area: "blender" | "unity" = "blender", setup?: (client: FakeClient) => void) {
  const client = new FakeClient();
  setup?.(client);
  const wrapper = mount(FileTree, { props: { area, client: client.asClient() } });
  return { client, wrapper };
}

async function openRootMenu(wrapper: ReturnType<typeof mountTree>["wrapper"]) {
  await wrapper.find(".tree-body").trigger("contextmenu", { clientX: 40, clientY: 60 });
}

function menuItems(wrapper: ReturnType<typeof mountTree>["wrapper"]) {
  return wrapper.findAll(".ctx-menu li");
}

afterEach(() => {
  vi.restoreAllMocks();
});

describe("FileTree", () => {
  it("lists the library root directly without a ModelLibrary row", () => {
    const { wrapper } = mountTree();
    expect(wrapper.text()).not.toContain("ModelLibrary");
    expect(wrapper.find(".tree-empty").text()).toContain("暂无资产，右键可新建文件夹");
  });

  it("opens the five-item context menu on the tree body", async () => {
    const { wrapper } = mountTree();
    expect(wrapper.find(".ctx-menu").exists()).toBe(false);

    await openRootMenu(wrapper);
    expect(menuItems(wrapper).map((item) => item.text())).toEqual([
      "新建文件夹",
      "重命名",
      "删除",
      "在资源管理器中显示",
      "刷新",
    ]);
  });

  it("greys out rename and delete when the target is not a file or folder", async () => {
    const { client, wrapper } = mountTree();
    const prompt = vi.spyOn(window, "prompt");
    const confirm = vi.spyOn(window, "confirm");

    await openRootMenu(wrapper);
    const items = menuItems(wrapper);
    expect(items[1].classes()).toContain("disabled");
    expect(items[2].classes()).toContain("disabled");

    await items[1].trigger("click");
    await items[2].trigger("click");
    await flushPromises();

    expect(prompt).not.toHaveBeenCalled();
    expect(confirm).not.toHaveBeenCalled();
    expect(client.callsOf("fs.operate")).toHaveLength(0);
  });

  it("creates a folder under the library root", async () => {
    const { client, wrapper } = mountTree();
    vi.spyOn(window, "prompt").mockReturnValue("Props");

    await openRootMenu(wrapper);
    await menuItems(wrapper)[0].trigger("click");
    await flushPromises();

    expect(client.callsOf("fs.operate")[0].params).toEqual({
      area: "blender",
      op: "mkdir",
      path: "",
      name: "Props",
    });
    expect(wrapper.find(".ctx-menu").exists()).toBe(false);
  });

  it("does nothing when the folder name prompt is cancelled", async () => {
    const { client, wrapper } = mountTree();
    vi.spyOn(window, "prompt").mockReturnValue(null as unknown as string);

    await openRootMenu(wrapper);
    await menuItems(wrapper)[0].trigger("click");
    await flushPromises();

    expect(client.callsOf("fs.operate")).toHaveLength(0);
  });

  it("reveals the library root without a name field", async () => {
    const { client, wrapper } = mountTree();
    const prompt = vi.spyOn(window, "prompt");

    await openRootMenu(wrapper);
    await menuItems(wrapper)[3].trigger("click");
    await flushPromises();

    expect(prompt).not.toHaveBeenCalled();
    expect(client.callsOf("fs.operate")[0].params).toEqual({
      area: "blender",
      op: "reveal",
      path: "",
    });
  });

  it("shows a notice instead of a raw error while fs.operate is not implemented", async () => {
    const { wrapper } = mountTree("blender", (fake) => {
      fake.failures.set("fs.operate", "unknown method: fs.operate");
    });

    await openRootMenu(wrapper);
    await menuItems(wrapper)[4].trigger("click");
    await flushPromises();

    expect(wrapper.find(".tree-note").text()).toContain("文件操作尚未接入");
    expect(wrapper.find(".tree-error").exists()).toBe(false);
  });

  it("shows the backend error for real failures", async () => {
    const { wrapper } = mountTree("blender", (fake) => {
      fake.failures.set("fs.operate", "invalid-path");
    });

    await openRootMenu(wrapper);
    await menuItems(wrapper)[4].trigger("click");
    await flushPromises();

    expect(wrapper.find(".tree-error").text()).toContain("invalid-path");
  });

  it("closes the menu on Escape and on an outside click", async () => {
    const { wrapper } = mountTree();

    await openRootMenu(wrapper);
    expect(wrapper.find(".ctx-menu").exists()).toBe(true);
    window.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape" }));
    await flushPromises();
    expect(wrapper.find(".ctx-menu").exists()).toBe(false);

    await openRootMenu(wrapper);
    document.body.dispatchEvent(new MouseEvent("mousedown", { bubbles: true }));
    await flushPromises();
    expect(wrapper.find(".ctx-menu").exists()).toBe(false);
  });

  it("never opens the custom menu for the Unity area", async () => {
    const { wrapper } = mountTree("unity");
    expect(wrapper.text()).toContain("尚未导入工程");

    await wrapper.find(".tree-body").trigger("contextmenu", { clientX: 10, clientY: 10 });
    expect(wrapper.find(".ctx-menu").exists()).toBe(false);
  });
});
