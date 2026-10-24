const EXTENSIONS: Record<string, string> = {
  cs: "csharp",
  json: "json",
  ts: "typescript",
  tsx: "typescript",
  mts: "typescript",
  cts: "typescript",
  lua: "lua",
};

// 只映射 PreviewPane 里实际注册过的高亮器；其余返回空串，由调用方回落纯文本
export function languageForPath(path: string): string {
  const dot = path.lastIndexOf(".");
  if (dot <= 0 || dot === path.length - 1) return "";
  return EXTENSIONS[path.slice(dot + 1).toLowerCase()] ?? "";
}
