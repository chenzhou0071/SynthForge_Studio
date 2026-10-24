import { describe, expect, it } from "vitest";
import { languageForPath } from "./language";

describe("languageForPath", () => {
  it("maps the extensions we ship highlighters for", () => {
    expect(languageForPath("Player.cs")).toBe("csharp");
    expect(languageForPath("package.json")).toBe("json");
    expect(languageForPath("main.ts")).toBe("typescript");
    expect(languageForPath("run.tsx")).toBe("typescript");
    expect(languageForPath("init.lua")).toBe("lua");
  });

  it("ignores case and matches only the extension", () => {
    expect(languageForPath("Assets/Scripts/PLAYER.CS")).toBe("csharp");
    expect(languageForPath("a.cs.txt")).toBe("");
  });

  it("returns an empty string when no highlighter applies", () => {
    expect(languageForPath("README.md")).toBe("");
    expect(languageForPath("Makefile")).toBe("");
    expect(languageForPath("")).toBe("");
  });
});
