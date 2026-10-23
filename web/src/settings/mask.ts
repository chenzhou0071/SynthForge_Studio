const SECRET_KEY = /apiKey|secretKey|secretId|token/i;

export function maskSecrets(value: unknown): unknown {
  if (Array.isArray(value)) return value.map(maskSecrets);
  if (value && typeof value === "object") {
    return Object.fromEntries(
      Object.entries(value as Record<string, unknown>).map(([key, inner]) => [
        key,
        SECRET_KEY.test(key) && typeof inner === "string" && inner.length > 0
          ? "****"
          : maskSecrets(inner),
      ]),
    );
  }
  return value;
}
