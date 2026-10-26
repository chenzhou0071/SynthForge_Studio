export interface FileStamp {
  size: number;
  modified: string;
}

export interface StabilityOptions {
  windowMs?: number; // 连续多久不变才算写完
  pollMs?: number;
  timeoutMs?: number; // 一直写不完时放行，避免预览永远转圈
  sleep?: (ms: number) => Promise<void>;
  now?: () => number;
}

// 导出的 glb 是边写边变的：只有 size 和 modified 连续 windowMs 都没动，
// 才认为文件已经写完，可以交给 three.js 读取。
export async function waitForStableFile(
  stat: () => Promise<FileStamp | null>,
  options: StabilityOptions = {},
): Promise<boolean> {
  const windowMs = options.windowMs ?? 300;
  const pollMs = options.pollMs ?? 150;
  const timeoutMs = options.timeoutMs ?? 5000;
  const sleep = options.sleep ?? ((ms: number) => new Promise((resolve) => setTimeout(resolve, ms)));
  const now = options.now ?? Date.now;
  const startedAt = now();
  let last: FileStamp | null = null;
  let lastAt = 0;

  while (now() - startedAt <= timeoutMs) {
    const stamp = await stat();
    if (!stamp) return false;
    const at = now();
    if (last && stamp.size === last.size && stamp.modified === last.modified) {
      if (at - lastAt >= windowMs) return true;
    } else {
      last = stamp;
      lastAt = at;
    }
    await sleep(pollMs);
  }
  return true;
}
