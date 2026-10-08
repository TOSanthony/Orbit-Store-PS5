import {
  memo,
  useCallback,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
  type ReactNode,
} from "react";
import { GameTile } from "./components";
import { railWindow } from "./railWindow";
import type { Game } from "./types";

export type RailMemory = {
  left: number;
  focusId: string;
  height: number;
  tile?: number;
  gap?: number;
};

export const GameRail = memo(function GameRail({
  games,
  focusedId,
  showDate,
  firstCovers,
  memory,
  states,
  favorites,
  open,
  select,
  instantScroll = false,
}: {
  games: Game[];
  focusedId: string;
  showDate: boolean;
  firstCovers: number;
  memory: RailMemory;
  states: Map<string, { label: string } | undefined>;
  favorites: Set<string>;
  open: (game: Game) => void;
  select: (id: string) => void;
  instantScroll?: boolean;
}) {
  const rail = useRef<HTMLDivElement>(null);
  const [focusId, setFocusId] = useState(memory.focusId);
  const [pendingFocus, setPendingFocus] = useState("");
  const [height, setHeight] = useState(memory.height);
  const heightWidth = useRef(memory.tile);
  const [view, setView] = useState(() => ({
    left: memory.left,
    width: window.innerWidth,
    tile: memory.tile || 176,
    gap: memory.gap ?? 22.4,
  }));
  const measuredView = useRef(view);
  const positions = useMemo(
    () => new Map(games.map((g, i) => [g.id, i])),
    [games],
  );
  const indices = railWindow(
    games.length,
    view.left,
    view.width,
    view.tile + view.gap,
    [positions.get(focusId) ?? -1, positions.get(pendingFocus) ?? -1],
  );
  const windowKey = indices.join(",");
  const onFocus = useCallback(
    (id: string) => {
      memory.focusId = id;
      setFocusId(id);
      select(id);
    },
    [memory, select],
  );
  const focusTile = useCallback(
    (tile: HTMLElement) => {
      tile.focus({ preventScroll: true });
      const row = rail.current!.getBoundingClientRect();
      const card = tile.getBoundingClientRect();
      // Most moves stay inside the visible row. Do not restart a native scroll
      // animation on every D-pad repeat or scroll the whole page unnecessarily.
      if (
        card.left < row.left ||
        card.right > row.right ||
        card.top < 0 ||
        card.bottom > window.innerHeight
      )
        tile.scrollIntoView({
          block: "nearest",
          inline: "nearest",
          behavior:
            instantScroll ||
            window.matchMedia?.("(prefers-reduced-motion: reduce)").matches
              ? "auto"
              : "smooth",
        });
    },
    [instantScroll],
  );

  useLayoutEffect(() => {
    const element = rail.current!;
    let frame = 0;
    function publish(next: typeof view) {
      const old = measuredView.current;
      const first = (value: typeof view) =>
        Math.floor(Math.max(0, value.left) / (value.tile + value.gap));
      // Scrolling within one card does not change the mounted window. Avoid
      // React work for every pixel of the browser's smooth-scroll animation.
      if (
        first(old) === first(next) &&
        old.width === next.width &&
        old.tile === next.tile &&
        old.gap === next.gap
      )
        return;
      measuredView.current = next;
      setView(next);
    }
    function scroll() {
      frame = 0;
      publish({ ...measuredView.current, left: element.scrollLeft });
    }
    function measure() {
      const tile = element.querySelector<HTMLElement>(".game-tile");
      if (!tile) return;
      const gap = parseFloat(getComputedStyle(element).columnGap) || 0;
      const next = {
        left: element.scrollLeft,
        width: element.clientWidth,
        tile: parseFloat(getComputedStyle(tile).width) || tile.offsetWidth,
        gap,
      };
      memory.left = next.left;
      memory.tile = next.tile;
      memory.gap = next.gap;
      publish(next);
    }
    function schedule() {
      // Save immediately so opening details before the next frame keeps position.
      memory.left = element.scrollLeft;
      if (!frame) frame = requestAnimationFrame(scroll);
    }
    element.scrollLeft = memory.left;
    measure();
    element.addEventListener("scroll", schedule, { passive: true });
    window.addEventListener("resize", measure);
    const observer =
      typeof ResizeObserver !== "undefined"
        ? new ResizeObserver(measure)
        : null;
    observer?.observe(element);
    return () => {
      cancelAnimationFrame(frame);
      element.removeEventListener("scroll", schedule);
      window.removeEventListener("resize", measure);
      observer?.disconnect();
    };
  }, [games, memory]);

  useLayoutEffect(() => {
    const element = rail.current!;
    const style = getComputedStyle(element);
    const tileHeight = Math.max(
      0,
      ...Array.from(
        element.querySelectorAll<HTMLElement>(".game-tile"),
        (tile) => tile.offsetHeight,
      ),
    );
    // Prevent row collapse when a short title replaces a taller off-screen tile.
    const measured =
      tileHeight +
      parseFloat(style.paddingTop) +
      parseFloat(style.paddingBottom);
    memory.height =
      heightWidth.current === view.tile
        ? Math.max(memory.height, measured)
        : measured;
    heightWidth.current = view.tile;
    setHeight(memory.height);
  }, [windowKey, games, view.tile, memory]);

  useLayoutEffect(() => {
    if (!pendingFocus) return;
    const tile = Array.from(
      rail.current!.querySelectorAll<HTMLElement>(".game-tile"),
    ).find((element) => element.dataset.game === pendingFocus);
    if (tile) focusTile(tile);
    setPendingFocus("");
  }, [pendingFocus, focusTile]);

  const children: ReactNode[] = [];
  let previous = -1;
  function spacer(count: number, key: number) {
    if (count > 0)
      children.push(
        <span
          key={`gap-${key}`}
          className="rail-spacer"
          aria-hidden="true"
          style={{ width: count * (view.tile + view.gap) - view.gap }}
        />,
      );
  }
  for (const index of indices) {
    spacer(index - previous - 1, index);
    const game = games[index];
    children.push(
      <GameTile
        key={game.id}
        game={game}
        status={states.get(game.id)?.label}
        favorite={favorites.has(game.id)}
        showDate={showDate}
        featured={game.id === focusedId}
        priority={game.id === focusedId || index < firstCovers}
        open={open}
        onFocus={onFocus}
      />,
    );
    previous = index;
  }
  spacer(games.length - previous - 1, games.length);
  return (
    <div
      ref={rail}
      className="game-rail virtual-rail"
      data-virtual-rail="true"
      style={{ minHeight: height || undefined }}
      onKeyDown={(event) => {
        if (event.altKey || event.ctrlKey || event.metaKey || event.shiftKey)
          return;
        if (!["ArrowLeft", "ArrowRight", "Home", "End"].includes(event.key))
          return;
        const tile = (event.target as HTMLElement).closest<HTMLElement>(
          ".game-tile",
        );
        const index = positions.get(tile?.dataset.game || "");
        if (index === undefined) return;
        event.preventDefault();
        document.documentElement.classList.remove("pointer");
        const next =
          event.key === "Home"
            ? 0
            : event.key === "End"
              ? games.length - 1
              : Math.max(
                  0,
                  Math.min(
                    games.length - 1,
                    index + (event.key === "ArrowLeft" ? -1 : 1),
                  ),
                );
        if (next !== index) {
          const id = games[next].id;
          const mounted = Array.from(
            rail.current!.querySelectorAll<HTMLElement>(".game-tile"),
          ).find((element) => element.dataset.game === id);
          if (mounted) focusTile(mounted);
          else setPendingFocus(id);
        }
      }}
    >
      {children}
    </div>
  );
});
