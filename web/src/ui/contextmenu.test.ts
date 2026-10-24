// @vitest-environment jsdom
import { afterEach, describe, expect, it } from "vitest";
import { suppressNativeContextMenu } from "./contextmenu";

let restore: (() => void) | null = null;

afterEach(() => {
  restore?.();
  restore = null;
  window.getSelection()?.removeAllRanges();
  document.body.innerHTML = "";
});

function dispatchContextMenu(target: Element): MouseEvent {
  const event = new MouseEvent("contextmenu", { bubbles: true, cancelable: true });
  target.dispatchEvent(event);
  return event;
}

describe("suppressNativeContextMenu", () => {
  it("prevents the native menu outside editable fields", () => {
    restore = suppressNativeContextMenu(document);
    expect(dispatchContextMenu(document.body).defaultPrevented).toBe(true);
  });

  it("keeps the native menu for text inputs", () => {
    restore = suppressNativeContextMenu(document);
    const input = document.createElement("input");
    document.body.append(input);
    expect(dispatchContextMenu(input).defaultPrevented).toBe(false);
  });

  it("keeps the native menu for textareas", () => {
    restore = suppressNativeContextMenu(document);
    const textarea = document.createElement("textarea");
    document.body.append(textarea);
    expect(dispatchContextMenu(textarea).defaultPrevented).toBe(false);
  });

  it("keeps the native menu for contenteditable elements", () => {
    restore = suppressNativeContextMenu(document);
    const editor = document.createElement("div");
    editor.setAttribute("contenteditable", "true");
    const child = document.createElement("span");
    editor.append(child);
    document.body.append(editor);
    expect(dispatchContextMenu(child).defaultPrevented).toBe(false);
  });

  it("keeps the native menu when text is selected", () => {
    restore = suppressNativeContextMenu(document);
    const paragraph = document.createElement("p");
    paragraph.textContent = "hello";
    document.body.append(paragraph);
    const range = document.createRange();
    range.selectNodeContents(paragraph);
    const selection = window.getSelection();
    if (!selection) throw new Error("no selection support");
    selection.removeAllRanges();
    selection.addRange(range);
    expect(dispatchContextMenu(paragraph).defaultPrevented).toBe(false);
  });

  it("stops intercepting after cleanup", () => {
    restore = suppressNativeContextMenu(document);
    restore();
    restore = null;
    expect(dispatchContextMenu(document.body).defaultPrevented).toBe(false);
  });
});
