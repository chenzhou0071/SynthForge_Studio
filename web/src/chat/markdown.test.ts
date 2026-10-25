// @vitest-environment jsdom
import { describe, expect, it } from "vitest";
import { renderMarkdown } from "./markdown";

describe("renderMarkdown", () => {
  it("renders gfm tables", () => {
    const html = renderMarkdown("| 物体 | 尺寸 |\n|---|---|\n| base | 22 |");
    expect(html).toContain("<table>");
    expect(html).toContain("<td>base</td>");
  });

  it("keeps fenced code blocks", () => {
    const html = renderMarkdown("```lua\nprint('hi')\n```");
    expect(html).toContain("<pre><code");
    expect(html).toContain("print(");
  });

  it("strips scripts and inline handlers", () => {
    const html = renderMarkdown('hello <img src=x onerror="alert(1)"><script>alert(2)</script>');
    expect(html).not.toContain("<script");
    expect(html).not.toContain("onerror");
  });

  it("returns an empty string for empty input", () => {
    expect(renderMarkdown("")).toBe("");
  });
});
