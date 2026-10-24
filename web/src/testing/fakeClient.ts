import type { WsClient } from "../ws/WsClient";

// 组件测试用的 WsClient 替身：记录请求、按 method 返回预置结果或错误。
export class FakeClient {
  calls: Array<{ method: string; params: Record<string, unknown> }> = [];
  results = new Map<string, unknown>();
  failures = new Map<string, string>();
  private handlers = new Map<string, Set<(data: unknown) => void>>();

  asClient(): WsClient {
    return this as unknown as WsClient;
  }

  on(event: string, cb: (data: unknown) => void): () => void {
    if (!this.handlers.has(event)) this.handlers.set(event, new Set());
    this.handlers.get(event)!.add(cb);
    return () => {
      this.handlers.get(event)?.delete(cb);
    };
  }

  request<T = unknown>(method: string, params: object = {}): Promise<T> {
    this.calls.push({ method, params: params as Record<string, unknown> });
    const failure = this.failures.get(method);
    if (failure) return Promise.reject(new Error(failure));
    return Promise.resolve(this.results.get(method) as T);
  }

  emit(event: string, data: unknown): void {
    this.handlers.get(event)?.forEach((cb) => cb(data));
  }

  callsOf(method: string): Array<{ method: string; params: Record<string, unknown> }> {
    return this.calls.filter((call) => call.method === method);
  }
}
