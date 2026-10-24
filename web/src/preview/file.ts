export type PreviewKind = "text" | "image" | "info";

// 预览区的数据契约：P4 的 asset.created / file.read 负责填充，kind=model 由 P4 接管
export interface PreviewFile {
  path: string;
  kind: PreviewKind;
  content?: string;
  dataUrl?: string;
  language?: string;
}
