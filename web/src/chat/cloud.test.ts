import { describe, expect, it } from "vitest";
import { isCloudConfigured } from "./cloud";

describe("isCloudConfigured", () => {
  it("requires a tripo api key when provider is tripo or unset", () => {
    expect(isCloudConfigured({ cloud3d: { provider: "tripo", tripo: { apiKey: "" } } })).toBe(false);
    expect(isCloudConfigured({ cloud3d: { provider: "tripo", tripo: { apiKey: "t-key" } } })).toBe(true);
    // provider 缺省按 tripo 处理，与 SettingsDialog 的 normalize 保持一致
    expect(isCloudConfigured({ cloud3d: { tripo: { apiKey: "t-key" } } })).toBe(true);
  });

  it("treats whitespace-only keys as missing", () => {
    expect(isCloudConfigured({ cloud3d: { provider: "tripo", tripo: { apiKey: "   " } } })).toBe(false);
  });

  it("requires both hunyuan secrets", () => {
    const cloud3d = { provider: "hunyuan", hunyuan: { secretId: "id", secretKey: "" } };
    expect(isCloudConfigured({ cloud3d })).toBe(false);
    expect(isCloudConfigured({ cloud3d: { ...cloud3d, hunyuan: { secretId: "id", secretKey: "key" } } })).toBe(true);
  });

  it("requires base url and token for custom providers", () => {
    expect(isCloudConfigured({ cloud3d: { provider: "custom", custom: { baseUrl: "https://x" } } })).toBe(false);
    expect(
      isCloudConfigured({ cloud3d: { provider: "custom", custom: { baseUrl: "https://x", apiToken: "tok" } } }),
    ).toBe(true);
  });

  it("treats unknown providers and missing settings as not configured", () => {
    expect(isCloudConfigured({ cloud3d: { provider: "unknown" } })).toBe(false);
    expect(isCloudConfigured(undefined)).toBe(false);
  });
});
