import { describe, expect, it } from "vitest";
import { normalizeAgentConfig, switchAgentProvider } from "./profiles";

describe("agent profiles", () => {
  it("seeds the active provider and keeps other provider profiles", () => {
    const cfg = normalizeAgentConfig({
      provider: "deepseek",
      model: "deepseek-chat",
      apiKey: "sk-ds",
      profiles: { openai: { model: "gpt-4o", apiKey: "sk-oa", baseUrl: "" } },
    });

    expect(cfg.provider).toBe("deepseek");
    expect(cfg.profiles.deepseek).toEqual({
      model: "deepseek-chat",
      apiKey: "sk-ds",
      baseUrl: "",
    });
    expect(cfg.profiles.openai).toEqual({
      model: "gpt-4o",
      apiKey: "sk-oa",
      baseUrl: "",
    });
  });

  it("stashes current edits and restores the target provider's own config", () => {
    const cfg = normalizeAgentConfig({ provider: "openai", profiles: {} });
    cfg.model = "gpt-4o-mini";
    cfg.apiKey = "sk-oa";

    switchAgentProvider(cfg, "deepseek");
    expect(cfg.provider).toBe("deepseek");
    expect(cfg.model).toBe("");
    cfg.model = "deepseek-chat";
    cfg.apiKey = "sk-ds";

    switchAgentProvider(cfg, "openai");
    expect(cfg.model).toBe("gpt-4o-mini");
    expect(cfg.apiKey).toBe("sk-oa");
  });

  it("keeps a custom base URL when switching away and back", () => {
    const cfg = normalizeAgentConfig({
      provider: "custom",
      model: "m",
      apiKey: "k",
      baseUrl: "https://x/v1",
    });

    switchAgentProvider(cfg, "qwen");
    expect(cfg.baseUrl).toBe("");
    switchAgentProvider(cfg, "custom");
    expect(cfg.baseUrl).toBe("https://x/v1");
  });
});
