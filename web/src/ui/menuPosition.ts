// 右键菜单纵向定位：默认贴着光标向下展开；若下方放不下就翻到光标上方，
// 两边都放不下时夹到顶部留白，避免菜单被视口底边截断（用户点不到）。
export function contextMenuTop(
  cursorY: number,
  menuHeight: number,
  viewportHeight: number,
  margin = 8,
): number {
  if (cursorY + menuHeight + margin <= viewportHeight) return cursorY;
  const flipped = cursorY - menuHeight;
  return Math.max(margin, flipped);
}
