import { describe, expect, it } from "vitest";
import { contextMenuTop } from "./menuPosition";

describe("contextMenuTop", () => {
  it("keeps the menu at the cursor when it fits below", () => {
    // 视口 1000，光标 50，菜单高 200 → 50+200 放得下
    expect(contextMenuTop(50, 200, 1000)).toBe(50);
  });

  it("flips above the cursor when the menu would overflow the bottom", () => {
    // 光标 900、菜单 200：900+200 超出 1000 → 菜单底贴光标，顶移到 700
    expect(contextMenuTop(900, 200, 1000)).toBe(700);
  });

  it("clamps to the top margin when neither side fits", () => {
    // 视口很小、菜单很高：向上翻也顶出界 → 夹到最小 margin(8)
    expect(contextMenuTop(50, 200, 100)).toBe(8);
  });

  it("does not flip for a zero-height menu", () => {
    expect(contextMenuTop(990, 0, 1000)).toBe(990);
  });
});
