// 全局屏蔽 Chromium 原生右键菜单（Back/Reload/Save page）。
// 输入框与可编辑区域放行，保留系统复制/粘贴菜单。
const EDITABLE = "input, textarea, [contenteditable]:not([contenteditable='false'])";

export function isEditableTarget(target: EventTarget | null): boolean {
  return target instanceof Element && target.closest(EDITABLE) !== null;
}

function hasSelectedText(doc: Document): boolean {
  return (doc.getSelection()?.toString() ?? "").length > 0;
}

export function suppressNativeContextMenu(doc: Document): () => void {
  const onContextMenu = (event: MouseEvent) => {
    // 输入框保留完整的编辑菜单；有选中文本时保留复制/全选
    if (isEditableTarget(event.target) || hasSelectedText(doc)) return;
    event.preventDefault();
  };
  doc.addEventListener("contextmenu", onContextMenu);
  return () => doc.removeEventListener("contextmenu", onContextMenu);
}
