import createDOMPurify from "dompurify";
import { marked } from "marked";

type Purifier = { sanitize: (html: string) => string };

// 浏览器 ESM 构建导出的是实例，CJS/Node 解析出的是工厂函数：两种形态都要认
const exported = createDOMPurify as unknown as Purifier & ((view: Window) => Purifier);
const purifier: Purifier =
  typeof exported.sanitize === "function" ? exported : exported(window);

// 对话气泡统一走这里：GFM 表格/代码块 + 消毒（模型输出偶尔夹带 HTML）
export function renderMarkdown(text: string): string {
  if (!text) return "";
  const html = marked.parse(text, { async: false, gfm: true, breaks: true }) as string;
  return purifier.sanitize(html);
}
