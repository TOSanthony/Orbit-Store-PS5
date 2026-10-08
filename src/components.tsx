import { memo, useLayoutEffect, useRef, type ReactNode } from "react";
import type { Game, Storage } from "./types";
import { bytes } from "./types";
import { Artwork } from "./Artwork";
import { formatDate, placeName, t } from "./i18n";
export function Icon({
  name,
}: {
  name:
    | "search"
    | "usb"
    | "close"
    | "download"
    | "check"
    | "power"
    | "settings"
    | "back"
    | "sources"
    | "heart"
    | "storage"
    | "scan"
    | "more"
    | "chevron"
    | "info";
}) {
  return (
    <svg
      width="26"
      height="26"
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth="1.6"
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden="true"
    >
      {name === "more" ? (
        <>
          <circle cx="5" cy="12" r="1" fill="currentColor" />
          <circle cx="12" cy="12" r="1" fill="currentColor" />
          <circle cx="19" cy="12" r="1" fill="currentColor" />
        </>
      ) : name === "chevron" ? (
        <path d="m9 5 7 7-7 7" />
      ) : name === "info" ? (
        <>
          <rect x="5" y="3" width="14" height="18" rx="2" />
          <path d="M9 8h6M9 12h6M9 16h4" />
        </>
      ) : name === "heart" ? (
        <path d="M20.5 5.7a5 5 0 0 0-8.5.6 5 5 0 0 0-8.5-.6C.6 9.4 4 13.6 12 20c8-6.4 11.4-10.6 8.5-14.3Z" />
      ) : name === "storage" ? (
        <>
          <ellipse cx="12" cy="5" rx="8" ry="3" />
          <path d="M4 5v14c0 4 16 4 16 0V5M4 10c0 4 16 4 16 0M4 15c0 4 16 4 16 0" />
        </>
      ) : name === "scan" ? (
        <>
          <path d="M8 3H4a1 1 0 0 0-1 1v4m13-5h4a1 1 0 0 1 1 1v4M3 16v4a1 1 0 0 0 1 1h4m8 0h4a1 1 0 0 0 1-1v-4" />
          <circle cx="11" cy="11" r="5" />
          <path d="m15 15 3 3" />
        </>
      ) : name === "search" ? (
        <>
          <circle cx="10.5" cy="10.5" r="6.8" />
          <path d="m16 16 5 5" />
        </>
      ) : name === "sources" ? (
        <>
          <path d="m12 3 9 5-9 5-9-5 9-5ZM3 12l9 5 9-5M3 16l9 5 9-5" />
        </>
      ) : name === "usb" ? (
        <>
          <path d="M8 8h8v13H8zM9 8V2h6v6M11 5h2M11 12h2" />
        </>
      ) : name === "close" ? (
        <path d="m6 6 12 12M6 18 18 6" />
      ) : name === "check" ? (
        <path d="m5 12 4 4L19 6" />
      ) : name === "back" ? (
        <path d="M15 5 8 12l7 7" />
      ) : name === "power" ? (
        <path d="M12 3v9M7.5 5.8a8 8 0 1 0 9 0" />
      ) : name === "settings" ? (
        <>
          <path d="M9.67 2.5h4.66l.5 2.24 1.8 1.04 2.19-.69 2.33 4.04-1.69 1.55v2.08l1.69 1.55-2.33 4.04-2.19-.69-1.8 1.04-.5 2.24H9.67l-.5-2.24-1.8-1.04-2.19.69-2.33-4.04 1.69-1.55v-2.08L2.85 9.13l2.33-4.04 2.19.69 1.8-1.04z" />
          <circle cx="12" cy="11.72" r="3.2" />
        </>
      ) : (
        <>
          <path d="M12 3v12m-5-5 5 5 5-5M4 16v5h16v-5" />
        </>
      )}
    </svg>
  );
}
export function Modal({
  title,
  close,
  children,
  className = "",
  scrimClassName = "",
}: {
  title: string;
  close: () => void;
  children: ReactNode;
  className?: string;
  scrimClassName?: string;
}) {
  const ref = useRef<HTMLDivElement>(null);
  useLayoutEffect(() => {
    const previous = document.activeElement as HTMLElement;
    const first = ref.current?.querySelector<HTMLElement>(
      "[data-initial-focus], button, input, select",
    );
    first?.focus();
    const old = document.body.style.overflow;
    document.body.style.overflow = "hidden";
    function trap(e: KeyboardEvent) {
      if (e.key !== "Tab") return;
      const elements = [
        ...(ref.current?.querySelectorAll<HTMLElement>(
          'button:not([disabled]),input,select,[tabindex="0"]',
        ) || []),
      ].filter((x) => x.offsetWidth);
      if (e.shiftKey && document.activeElement === elements[0]) {
        e.preventDefault();
        elements[elements.length - 1]?.focus();
      } else if (
        !e.shiftKey &&
        document.activeElement === elements[elements.length - 1]
      ) {
        e.preventDefault();
        elements[0]?.focus();
      }
    }
    document.addEventListener("keydown", trap);
    return () => {
      document.body.style.overflow = old;
      document.removeEventListener("keydown", trap);
      // Wait until the parent has removed inert/hidden from the game hub.
      // A newly opened dialog or provider notice may already own focus.
      requestAnimationFrame(() => {
        if (previous?.isConnected && document.activeElement === document.body)
          previous.focus({ preventScroll: true });
      });
    };
  }, []);
  return (
    <div
      className={`scrim ${scrimClassName}`}
      onClick={(e) => {
        if (e.target === e.currentTarget) close();
      }}
    >
      <div
        ref={ref}
        className={`modal ${className}`}
        role="dialog"
        aria-modal="true"
        aria-label={title}
      >
        <button
          className="close icon-button"
          onClick={close}
          aria-label={t("Close")}
        >
          <Icon name="close" />
        </button>
        {children}
      </div>
    </div>
  );
}
export const GameTile = memo(function GameTile({
  game,
  open,
  onFocus,
  showDate = false,
  favorite = false,
  priority = false,
  featured = false,
  status,
}: {
  game: Game;
  open: (game: Game) => void;
  onFocus?: (id: string) => void;
  showDate?: boolean;
  favorite?: boolean;
  priority?: boolean;
  featured?: boolean;
  status?: string;
}) {
  return (
    <button
      className="game-tile"
      data-game={game.id}
      onClick={() => open(game)}
      onFocus={() => onFocus?.(game.id)}
      aria-label={`${t("View {title}", { title: game.title })}${status ? `, ${t(status)}` : ""}`}
      aria-current={featured ? "true" : undefined}
    >
      <span className="cover">
        <Artwork
          key={game.cover}
          src={game.cover}
          fallbackSrc={game.coverFallback}
          alt=""
          width="600"
          height="600"
          priority={priority}
          decoding="async"
          referrerPolicy="no-referrer"
        />
      </span>
      <span className="tile-title">{game.title}</span>
      {status && <span className="tile-collection-state">{t(status)}</span>}
      {favorite && (
        <span className="tile-favorite" aria-label={t("Favourite")}>
          ★
        </span>
      )}
      {showDate && game.releaseDate && (
        <time className="tile-date" dateTime={game.releaseDate}>
          {formatDate(new Date(game.releaseDate), {
            month: "short",
            day: "numeric",
            year: "numeric",
            timeZone: "UTC",
          })}
        </time>
      )}
    </button>
  );
});
export { Backdrop } from "./Backdrop";
export function StorageRows({ drives }: { drives: Storage[] }) {
  return (
    <div className="storage-list">
      {drives.length ? (
        drives.map((d) => (
          <div className="storage-row" key={d.id}>
            <Icon name="usb" />
            <div>
              <h3>{placeName(d.label)}</h3>
              <p>{d.path}</p>
              <div className="storage-meter">
                <span
                  style={{
                    width: `${Math.min(100, Math.max(0, (1 - d.freeBytes / d.totalBytes) * 100))}%`,
                  }}
                />
              </div>
              <p>
                {t("{free} free of {total}", {
                  free: bytes(d.freeBytes),
                  total: bytes(d.totalBytes),
                })}
              </p>
              <p>
                {t("{size} needed by unfinished downloads", {
                  size: bytes(d.pendingBytes),
                })}
                {" · "}
                {d.projectedFreeBytes < 0
                  ? t("{size} short", { size: bytes(-d.projectedFreeBytes) })
                  : t("{size} after queue", {
                      size: bytes(d.projectedFreeBytes),
                    })}
              </p>
            </div>
          </div>
        ))
      ) : (
        <div className="empty compact">
          <Icon name="usb" />
          <h3>{t("No writable storage connected")}</h3>
          <p>{t("Connect your external drive to the PS5, then refresh.")}</p>
        </div>
      )}
      {!!drives.length && (
        <p className="fine">
          {t(
            "Estimates include paused and failed downloads. Other apps can change free space.",
          )}
        </p>
      )}
    </div>
  );
}
