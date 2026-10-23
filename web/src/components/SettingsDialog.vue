<script setup lang="ts">
import { ref, watch } from "vue";
import { maskSecrets } from "../settings/mask";
import {
  isAgentProvider,
  normalizeAgentConfig,
  stashActiveProfile,
  switchAgentProvider,
} from "../settings/profiles";
import type { AgentConfig, AgentProvider } from "../settings/profiles";
import type { WsClient } from "../ws/WsClient";

type AgentGroup = "primary" | "summarizer";
type CloudProvider = "tripo" | "hunyuan" | "custom";

const props = defineProps<{ open: boolean; client: WsClient }>();
const emit = defineEmits<{
  (e: "update:open", value: boolean): void;
  (e: "saved"): void;
}>();

const agentGroups = [
  { key: "primary" as const, label: "主 Agent" },
  { key: "summarizer" as const, label: "总结 Agent" },
];

const agentProviders: Array<{ value: AgentProvider; label: string; placeholder: string }> = [
  { value: "openai", label: "OpenAI", placeholder: "gpt-4o-mini" },
  { value: "doubao", label: "豆包（火山方舟）", placeholder: "doubao-seed-1-6-250615" },
  { value: "qwen", label: "通义千问", placeholder: "qwen-plus" },
  { value: "zhipu", label: "智谱 GLM", placeholder: "glm-4-plus" },
  { value: "deepseek", label: "DeepSeek", placeholder: "deepseek-chat" },
  { value: "custom", label: "自定义", placeholder: "模型名称" },
];

const cloudProviders: Array<{ value: CloudProvider; label: string }> = [
  { value: "hunyuan", label: "腾讯混元3D" },
  { value: "tripo", label: "Tripo" },
  { value: "custom", label: "自定义" },
];

const tab = ref<"agent" | "cloud3d">("agent");
const settings = ref<any>(null);
const loading = ref(false);
const saving = ref(false);
const error = ref("");
const savedHint = ref("");
const preview = ref(false);
const previewText = ref("");

watch(
  () => props.open,
  (open) => {
    if (open) void load();
  },
);

async function load() {
  tab.value = "agent";
  error.value = "";
  savedHint.value = "";
  preview.value = false;
  loading.value = true;
  try {
    const data = await props.client.request<unknown>("settings.get");
    settings.value = normalize(data);
  } catch (cause) {
    settings.value = null;
    error.value = `读取设置失败：${messageOf(cause)}`;
  } finally {
    loading.value = false;
  }
}

function normalize(raw: unknown): any {
  const data: any = JSON.parse(JSON.stringify(raw ?? {}));
  data.providerBaseUrls = asObject(data.providerBaseUrls);
  data.agent = asObject(data.agent);

  for (const group of agentGroups)
    data.agent[group.key] = normalizeAgentConfig(data.agent[group.key]);

  const cloud = asObject(data.cloud3d);
  data.cloud3d = {
    provider: cloud.provider ?? "tripo",
    tripo: { apiKey: "", version: "v2.5-20250123", ...asObject(cloud.tripo) },
    hunyuan: {
      secretId: "",
      secretKey: "",
      region: "ap-guangzhou",
      model: "",
      ...asObject(cloud.hunyuan),
    },
    custom: { baseUrl: "", apiToken: "", ...asObject(cloud.custom) },
  };
  return data;
}

function asObject(value: unknown): Record<string, any> {
  return value && typeof value === "object" && !Array.isArray(value)
    ? (value as Record<string, any>)
    : {};
}

function placeholderFor(provider: AgentProvider) {
  return agentProviders.find((item) => item.value === provider)?.placeholder ?? "";
}

function onProviderChange(group: AgentGroup, event: Event) {
  const next = (event.target as HTMLSelectElement | null)?.value;
  const cfg = settings.value?.agent[group] as AgentConfig | undefined;
  if (!cfg || !isAgentProvider(next)) return;
  switchAgentProvider(cfg, next);
}

async function save() {
  if (!settings.value || saving.value) return;
  saving.value = true;
  error.value = "";
  savedHint.value = "";
  try {
    // 保存前把当前输入同步进当前服务商档案，保证下次切回能恢复
    for (const group of agentGroups)
      stashActiveProfile(settings.value.agent[group.key] as AgentConfig);
    await props.client.request("settings.save", settings.value);
    savedHint.value = "已保存";
    emit("saved");
  } catch (cause) {
    error.value = `保存失败：${messageOf(cause)}`;
  } finally {
    saving.value = false;
  }
}

async function openPreview() {
  preview.value = true;
  previewText.value = "加载中…";
  try {
    const data = await props.client.request<unknown>("settings.get");
    previewText.value = JSON.stringify(maskSecrets(data), null, 2);
  } catch (cause) {
    previewText.value = `读取失败：${messageOf(cause)}`;
  }
}

function close() {
  preview.value = false;
  emit("update:open", false);
}

function messageOf(cause: unknown) {
  return cause instanceof Error ? cause.message : String(cause);
}
</script>

<template>
  <div v-if="open" class="dialog-mask" @click.self="close">
    <div class="dialog settings-dialog" role="dialog" aria-modal="true" aria-label="设置">
      <header class="dialog-head">
        <h2>设置</h2>
        <button class="icon" title="关闭" @click="close">×</button>
      </header>

      <nav class="tabs">
        <button :class="{ active: tab === 'agent' }" @click="tab = 'agent'">Agent 模型</button>
        <button :class="{ active: tab === 'cloud3d' }" @click="tab = 'cloud3d'">3D 模型</button>
      </nav>

      <p v-if="loading" class="dialog-empty">正在读取配置…</p>
      <p v-else-if="error" class="form-error">{{ error }}</p>

      <template v-if="!loading && settings">
        <section v-show="tab === 'agent'" class="settings-body">
          <div class="settings-grid">
            <article v-for="group in agentGroups" :key="group.key" class="settings-card">
              <h3>{{ group.label }}</h3>

              <label class="field">
                <span>服务商</span>
                <select
                  :value="settings.agent[group.key].provider"
                  @change="onProviderChange(group.key, $event)"
                >
                  <option v-for="p in agentProviders" :key="p.value" :value="p.value">
                    {{ p.label }}
                  </option>
                </select>
              </label>

              <label class="field">
                <span>模型名称</span>
                <input
                  v-model="settings.agent[group.key].model"
                  :placeholder="placeholderFor(settings.agent[group.key].provider)"
                />
              </label>

              <label class="field">
                <span>API Key</span>
                <input
                  v-model="settings.agent[group.key].apiKey"
                  type="password"
                  autocomplete="off"
                  placeholder="sk-..."
                />
              </label>

              <label v-if="settings.agent[group.key].provider === 'custom'" class="field">
                <span>请求 Base URL</span>
                <input v-model="settings.agent[group.key].baseUrl" placeholder="https://your-host/v1" />
              </label>
              <p v-else class="field-hint">Base URL 使用内置地址，不需要填写。</p>
            </article>
          </div>
        </section>

        <section v-show="tab === 'cloud3d'" class="settings-body">
          <article class="settings-card cloud-card">
            <label class="field">
              <span>服务商</span>
              <select v-model="settings.cloud3d.provider">
                <option v-for="p in cloudProviders" :key="p.value" :value="p.value">
                  {{ p.label }}
                </option>
              </select>
            </label>

            <template v-if="settings.cloud3d.provider === 'tripo'">
              <label class="field">
                <span>API Key</span>
                <input
                  v-model="settings.cloud3d.tripo.apiKey"
                  type="password"
                  autocomplete="off"
                  placeholder="Tripo API Key"
                />
              </label>
              <label class="field">
                <span>模型版本</span>
                <input v-model="settings.cloud3d.tripo.version" placeholder="v2.5-20250123" />
              </label>
              <p class="field-hint">直连 Tripo OpenAPI（Bearer 认证）。</p>
            </template>

            <template v-else-if="settings.cloud3d.provider === 'hunyuan'">
              <label class="field">
                <span>SecretId</span>
                <input v-model="settings.cloud3d.hunyuan.secretId" autocomplete="off" />
              </label>
              <label class="field">
                <span>SecretKey</span>
                <input
                  v-model="settings.cloud3d.hunyuan.secretKey"
                  type="password"
                  autocomplete="off"
                />
              </label>
              <label class="field">
                <span>地域</span>
                <input v-model="settings.cloud3d.hunyuan.region" placeholder="ap-guangzhou" />
              </label>
              <label class="field">
                <span>模型版本</span>
                <input v-model="settings.cloud3d.hunyuan.model" placeholder="3.0" />
              </label>
              <p class="field-hint">腾讯云 API 3.0，使用 TC3-HMAC-SHA256 签名。</p>
            </template>

            <template v-else>
              <label class="field">
                <span>请求 Base URL</span>
                <input v-model="settings.cloud3d.custom.baseUrl" placeholder="https://your-host" />
              </label>
              <label class="field">
                <span>API Token</span>
                <input
                  v-model="settings.cloud3d.custom.apiToken"
                  type="password"
                  autocomplete="off"
                />
              </label>
              <p class="field-hint">
                自定义服务需实现 POST /task 与 GET /task/{id}，协议与 Tripo 风格一致。
              </p>
            </template>
          </article>
        </section>

        <footer class="settings-actions">
          <span v-if="savedHint" class="form-ok">{{ savedHint }}</span>
          <span class="spacer" />
          <button class="secondary" :disabled="saving" @click="openPreview">预览配置</button>
          <button class="primary" :disabled="saving" @click="save">
            {{ saving ? "保存中…" : "保存配置" }}
          </button>
        </footer>
      </template>
    </div>
  </div>

  <div v-if="preview" class="dialog-mask preview-mask" @click.self="preview = false">
    <div class="dialog preview-dialog" role="dialog" aria-modal="true" aria-label="预览配置">
      <header class="dialog-head">
        <h2>预览配置（密钥已脱敏）</h2>
        <button class="icon" title="关闭" @click="preview = false">×</button>
      </header>
      <pre class="preview-json">{{ previewText }}</pre>
    </div>
  </div>
</template>
