// API responses are JSON. Keep unchanged branches so polling does not invalidate
// the catalogue/collection views when only a clock or a transfer changed.
export function shareSnapshot<T>(previous: T, next: T): T {
  if (Object.is(previous, next)) return previous;
  if (
    !previous ||
    !next ||
    typeof previous !== "object" ||
    typeof next !== "object" ||
    Array.isArray(previous) !== Array.isArray(next)
  )
    return next;
  const old = previous as Record<string, unknown>;
  const incoming = next as Record<string, unknown>;
  const keys = Object.keys(incoming);
  let equal = Object.keys(old).length === keys.length;
  let result = incoming;
  for (const key of keys) {
    if (!Object.prototype.hasOwnProperty.call(old, key)) {
      equal = false;
      continue;
    }
    const value = shareSnapshot(old[key], incoming[key]);
    if (value !== old[key]) equal = false;
    if (value !== incoming[key]) {
      if (result === incoming)
        result = (Array.isArray(next) ? [...next] : { ...next }) as Record<
          string,
          unknown
        >;
      result[key] = value;
    }
  }
  return equal ? previous : (result as T);
}

// The server reports whole seconds remaining. Preserve the deadline across
// polls instead of moving it (and rerendering) for network/rounding jitter.
export function refreshDeadline(
  previous: number,
  remaining: number,
  now: number,
): number {
  if (remaining <= 0) return 0;
  const next = now + remaining * 1000;
  return previous > now && Math.abs(previous - next) < 1500 ? previous : next;
}
