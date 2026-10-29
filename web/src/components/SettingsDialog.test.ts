// @vitest-environment jsdom
import { flushPromises, mount } from "@vue/test-utils";
import type { VueWrapper } from "@vue/test-utils";
import { describe, expect, it } from "vitest";
import SettingsDialog from "./SettingsDialog.vue";
import { FakeClient } from "../testing/fakeClient";

function mountDialog(settings: unknown = {}) {
  const client = new FakeClient();
  client.results.set("settings.get", settings);
  const wrapper = mount(SettingsDialog, {
    props: { open: false, client: client.asClient() },
  });
  return { client, wrapper };
}

// 对话框的 watch 不带 immediate：先以 open=false 挂载，再翻成 true 才会走 load()
async function openDialog(wrapper: VueWrapper) {
  await wrapper.setProps({ open: true });
  await flushPromises();
}

async function openCloudTab(wrapper: VueWrapper) {
  await wrapper.findAll(".tabs button")[1].trigger("click");
}

// Agent 页两个分组也有“服务商/API Key”，必须限定在 3D 卡片内查找
function cloudField(wrapper: VueWrapper, label: string) {
  return wrapper
    .findAll(".cloud-card .field")
    .find((item) => item.find("span").text() === label);
}

async function save(wrapper: VueWrapper) {
  await wrapper.find(".settings-actions button.primary").trigger("click");
  await flushPromises();
}

describe("SettingsDialog 3D 页", () => {
  it("opens the requested tab directly", async () => {
    const client = new FakeClient();
    client.results.set("settings.get", {});
    const wrapper = mount(SettingsDialog, {
      props: { open: false, client: client.asClient(), initialTab: "cloud3d" },
    });
    await openDialog(wrapper);

    expect(wrapper.find(".tabs button.active").text()).toBe("3D 模型");
  });

  it("shows tripo fields and the default model version", async () => {
    const { wrapper } = mountDialog();
    await openDialog(wrapper);
    await openCloudTab(wrapper);

    const key = cloudField(wrapper, "API Key");
    expect(key?.find("input").attributes("placeholder")).toBe("Tripo API Key");
    expect((cloudField(wrapper, "模型版本")?.find("input").element as HTMLInputElement).value).toBe(
      "v3.1-20260211",
    );
  });

  it("swaps to hunyuan fields with the default region", async () => {
    const { wrapper } = mountDialog();
    await openDialog(wrapper);
    await openCloudTab(wrapper);
    await cloudField(wrapper, "服务商")!.find("select").setValue("hunyuan");

    expect(cloudField(wrapper, "SecretId")).toBeTruthy();
    expect(cloudField(wrapper, "SecretKey")).toBeTruthy();
    expect((cloudField(wrapper, "地域")!.find("input").element as HTMLInputElement).value).toBe(
      "ap-guangzhou",
    );
    // Tripo 的字段必须消失，避免用户以为填了同一个 Key 就能切换
    expect(cloudField(wrapper, "API Key")).toBeUndefined();
  });

  it("swaps to custom fields", async () => {
    const { wrapper } = mountDialog();
    await openDialog(wrapper);
    await openCloudTab(wrapper);
    await cloudField(wrapper, "服务商")!.find("select").setValue("custom");

    expect(cloudField(wrapper, "请求 Base URL")).toBeTruthy();
    expect(cloudField(wrapper, "API Token")).toBeTruthy();
  });

  it("writes edited 3D fields back to settings.save and emits saved", async () => {
    const { client, wrapper } = mountDialog();
    await openDialog(wrapper);
    await openCloudTab(wrapper);
    await cloudField(wrapper, "API Key")!.find("input").setValue("tp-key");
    await save(wrapper);

    const payload = client.callsOf("settings.save")[0].params as any;
    expect(payload.cloud3d.provider).toBe("tripo");
    expect(payload.cloud3d.tripo.apiKey).toBe("tp-key");
    expect(payload.cloud3d.tripo.version).toBe("v3.1-20260211"); // 没改也要回传默认值
    expect(wrapper.emitted("saved")).toHaveLength(1);
    expect(wrapper.text()).toContain("已保存");
  });

  it("saves the provider that is selected at save time", async () => {
    const { client, wrapper } = mountDialog();
    await openDialog(wrapper);
    await openCloudTab(wrapper);
    await cloudField(wrapper, "服务商")!.find("select").setValue("custom");
    await cloudField(wrapper, "请求 Base URL")!.find("input").setValue("http://127.0.0.1:8123");
    await save(wrapper);

    const payload = client.callsOf("settings.save")[0].params as any;
    expect(payload.cloud3d.provider).toBe("custom");
    expect(payload.cloud3d.custom.baseUrl).toBe("http://127.0.0.1:8123");
  });

  it("reports the failure and does not emit saved when settings.save rejects", async () => {
    const { client, wrapper } = mountDialog();
    client.failures.set("settings.save", "disk full");
    await openDialog(wrapper);
    await save(wrapper);

    expect(wrapper.text()).toContain("保存失败：disk full");
    expect(wrapper.emitted("saved")).toBeUndefined();
  });
});
