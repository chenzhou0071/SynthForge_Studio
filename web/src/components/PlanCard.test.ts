// @vitest-environment jsdom
import { describe, expect, it } from "vitest";
import { mount } from "@vue/test-utils";
import PlanCard from "./PlanCard.vue";
import { FakeClient } from "../testing/fakeClient";

function mountCard(
  groups: { title: string; items: string[]; placements: { name: string; where: string }[]; acceptance: string }[],
) {
  const client = new FakeClient();
  return mount(PlanCard, {
    props: {
      client: client.asClient(),
      area: "blender",
      id: 1,
      state: "pending",
      plan: { taskId: "t", rules: [], groups, totalSteps: 4 },
    },
  });
}

describe("PlanCard", () => {
  // 模型有时把「1. 」写进组标题，卡片自己又补一个序号 → 「1. 1. 地面」双编号
  it("renders a single sequential number even when the title already carries one", () => {
    const wrapper = mountCard([
      { title: "1. 地面与主轴", items: [], placements: [], acceptance: "沿主轴贯通" },
      { title: "近景分区", items: [], placements: [], acceptance: "入口可见" },
    ]);
    const titles = wrapper.findAll(".group-title").map((t) => t.text());
    expect(titles[0]).toBe("1. 地面与主轴");
    expect(titles[1]).toBe("2. 近景分区");
  });
});
