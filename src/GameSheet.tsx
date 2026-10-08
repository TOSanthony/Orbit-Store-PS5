import { useLayoutEffect, useRef, type ReactNode } from "react";
import { Icon, Modal } from "./components";
import { t } from "./i18n";

export type SheetView =
  "downloads" | "source" | "delivery" | "storage" | "about";

// All controller symbols share the same white disc and black glyph.
export function PadSymbol({
  name,
}: {
  name: "cross" | "circle" | "triangle" | "square";
}) {
  return (
    <svg
      viewBox="0 0 28 28"
      width="28"
      height="28"
      aria-hidden="true"
      className="pad-symbol"
    >
      <circle cx="14" cy="14" r="13" fill="currentColor" />
      <g
        fill="none"
        stroke="#080808"
        strokeWidth="1.8"
        strokeLinecap="round"
        strokeLinejoin="round"
      >
        {name === "cross" ? (
          <path d="m9 9 10 10M9 19 19 9" />
        ) : name === "circle" ? (
          <circle cx="14" cy="14" r="6" />
        ) : name === "triangle" ? (
          <path d="m14 7 7 13H7Z" />
        ) : (
          <path d="M8.5 8.5h11v11h-11Z" />
        )}
      </g>
    </svg>
  );
}

export function GameSheet({
  view,
  title,
  back,
  close,
  children,
}: {
  view: SheetView;
  title: string;
  back: () => void;
  close: () => void;
  children: ReactNode;
}) {
  const previousView = useRef(view);
  useLayoutEffect(() => {
    document.documentElement.classList.add("game-sheet-open");
    return () => document.documentElement.classList.remove("game-sheet-open");
  }, []);
  useLayoutEffect(() => {
    const sheet = document.querySelector<HTMLElement>(".game-sheet");
    const previous = previousView.current;
    const returned =
      view === "downloads" &&
      ["source", "delivery", "storage"].includes(previous)
        ? sheet?.querySelector<HTMLElement>(
            `[data-setting="${previous}"]:not([disabled])`,
          )
        : null;
    const target =
      returned ||
      (view === "downloads"
        ? sheet?.querySelector<HTMLElement>(".sheet-primary:not(:disabled)")
        : null) ||
      sheet?.querySelector<HTMLElement>(
        "[data-initial-focus]:not([disabled])",
      ) ||
      sheet?.querySelector<HTMLElement>(".sheet-back");
    target?.focus({ preventScroll: true });
    previousView.current = view;
  }, [view]);
  return (
    <Modal
      title={title}
      close={close}
      className={`game-sheet game-sheet-${view}`}
      scrimClassName="game-sheet-scrim"
    >
      <header className="sheet-heading">
        <button
          className="sheet-back sheet-visible-back"
          data-dialog-back
          onClick={back}
        >
          <Icon name="back" />
          {t("Back")}
        </button>
        <h2>{title}</h2>
      </header>
      {children}
      <div className="game-pad-hints sheet-hints">
        <span>
          <PadSymbol name="cross" />
          {t("Select")}
        </span>
        <span>
          <PadSymbol name="circle" />
          {t("Back")}
        </span>
      </div>
    </Modal>
  );
}
