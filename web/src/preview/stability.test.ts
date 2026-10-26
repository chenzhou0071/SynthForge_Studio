import { describe, expect, it } from "vitest";
import { waitForStableFile } from "./stability";

describe("waitForStableFile", () => {
  it("waits until size and modified stay unchanged for the whole window", async () => {
    let now = 0;
    let calls = 0;
    const stamps = [
      { size: 10, modified: "t1" },
      { size: 20, modified: "t2" },
      { size: 20, modified: "t2" },
      { size: 20, modified: "t2" },
      { size: 20, modified: "t2" },
    ];
    const stat = async () => stamps[Math.min(calls++, stamps.length - 1)];
    const sleep = async (ms: number) => {
      now += ms;
    };

    const ok = await waitForStableFile(stat, {
      windowMs: 300,
      pollMs: 100,
      timeoutMs: 5000,
      sleep,
      now: () => now,
    });

    expect(ok).toBe(true);
    expect(calls).toBe(5); // 先变化一次，再连续 300ms 不变才放行
  });

  it("does not treat two quick identical reads as stable", async () => {
    let now = 0;
    let calls = 0;
    const stat = async () => {
      calls += 1;
      return { size: 10, modified: "t1" };
    };
    const sleep = async (ms: number) => {
      now += ms;
    };

    const ok = await waitForStableFile(stat, {
      windowMs: 300,
      pollMs: 100,
      timeoutMs: 1000,
      sleep,
      now: () => now,
    });

    expect(ok).toBe(true);
    expect(calls).toBeGreaterThan(2);
  });

  it("releases after the timeout when the file never settles", async () => {
    let now = 0;
    let calls = 0;
    const stat = async () => {
      calls += 1;
      return { size: calls, modified: `t${calls}` };
    };
    const sleep = async (ms: number) => {
      now += ms;
    };

    const ok = await waitForStableFile(stat, {
      windowMs: 300,
      pollMs: 100,
      timeoutMs: 500,
      sleep,
      now: () => now,
    });

    expect(ok).toBe(true); // 放行比永远转圈好，加载失败会走原有错误提示
    expect(calls).toBeGreaterThan(1);
  });

  it("reports failure when the file disappears", async () => {
    const ok = await waitForStableFile(async () => null);
    expect(ok).toBe(false);
  });
});
