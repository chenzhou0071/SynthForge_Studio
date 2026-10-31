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

  it("renders folders and model files from assets.list", async () => {
    const { client, wrapper } = mountTree("blender", (fake) => {
      fake.results.set("assets.list", [
        { name: "chair", relativePath: "Props/chair.glb", kind: "glb" },
        { name: "table", relativePath: "Props/table.fbx", kind: "fbx" },
        { name: "lamp", relativePath: "Street/lamp.glb", kind: "glb" },
        { name: "root", relativePath: "root.blend", kind: "blend" },
      ]);
    });
    await flushPromises();

    expect(client.callsOf("assets.list")).toHaveLength(1);
    expect(wrapper.findAll(".tree-dir .tree-name").map((row) => row.text())).toEqual([
      "Props",
      "Street",
    ]);
    expect(wrapper.findAll(".tree-file .tree-name").map((row) => row.text())).toEqual([
      "chair.glb",
      "table.fbx",
      "lamp.glb",
      "root.blend",
    ]);
    expect(wrapper.findAll(".tree-file .suffix").map((tag) => tag.text())).toEqual([
      "glb",
      "fbx",
      "glb",
      "blend",
    ]);
  });

  it("marks cloud-generated assets with a blue 云 · glb tag", async () => {
    const { wrapper } = mountTree("blender", (fake) => {
      fake.results.set("assets.list", [
        {
          name: "crate",
          relativePath: "Props/crate.glb",
          kind: "glb",
          source: "cloud",
          provider: "tripo",
        },
      ]);
    });
    await flushPromises();

    const tag = wrapper.find(".tree-file .suffix");
    expect(tag.text()).toBe("云 · glb");
    expect(tag.classes()).toContain("cloud");
    expect(tag.attributes("title")).toBe("云生成 · Tripo");
  });

  it("keeps the plain glb tag for locally exported assets", async () => {
    const { wrapper } = mountTree("blender", (fake) => {
      fake.results.set("assets.list", [
        { name: "chair", relativePath: "Props/chair.glb", kind: "glb" },
      ]);
    });
    await flushPromises();

    const tag = wrapper.find(".tree-file .suffix");
    expect(tag.text()).toBe("glb");
    expect(tag.classes()).not.toContain("cloud");
    expect(tag.attributes("title")).toBeUndefined();
  });

  it("shows empty folders even when they contain no model files", async () => {
    const { wrapper } = mountTree("blender", (fake) => {
      fake.results.set("assets.list", [
        { name: "test1", relativePath: "test1", kind: "dir" },
        { name: "Inner", relativePath: "test1/Inner", kind: "dir" },
      ]);
    });
    await flushPromises();

    expect(wrapper.findAll(".tree-dir .tree-name").map((row) => row.text())).toEqual([
      "test1",
      "Inner",
    ]);
    expect(wrapper.findAll(".tree-file")).toHaveLength(0);
    expect(wrapper.find(".tree-empty").exists()).toBe(false);
  });

  it("refetches the tree when fileTree.changed arrives", async () => {
    const { client } = mountTree("blender", (fake) => fake.results.set("assets.list", []));
    await flushPromises();

    client.emit("fileTree.changed", { area: "blender", reason: "incremental", paths: [""] });
    await flushPromises();
    expect(client.callsOf("assets.list")).toHaveLength(2);
  });

  it("renames the folder that was right-clicked", async () => {
    const { client, wrapper } = mountTree("blender", (fake) => {
      fake.results.set("assets.list", [
        { name: "chair", relativePath: "Props/chair.glb", kind: "glb" },
      ]);
    });
    await flushPromises();
    vi.spyOn(window, "prompt").mockReturnValue("Models");

    await wrapper.findAll(".tree-dir")[0].trigger("contextmenu", { clientX: 20, clientY: 30 });
    await menuItems(wrapper)[1].trigger("click");
    await flushPromises();

    expect(client.callsOf("fs.operate")[0].params).toEqual({
      area: "blender",
      op: "rename",
      path: "Props",
      name: "Models",
    });
  });

  it("refreshes after a folder is created so it shows up immediately", async () => {
    const { client, wrapper } = mountTree("blender", (fake) => fake.results.set("assets.list", []));
    await flushPromises();
    vi.spyOn(window, "prompt").mockReturnValue("Props");

    await openRootMenu(wrapper);
    await menuItems(wrapper)[0].trigger("click");
    await flushPromises();

    expect(client.callsOf("assets.list")).toHaveLength(2);
  });

  it("selects a file when its row is clicked", async () => {
    const { wrapper } = mountTree("blender", (fake) => {
      fake.results.set("assets.list", [
        { name: "chair", relativePath: "Props/chair.glb", kind: "glb" },
      ]);
    });
    await flushPromises();

    await wrapper.find(".tree-file").trigger("click");
    expect(wrapper.emitted("select")?.[0]).toEqual(["Props/chair.glb"]);
  });

  it("does not treat a folder click as a file selection", async () => {
    const { wrapper } = mountTree("blender", (fake) => {
      fake.results.set("assets.list", [
        { name: "Empty", relativePath: "Empty", kind: "dir" },
      ]);
    });
    await flushPromises();

    await wrapper.find(".tree-dir").trigger("click");
    expect(wrapper.emitted("select")).toBeUndefined();
  });

  it("offers 修改此资产 on file rows but not on folders", async () => {
    const { wrapper } = mountTree("blender", (fake) => {
      fake.results.set("assets.list", [
        { name: "chair", relativePath: "Props/chair.blend", kind: "blend" },
      ]);
    });
    await flushPromises();

    await wrapper.findAll(".tree-dir")[0].trigger("contextmenu", { clientX: 20, clientY: 30 });
    expect(menuItems(wrapper).map((item) => item.text())).not.toContain("修改此资产");

    await wrapper.find(".tree-file").trigger("contextmenu", { clientX: 20, clientY: 50 });
    const items = menuItems(wrapper);
    expect(items.map((item) => item.text())).toEqual([
      "新建文件夹",
      "重命名",
      "删除",
      "在资源管理器中显示",
      "刷新",
      "修改此资产",
    ]);

    await items[5].trigger("click");
    expect(wrapper.emitted("modify")?.[0]).toEqual(["Props/chair.blend"]);
    expect(wrapper.find(".ctx-menu").exists()).toBe(false);
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

  it("surfaces ok:false results such as already-exists", async () => {
    const { wrapper } = mountTree("blender", (fake) => {
      fake.results.set("fs.operate", { ok: false, error: "already-exists" });
    });
    vi.spyOn(window, "prompt").mockReturnValue("Props");

    await openRootMenu(wrapper);
    await menuItems(wrapper)[0].trigger("click");
    await flushPromises();

    expect(wrapper.find(".tree-error").text()).toContain("already-exists");
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

  it("renders the Unity project tree from tree.refresh", async () => {
    const { client, wrapper } = mountTree("unity", (fake) => {
      fake.results.set("tree.refresh", {
        ok: true,
        tree: {
          name: "Assets",
          path: "",
          type: "dir",
          children: [
            {
              name: "Scripts",
              path: "Scripts",
              type: "dir",
              children: [{ name: "Player.cs", path: "Scripts/Player.cs", type: "file" }],
            },
          ],
        },
      });
    });
    await flushPromises();
    expect(wrapper.text()).toContain("Scripts");
    expect(wrapper.text()).toContain("Player.cs");
    expect(client.callsOf("assets.list")).toHaveLength(0);
  });

  it("opens the context menu for the Unity area", async () => {
    const { wrapper } = mountTree("unity", (fake) => {
      fake.results.set("tree.refresh", {
        ok: true,
        tree: { name: "Assets", path: "", type: "dir", children: [] },
      });
    });
    await flushPromises();
    await wrapper.find(".tree-body").trigger("contextmenu", { clientX: 10, clientY: 10 });
    expect(wrapper.find(".ctx-menu").exists()).toBe(true);
    // 修改此资产是 blender 专属：unity 不出现
    expect(menuItems(wrapper).map((item) => item.text())).not.toContain("修改此资产");
  });

  it("shows an empty Unity tree when no project is active", async () => {
    const { wrapper } = mountTree("unity", (fake) => {
      fake.results.set("tree.refresh", { ok: false, error: "no-active-project" });
    });
    await flushPromises();
    expect(wrapper.find(".tree-empty").text()).toContain("尚未导入工程");
  });
});
