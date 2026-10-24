type CloudSettings = Record<string, any> | null | undefined;

function filled(value: unknown): boolean {
  return typeof value === "string" && value.trim() !== "";
}

// 云 3D 是否已配置：字段与 SettingsDialog 的三种服务商一一对应
export function isCloudConfigured(settings: unknown): boolean {
  const cloud = (settings as { cloud3d?: CloudSettings } | null | undefined)?.cloud3d ?? {};
  const provider = cloud.provider ?? "tripo";
  if (provider === "tripo") return filled(cloud.tripo?.apiKey);
  if (provider === "hunyuan") return filled(cloud.hunyuan?.secretId) && filled(cloud.hunyuan?.secretKey);
  if (provider === "custom") return filled(cloud.custom?.baseUrl) && filled(cloud.custom?.apiToken);
  return false;
}
