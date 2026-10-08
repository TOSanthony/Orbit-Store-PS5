import { useEffect, useMemo, useRef, useState } from "react";
import { Icon, Modal } from "./components";
import {
  bytes,
  type LibraryGame,
  type LibraryTarget,
  type Storage,
} from "./types";
import type { LibraryModel } from "./useLibrary";
import {
  GameActions,
  LibraryFeedback,
  StorageOverview,
  StorageProgress,
} from "./LibraryActions";
import { Artwork } from "./Artwork";
import { Select } from "./Select";
import { formatDate, placeName, t, tn } from "./i18n";

const emptyGames: LibraryGame[] = [];
const views = ["All games", "Installed", "On drive", "Unavailable"] as const;
type View = (typeof views)[number];
const matches = (game: LibraryGame, view: View) =>
  view === "Installed"
    ? game.installed
    : view === "On drive"
      ? game.onDrive
      : view === "Unavailable"
        ? !game.onDrive
        : true;
const platformName = (game: LibraryGame) =>
  game.platform === "unknown"
    ? t("Platform unknown")
    : game.platform.toUpperCase();
const checkedTime = (value: number) =>
  formatDate(new Date(value * 1000), {
    month: "short",
    day: "numeric",
    hour: "2-digit",
    minute: "2-digit",
  });

export function Library({
  paired,
  pair,
  model,
  target,
  drives,
}: {
  paired: boolean;
  pair: () => void;
  model: LibraryModel;
  target: LibraryTarget | null;
  drives: Storage[];
}) {
  return paired ? (
    <Inventory model={model} target={target} drives={drives} />
  ) : (
    <section className="page library">
      <h1>{t("Library")}</h1>
      <div className="empty">
        <Icon name="sources" />
        <h2>{t("See the games on your PS5")}</h2>
        <p>
          {t(
            "Pair this device to see installed games and games available on your drives.",
          )}
        </p>
        <button className="primary" onClick={pair}>
          {t("Pair with your PS5")}
        </button>
      </div>
    </section>
  );
}
function StatusLabels({ game }: { game: LibraryGame }) {
  return (
    <span className="library-labels">
      <span className={`library-label ${game.installed ? "installed" : ""}`}>
        {game.installed ? t("Installed") : t("Not installed")}
      </span>
      {game.mounted && (
        <span className="library-label mounted">{t("Mounted")}</span>
      )}
      <span
        className={`library-label ${game.onDrive ? "available" : "missing"}`}
      >
        {game.onDrive ? t("On drive") : t("Source missing")}
      </span>
    </span>
  );
}
function Inventory({
  model,
  target,
  drives,
}: {
  model: LibraryModel;
  target: LibraryTarget | null;
  drives: Storage[];
}) {
  const { snapshot, error, refreshAt } = model;
  const [section, setSection] = useState<"games" | "storage">("games");
  const [scan, setScan] = useState(false);
  const [scanError, setScanError] = useState("");
  const [lookup, setLookup] = useState<LibraryTarget | null>(null);
  const consumed = useRef(0);
  const [query, setQuery] = useState("");
  const [view, setView] = useState<View>("All games");
  const [location, setLocation] = useState("");
  const [page, setPage] = useState(0);
  const [selected, setSelected] = useState<string | null>(null);
  const games = snapshot?.games || emptyGames;
  const rows = useMemo(
    () =>
      games
        .filter(
          (g) =>
            matches(g, view) &&
            (!location || g.location === location) &&
            `${g.title} ${g.titleId} ${g.path} ${g.platform}`
              .toLowerCase()
              .includes(query.trim().toLowerCase()),
        )
        .sort(
          (a, b) =>
            a.title.localeCompare(b.title) ||
            a.titleId.localeCompare(b.titleId),
        ),
    [games, view, location, query],
  );
  const pages = Math.max(1, Math.ceil(rows.length / 48));
  const currentPage = Math.min(page, pages - 1);
  const current = games.find((g) => g.titleId === selected);
  const stale = model.stale;
  const busy = (!snapshot && !error) || (!error && !!snapshot?.busy);
  const ready = !!snapshot?.updatedAt;
  useEffect(() => {
    if (!target || consumed.current === target.request) return;
    consumed.current = target.request;
    setSection("games");
    setView("All games");
    setLocation("");
    setPage(0);
    setQuery(target.titleId || "");
    setLookup(target);
    void model.read(true);
  }, [target, model.read]);
  useEffect(() => {
    if (!lookup) return;
    const found = games.find(
      (g) =>
        g.onDrive &&
        (lookup.path ? g.path === lookup.path : g.titleId === lookup.titleId),
    );
    if (found) {
      setSelected(found.titleId);
      setLookup(null);
    }
  }, [games, lookup]);
  useEffect(() => {
    if (selected && !current) setSelected(null);
  }, [selected, current]);
  useEffect(() => {
    // Do not leave a hidden filter active after a drive disappears.
    if (location && !games.some((g) => g.location === location)) {
      setLocation("");
      setPage(0);
    }
  }, [games, location]);
  function reset() {
    setQuery("");
    setView("All games");
    setLocation("");
    setPage(0);
  }
  function changePage(next: number) {
    setPage(next);
    requestAnimationFrame(() =>
      document.querySelector<HTMLElement>(".library-card")?.focus(),
    );
  }
  return (
    <section className="page library">
      <div className="library-heading">
        <div className="page-heading">
          <h1>{t("Library")}</h1>
          <span>{tn(rows.length, "{count} game", "{count} games")}</span>
        </div>
        <div className="library-heading-actions">
          <button
            onClick={() =>
              setSection(section === "games" ? "storage" : "games")
            }
          >
            <Icon name="storage" />
            {section === "games" ? t("Storage") : t("Games")}
          </button>
          {(!ready || stale || section === "storage") && (
            <button
              onClick={() => void model.read(true)}
              disabled={busy || Date.now() < refreshAt}
            >
              {busy ? t("Refreshing…") : t("Refresh library")}
            </button>
          )}
          {model.can("rescan") && (
            <button
              disabled={model.working}
              onClick={() => {
                setScan(true);
                setScanError("");
              }}
            >
              <Icon name="scan" />
              {t("Scan for games")}
            </button>
          )}
        </div>
      </div>
      {stale && (
        <p className="library-sync" role="status">
          {ready
            ? t("Saved snapshot · Last refreshed {time}", {
                time: checkedTime(snapshot.updatedAt),
              })
            : t("Saved snapshot · Not refreshed yet")}
        </p>
      )}
      {(error || snapshot?.message) && (
        <div className="notice library-notice" role="status">
          <strong>{error || t(snapshot?.message || "")}</strong>
          {ready && (
            <p>
              {t(
                "Showing the last successful result. Game and drive status may have changed.",
              )}
            </p>
          )}
        </div>
      )}
      <LibraryFeedback model={model} />
      {lookup && (
        <div className="notice" role="status">
          <strong>{t("Looking for {title}", { title: lookup.title })}</strong>
          <p>
            {t(
              "The completed file is not in the current Library inventory. Check that its drive is connected, then scan for games. A completed download alone does not confirm installation.",
            )}
          </p>
          <button onClick={() => setLookup(null)}>{t("Dismiss")}</button>
        </div>
      )}
      {!ready ? (
        <div className="empty compact">
          <Icon name="sources" />
          <h2>{busy ? t("Finding your games") : t("Connect your library")}</h2>
          <p>
            {busy
              ? t(
                  "Reading installed titles and drive availability from ShadowMount.",
                )
              : t(
                  "Library uses ShadowMount’s local games API on your PS5. Start a version with that API enabled, then refresh here.",
                )}
          </p>
          {!busy && (
            <p className="fine">
              {t(
                "Games are not inferred from download history. An unavailable library does not mean your games are missing.",
              )}
            </p>
          )}
        </div>
      ) : (
        <>
          {section === "storage" ? (
            <StorageOverview
              model={model}
              openGame={(g) => setSelected(g.titleId)}
            />
          ) : (
            <>
              <StorageProgress model={model} />
              <div className="library-toolbar">
                <div
                  className="library-tabs"
                  role="group"
                  aria-label={t("Library views")}
                >
                  {views.map((v) => (
                    <button
                      key={v}
                      aria-pressed={view === v}
                      onClick={() => {
                        setView(v);
                        setPage(0);
                      }}
                    >
                      {t(v)}
                    </button>
                  ))}
                </div>
                <div className="library-filters">
                  <label className="search-field">
                    <Icon name="search" />
                    <input
                      aria-label={t("Search library")}
                      value={query}
                      onChange={(e) => {
                        setQuery(e.target.value);
                        setPage(0);
                      }}
                      placeholder={t("Search your library")}
                    />
                  </label>
                  <Select
                    label={t("Location")}
                    value={location}
                    onChange={(value) => {
                      setLocation(value);
                      setPage(0);
                    }}
                    options={[
                      { value: "", label: t("All locations") },
                      ...[...new Set(games.map((g) => g.location))]
                        .sort()
                        .map((value) => ({ value, label: placeName(value) })),
                    ]}
                  />
                </div>
              </div>
              {rows.length ? (
                <div className="library-grid">
                  {rows
                    .slice(currentPage * 48, (currentPage + 1) * 48)
                    .map((g) => (
                      <button
                        key={g.titleId}
                        className={`library-card ${!g.onDrive ? "unavailable" : ""}`}
                        onClick={() => setSelected(g.titleId)}
                        aria-label={t("View library details for {title}", {
                          title: g.title,
                        })}
                      >
                        <span className="library-cover">
                          <Artwork
                            key={g.cover || "mark"}
                            src={g.cover || "/orbit.svg"}
                            fallbackSrc={g.coverFallback}
                            alt=""
                            decoding="async"
                            referrerPolicy="no-referrer"
                          />
                        </span>
                        <span className="library-card-content">
                          <span className="library-title">{g.title}</span>
                          <span className="library-status-line">
                            {!g.onDrive
                              ? t("Unavailable")
                              : g.mounted
                                ? t("Mounted")
                                : g.installed
                                  ? t("Installed")
                                  : t("On drive")}{" "}
                            ·{" "}
                            {g.onDrive
                              ? placeName(g.location)
                              : t("Drive disconnected")}
                          </span>
                        </span>
                      </button>
                    ))}
                </div>
              ) : (
                <div className="empty compact">
                  <h2>
                    {games.length
                      ? t("No matching games")
                      : t("No games reported yet")}
                  </h2>
                  <p>
                    {games.length
                      ? t("Try a different title, status or location.")
                      : t(
                          "ShadowMount has not reported any installed or on-drive games. Refresh after its inventory updates.",
                        )}
                  </p>
                  {games.length > 0 && (
                    <button onClick={reset}>
                      {t("Reset library filters")}
                    </button>
                  )}
                </div>
              )}
              {pages > 1 && (
                <nav className="catalog-pages" aria-label={t("Library pages")}>
                  <button
                    disabled={currentPage === 0}
                    onClick={() => changePage(currentPage - 1)}
                  >
                    {t("Previous")}
                  </button>
                  <span aria-live="polite">
                    {t("Page {page} of {pages}", {
                      page: currentPage + 1,
                      pages,
                    })}
                  </span>
                  <button
                    disabled={currentPage + 1 === pages}
                    onClick={() => changePage(currentPage + 1)}
                  >
                    {t("Next")}
                  </button>
                </nav>
              )}
              <div
                className="library-capacity"
                aria-label={t("Available storage")}
              >
                {drives.map((drive) => (
                  <span key={drive.id}>
                    <Icon name={drive.id === "internal" ? "storage" : "usb"} />
                    {placeName(drive.label)}{" "}
                    <span>
                      {t("{size} free", { size: bytes(drive.freeBytes) })}
                    </span>
                  </span>
                ))}
              </div>
            </>
          )}
        </>
      )}
      {scan && (
        <Modal
          title={t("Scan for games")}
          close={() => !model.actionBusy && setScan(false)}
        >
          <h1>{t("Scan your drives?")}</h1>
          <p>
            {t(
              "ShadowMount will search its configured locations for games. It may register games on the console or mount detected sources. If a game is running, the scan may wait until it is safe.",
            )}
          </p>
          <div className="dialog-actions">
            <button
              className="primary"
              disabled={model.working}
              onClick={() => {
                void model
                  .run("scan", { confirmed: true })
                  .then(() => setScan(false))
                  .catch((e) => setScanError((e as Error).message));
              }}
            >
              {t("Start scan")}
            </button>
            <button disabled={model.actionBusy} onClick={() => setScan(false)}>
              {t("Back")}
            </button>
          </div>
          {scanError && (
            <p className="form-error" role="alert">
              {scanError}
            </p>
          )}
        </Modal>
      )}
      {current && (
        <Modal
          title={t("Library game details")}
          close={() => setSelected(null)}
          className="library-details"
        >
          <p className="library-eyebrow">
            {platformName(current)} · {current.titleId}
          </p>
          <h1>{current.title}</h1>
          <StatusLabels game={current} />
          {stale && (
            <p className="notice">
              {t(
                "Saved snapshot. Refresh the library before relying on these statuses.",
              )}
            </p>
          )}
          <GameActions game={current} model={model} />
          <dl className="library-facts">
            <div>
              <dt>{t("Installation")}</dt>
              <dd>
                {current.installed
                  ? t("Registered on the console")
                  : t("Not installed on the console")}
              </dd>
            </div>
            <div>
              <dt>{t("Mount status")}</dt>
              <dd>{current.mounted ? t("Mounted") : t("Not mounted")}</dd>
            </div>
            <div>
              <dt>{t("Source")}</dt>
              <dd>
                {current.onDrive
                  ? t("Available on drive")
                  : t("Unavailable; the drive or source may be missing")}
              </dd>
            </div>
            <div>
              <dt>{t("Format")}</dt>
              <dd>{current.format}</dd>
            </div>
            <div>
              <dt>{t("Location")}</dt>
              <dd>{placeName(current.location)}</dd>
            </div>
            <div>
              <dt>{t("Source path")}</dt>
              <dd className="library-path">
                {current.path || t("Not reported")}
              </dd>
            </div>
            {current.runtimePath && current.runtimePath !== current.path && (
              <div>
                <dt>{t("Game path")}</dt>
                <dd className="library-path">{current.runtimePath}</dd>
              </div>
            )}
            <div>
              <dt>{t("Source size")}</dt>
              <dd>
                {current.sizeBytes !== null
                  ? bytes(current.sizeBytes)
                  : current.sizeStatus === "unavailable"
                    ? t("Unavailable")
                    : t("Use Measure game sizes in Storage")}
              </dd>
            </div>
            <div>
              <dt>{t("Size reported by console")}</dt>
              <dd>
                {current.installedSizeBytes === null
                  ? t("Not reported")
                  : bytes(current.installedSizeBytes)}
              </dd>
            </div>
          </dl>
          <p className="fine">
            {t(
              "Installed means registered on the console. Mounted means its source is currently mounted. On drive means the source is available. These statuses do not confirm that a game will launch.",
            )}
          </p>
        </Modal>
      )}
    </section>
  );
}
