import { afterEach, describe, expect, it, vi } from "vitest";
import { WsClient } from "./WsClient";

class MockWebSocket {
  static instances: MockWebSocket[] = [];
  onopen?: () => void;
  onmessage?: (ev: { data: string }) => void;
  onclose?: () => void;
  onerror?: () => void;
  sent: string[] = [];
  constructor(public url: string) { MockWebSocket.instances.push(this); }
  send(data: string) { this.sent.push(data); }
  close() {}
  emit(payload: object) { this.onmessage?.({ data: JSON.stringify(payload) }); }
  open() { this.onopen?.(); }
}

describe("WsClient", () => {
  afterEach(() => { MockWebSocket.instances = []; });

  it("matches responses by id", async () => {
    vi.stubGlobal("WebSocket", MockWebSocket as unknown as typeof WebSocket);
    const client = new WsClient();
    const ready = client.connect("ws://x");
    MockWebSocket.instances[0].open();
    await ready;

    const p = client.request("settings.get");
    const req = JSON.parse(MockWebSocket.instances[0].sent[0]);
    expect(req.method).toBe("settings.get");
    MockWebSocket.instances[0].emit({ id: req.id, result: { ok: true } });
    await expect(p).resolves.toEqual({ ok: true });
  });

  it("dispatches events without id", async () => {
    vi.stubGlobal("WebSocket", MockWebSocket as unknown as typeof WebSocket);
    const client = new WsClient();
    const ready = client.connect("ws://x");
    MockWebSocket.instances[0].open();
    await ready;

    const got: any[] = [];
    client.on("chat.delta", (d) => got.push(d));
    MockWebSocket.instances[0].emit({ event: "chat.delta", data: { text: "hi" } });
    expect(got).toEqual([{ text: "hi" }]);
  });

  it("rejects pending requests on close", async () => {
    vi.stubGlobal("WebSocket", MockWebSocket as unknown as typeof WebSocket);
    const client = new WsClient();
    const ready = client.connect("ws://x");
    MockWebSocket.instances[0].open();
    await ready;

    const p = client.request("slow.call");
    MockWebSocket.instances[0].onclose?.();
    await expect(p).rejects.toThrow("connection closed");
  });

  it("queues requests made before the socket opens", async () => {
    vi.stubGlobal("WebSocket", MockWebSocket as unknown as typeof WebSocket);
    const client = new WsClient();
    const ready = client.connect("ws://x");

    // 子组件 onMounted 早于 App 的 connect()：此时发请求不能静默丢失
    const p = client.request("session.load", { area: "blender" });
    expect(MockWebSocket.instances[0].sent).toHaveLength(0);

    MockWebSocket.instances[0].open();
    await ready;
    expect(MockWebSocket.instances[0].sent).toHaveLength(1);

    const req = JSON.parse(MockWebSocket.instances[0].sent[0]);
    expect(req.method).toBe("session.load");
    MockWebSocket.instances[0].emit({ id: req.id, result: { messages: [] } });
    await expect(p).resolves.toEqual({ messages: [] });
  });

  it("queues requests made before connect is even called", async () => {
    vi.stubGlobal("WebSocket", MockWebSocket as unknown as typeof WebSocket);
    const client = new WsClient();
    const p = client.request("settings.get");

    const ready = client.connect("ws://x");
    MockWebSocket.instances[0].open();
    await ready;

    expect(MockWebSocket.instances[0].sent).toHaveLength(1);
    const req = JSON.parse(MockWebSocket.instances[0].sent[0]);
    MockWebSocket.instances[0].emit({ id: req.id, result: { ok: true } });
    await expect(p).resolves.toEqual({ ok: true });
  });
});
