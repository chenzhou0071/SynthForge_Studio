import { describe, expect, it } from "vitest";
import { maskSecrets } from "./mask";

describe("maskSecrets", () => {
  it("masks secret fields recursively and keeps the rest", () => {
    const masked = maskSecrets({
      agent: { primary: { apiKey: "sk-123", model: "gpt-4o-mini" } },
      cloud3d: {
        tripo: { apiKey: "tp-key" },
        custom: { apiToken: "custom-token" },
        hunyuan: { secretId: "secret-id", secretKey: "secret-key" },
      },
      providerBaseUrls: { openai: "https://api.openai.com/v1" },
    }) as any;

    expect(masked.agent.primary.apiKey).toBe("****");
    expect(masked.agent.primary.model).toBe("gpt-4o-mini");
    expect(masked.cloud3d.tripo.apiKey).toBe("****");
    expect(masked.cloud3d.custom.apiToken).toBe("****");
    expect(masked.cloud3d.hunyuan.secretId).toBe("****");
    expect(masked.cloud3d.hunyuan.secretKey).toBe("****");
    expect(masked.providerBaseUrls.openai).toBe("https://api.openai.com/v1");
  });

  it("keeps empty secrets empty and does not touch arrays of plain values", () => {
    const masked = maskSecrets({
      apiKey: "",
      list: [{ token: "abc" }, "plain"],
    }) as any;

    expect(masked.apiKey).toBe("");
    expect(masked.list[0].token).toBe("****");
    expect(masked.list[1]).toBe("plain");
  });
});
