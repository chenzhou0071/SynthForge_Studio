export type AgentProvider = "openai" | "doubao" | "qwen" | "zhipu" | "deepseek" | "custom";
export type AgentProfile = { baseUrl: string; apiKey: string; model: string };
export type AgentConfig = {
  provider: AgentProvider;
  baseUrl: string;
  apiKey: string;
  model: string;
  profiles: Record<string, AgentProfile>;
};

export const AGENT_PROVIDERS: readonly AgentProvider[] = [
  "openai",
  "doubao",
  "qwen",
  "zhipu",
  "deepseek",
  "custom",
];

export function emptyProfile(): AgentProfile {
  return { baseUrl: "", apiKey: "", model: "" };
}

export function normalizeAgentConfig(raw: unknown): AgentConfig {
  const cfg = asObject(raw);
  const provider = isAgentProvider(cfg.provider) ? cfg.provider : "openai";
  const active: AgentProfile = {
    baseUrl: stringOf(cfg.baseUrl),
    apiKey: stringOf(cfg.apiKey),
    model: stringOf(cfg.model),
  };
  // 预置服务商用内置 Base URL，历史遗留的自定义地址不能继续生效
  if (provider !== "custom") active.baseUrl = "";

  const savedProfiles = asObject(cfg.profiles);
  const profiles: Record<string, AgentProfile> = {};
  for (const id of AGENT_PROVIDERS) profiles[id] = normalizeProfile(savedProfiles[id]);

  // 扁平字段是唯一真相：打开设置时用它覆盖当前服务商档案
  profiles[provider] = { ...active };
  return { provider, ...active, profiles };
}

export function stashActiveProfile(cfg: AgentConfig): void {
  cfg.profiles[cfg.provider] = {
    baseUrl: cfg.baseUrl,
    apiKey: cfg.apiKey,
    model: cfg.model,
  };
}

export function switchAgentProvider(cfg: AgentConfig, next: AgentProvider): void {
  if (next === cfg.provider) return;
  stashActiveProfile(cfg);
  const profile = cfg.profiles[next] ?? emptyProfile();
  cfg.provider = next;
  cfg.apiKey = profile.apiKey;
  cfg.model = profile.model;
  cfg.baseUrl = next === "custom" ? profile.baseUrl : "";
}

export function isAgentProvider(value: unknown): value is AgentProvider {
  return typeof value === "string" && (AGENT_PROVIDERS as readonly string[]).includes(value);
}

function normalizeProfile(raw: unknown): AgentProfile {
  const profile = asObject(raw);
  return {
    baseUrl: stringOf(profile.baseUrl),
    apiKey: stringOf(profile.apiKey),
    model: stringOf(profile.model),
  };
}

function asObject(value: unknown): Record<string, unknown> {
  return value && typeof value === "object" && !Array.isArray(value)
    ? (value as Record<string, unknown>)
    : {};
}

function stringOf(value: unknown): string {
  return typeof value === "string" ? value : "";
}
