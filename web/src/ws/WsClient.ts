type Pending = {
  resolve: (value: unknown) => void;
  reject: (reason: Error) => void;
  timer: ReturnType<typeof setTimeout>;
};

export class WsClient {
  private socket?: WebSocket;
  private pending = new Map<number, Pending>();
  private listeners = new Map<string, Set<(data: unknown) => void>>();
  private nextId = 1;
  private url = "";
  private reconnectDelay = 1000;
  private closedByUser = false;
  private isOpen = false;
  // 子组件的 onMounted 早于 App 的 connect()：请求先入队，连上后统一补发
  private outbox: string[] = [];

  connect(url: string): Promise<void> {
    this.url = url;
    this.closedByUser = false;
    return new Promise<void>((resolve, reject) => {
      const socket = new WebSocket(url);
      this.socket = socket;
      this.isOpen = false;
      socket.onopen = () => {
        this.isOpen = true;
        this.reconnectDelay = 1000;
        this.flushOutbox();
        resolve();
      };
      socket.onerror = () => reject(new Error("ws connect failed"));
      socket.onclose = () => {
        this.isOpen = false;
        this.failAll("connection closed");
        if (!this.closedByUser) this.scheduleReconnect();
      };
      socket.onmessage = (ev: MessageEvent) => this.onMessage(String(ev.data));
    });
  }

  request<T = unknown>(method: string, params: object = {}): Promise<T> {
    const id = this.nextId++;
    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`timeout: ${method}`));
      }, 30000);
      this.pending.set(id, { resolve: resolve as (v: unknown) => void, reject, timer });
      const payload = JSON.stringify({ id, method, params });
      if (this.isOpen && this.socket) this.socket.send(payload);
      else this.outbox.push(payload);
    });
  }

  on(event: string, cb: (data: unknown) => void): () => void {
    if (!this.listeners.has(event)) this.listeners.set(event, new Set());
    this.listeners.get(event)!.add(cb);
    return () => { this.listeners.get(event)?.delete(cb); };
  }

  private onMessage(text: string) {
    const frame = JSON.parse(text) as {
      id?: number; result?: unknown; error?: { message: string }; event?: string; data?: unknown;
    };
    if (frame.id !== undefined) {
      const p = this.pending.get(frame.id);
      if (!p) return;
      clearTimeout(p.timer);
      this.pending.delete(frame.id);
      if (frame.error) p.reject(new Error(frame.error.message));
      else p.resolve(frame.result);
      return;
    }
    if (frame.event) this.listeners.get(frame.event)?.forEach((cb) => cb(frame.data));
  }

  private flushOutbox() {
    if (!this.isOpen || !this.socket) return;
    const queued = this.outbox;
    this.outbox = [];
    queued.forEach((payload) => this.socket?.send(payload));
  }

  private failAll(reason: string) {
    this.outbox = []; // 队列里的请求也在 pending 中，下面会一起 reject
    this.pending.forEach((p) => { clearTimeout(p.timer); p.reject(new Error(reason)); });
    this.pending.clear();
  }

  private scheduleReconnect() {
    const delay = this.reconnectDelay;
    this.reconnectDelay = Math.min(this.reconnectDelay * 2, 10000);
    setTimeout(() => {
      this.connect(this.url).catch(() => { /* 下一轮 onclose 继续退避 */ });
    }, delay);
  }
}
