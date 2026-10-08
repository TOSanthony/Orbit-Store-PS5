// Keep a small buffer on either side for native scrolling and keyboard focus.
// A focused tile can stay mounted outside this window without filling the gap.
export function railWindow(
  count: number,
  left: number,
  width: number,
  stride: number,
  pinned: number[] = [],
) {
  if (!count || stride <= 0) return [];
  const first = Math.min(count - 1, Math.floor(Math.max(0, left) / stride));
  const start = Math.max(0, first - 3);
  const end = Math.min(
    count,
    first + Math.ceil(Math.max(0, width) / stride) + 4,
  );
  const indices = new Set<number>();
  for (let i = start; i < end; i++) indices.add(i);
  for (const i of pinned) if (i >= 0 && i < count) indices.add(i);
  return [...indices].sort((a, b) => a - b);
}
