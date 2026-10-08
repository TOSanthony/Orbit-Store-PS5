import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { useOrbit } from "./useOrbit";
import { useController } from "./controller";
import { Backdrop, GameTile, Icon, Modal, StorageRows } from "./components";
import { GameRail, type RailMemory } from "./GameRail";
import { Details } from "./Details";
import { Downloads } from "./Downloads";
import { Library } from "./Library";
import { useLibrary } from "./useLibrary";
import { StorageOverview } from "./LibraryActions";
import { gameState } from "./collection";
import { Select } from "./Select";
import { BrowserNotice } from "./BrowserNotice";
import { UpdateNotice } from "./UpdateNotice";
import { NativeAppNotice } from "./NativeAppNotice";
import { AutoStart } from "./AutoStart";
import { Loader } from "./Loader";
import { Sources } from "./Sources";
import { Pairing } from "./Pairing";
import type { Game, LibraryTarget } from "./types";
import { browseGames, newlyAddedGames, defaultFilters, CATALOG_PAGE_SIZE, type BrowseFilters } from "./browse";
import {
  parseBrowserHandoff,
  handoffGame,
  type BrowserSelection,
} from "./browserHandoff";
import { t, tn, formatNumber } from "./i18n";
export default function App() {
  const orbit = useOrbit();
  const [handoffHash, setHandoffHash] = useState(() => window.location.hash);
  const [handoffReady, setHandoffReady] = useState("");
  const [handoff, setHandoff] = useState<BrowserSelection | null>(null);
  const [handoffError, setHandoffError] = useState("");
  useEffect(() => {
    const changed = () => {
      setHandoffReady("");
      setHandoffHash(window.location.hash);
    };
    window.addEventListener("hashchange", changed);
    return () => window.removeEventListener("hashchange", changed);
  }, []);
  useEffect(() => {
    if (!handoffHash.startsWith("#download?")) return;
    let current = true;
    void orbit.refresh().then(() => {
      if (current) setHandoffReady(handoffHash);
    });
    return () => {
      current = false;
    };
  }, [handoffHash, orbit.refresh]);
  const [tab, setTab] = useState<
      "Discover" | "Browse" | "Library" | "Downloads"
    >("Discover"),
    [gameId, setGameId] = useState<string | null>(() =>
      new URLSearchParams(window.location.search).get("game"),
    ),
    [focusedId, setFocusedId] = useState("");
  useEffect(() => {
    if (
      !handoffHash.startsWith("#download?") ||
      handoffReady !== handoffHash ||
      orbit.error ||
      !orbit.ready ||
      !orbit.system?.paired ||
      !orbit.sources?.acknowledged
    )
      return;
    const selection = parseBrowserHandoff(handoffHash);
    const selected = selection && handoffGame(selection, orbit.games);
    setHandoffError(
      selected
        ? ""
        : t(
            "This download option is no longer available. Check your enabled sources, then choose a game and source in Browse.",
          ),
    );
    setHandoff(selected ? selection : null);
    setGameId(selected?.id || null);
    setTab("Browse");
    // Consumed once; polling and reloads must not override later user choices.
    window.history.replaceState(
      null,
      "",
      window.location.pathname + window.location.search,
    );
    setHandoffHash("");
    setHandoffReady("");
  }, [
    handoffHash,
    handoffReady,
    orbit.error,
    orbit.ready,
    orbit.system?.paired,
    orbit.sources?.acknowledged,
    orbit.games,
  ]);
  const [storageOpen, setStorageOpen] = useState(false),
    [sourcesOpen, setSourcesOpen] = useState(false),
    [autoOpen, setAutoOpen] = useState(false),
    [pairOpen, setPairOpen] = useState(false);
  const [settingsSection, setSettingsSection] = useState<"debrid" | undefined>();
  const settingsReturn = useRef<(() => void) | null>(null);
  const closeSettings = useCallback((restoreSelection = true) => {
    setAutoOpen(false);
    setSettingsSection(undefined);
    const onReturn = settingsReturn.current;
    settingsReturn.current = null;
    if (restoreSelection) onReturn?.();
  }, []);
  const [libraryTarget, setLibraryTarget] = useState<LibraryTarget | null>(
    null,
  );
  const [downloadTarget, setDownloadTarget] = useState<string | null>(null);
  const [heroFavoriteBusy, setHeroFavoriteBusy] = useState(false);
  const [heroFavoriteError, setHeroFavoriteError] = useState("");
  const library = useLibrary(
    !!orbit.system?.paired,
    tab === "Library" || storageOpen,
  );
  const collectionStates = useMemo(
    () =>
      new Map(
        orbit.games.map((g) => [
          g.id,
          gameState(
            g,
            orbit.jobs,
            library.stale ? [] : library.snapshot?.games || [],
          ),
        ]),
      ),
    [orbit.games, orbit.jobs, library.snapshot?.games, library.stale],
  );
  const [filters, setFilters] = useState<BrowseFilters>(defaultFilters);
  const [filtersOpen, setFiltersOpen] = useState(false);
  const query = filters.query;
  const [pageIndex, setPageIndex] = useState(0);
  const [discoverPageIndex, setDiscoverPageIndex] = useState(0);
  const game = orbit.games.find((entry) => entry.id === gameId);
  const detailsOpen = !!game;
  useEffect(() => {
    // Details have denser controls than the storefront. Keep their scale local
    // to this screen, including combobox menus rendered in a portal.
    document.documentElement.classList.toggle("details-open", detailsOpen);
    return () => document.documentElement.classList.remove("details-open");
  }, [detailsOpen]);
  const search = useRef<HTMLInputElement>(null),
    returnTo = useRef("");
  const rails = useRef<Record<string, RailMemory>>({
    latest: { left: 0, focusId: "", height: 0 },
    added: { left: 0, focusId: "", height: 0 },
  });
  const favorites = useMemo(() => new Set(orbit.favorites), [orbit.favorites]);
  function filter<K extends keyof BrowseFilters>(
    key: K,
    value: BrowseFilters[K],
  ) {
    setFilters((current) => ({ ...current, [key]: value }));
    setPageIndex(0);
  }
  function resetFilters() {
    setFilters(defaultFilters);
    setPageIndex(0);
  }
  const back = useCallback(() => {
    if (pairOpen) setPairOpen(false);
    else if (sourcesOpen) setSourcesOpen(false);
    else if (storageOpen) setStorageOpen(false);
    else if (autoOpen) closeSettings();
    else if (game) setGameId(null);
    else if (tab !== "Discover") {
      setTab("Discover");
      requestAnimationFrame(() => {
        const first = document.querySelector<HTMLElement>(".hero .primary");
        first?.focus({ preventScroll: true });
      });
    }
  }, [game, tab, storageOpen, autoOpen, pairOpen, sourcesOpen, closeSettings]);
  const consoleBrowser =
    orbit.system?.platform === "ps5" && !!orbit.system.localSessionAvailable;
  useController(back, consoleBrowser);
  const filtered = useMemo(
    () => browseGames(orbit.games, filters, favorites),
    [orbit.games, filters, favorites],
  );
  const pageCount = Math.max(1, Math.ceil(filtered.length / CATALOG_PAGE_SIZE));
  const currentPage = Math.min(pageIndex, pageCount - 1);
  const visibleGames = filtered.slice(currentPage * CATALOG_PAGE_SIZE, (currentPage + 1) * CATALOG_PAGE_SIZE);
  const discoverGames = useMemo(
    () =>
      browseGames(
        orbit.games,
        { ...defaultFilters, sort: "size-desc" },
        new Set(),
      ),
    [orbit.games],
  );
  const discoverPageCount = Math.max(1, Math.ceil(discoverGames.length / CATALOG_PAGE_SIZE));
  const discoverPage = Math.min(discoverPageIndex, discoverPageCount - 1);
  const discoverVisible = discoverGames.slice(
    discoverPage * CATALOG_PAGE_SIZE,
    (discoverPage + 1) * CATALOG_PAGE_SIZE,
  );
  const latestGames = useMemo(
    () =>
      orbit.games
        .filter(
          (g) =>
            g.releaseDate &&
            g.releaseDate <= new Date().toISOString().slice(0, 10),
        )
        .sort(
          (a, b) =>
            b.releaseDate!.localeCompare(a.releaseDate!) ||
            a.title.localeCompare(b.title),
        )
        .slice(0, 20),
    [orbit.games],
  );
  const newlyAdded = useMemo(() => newlyAddedGames(orbit.games), [orbit.games]);
  const active = orbit.jobs.filter((j) =>
    ["queued", "downloading", "retrying", "verifying"].includes(j.status),
  ).length;
  // Start with the newest release so the hero and highlighted tile agree.
  const focused =
    orbit.games.find((g) => g.id === focusedId) ||
    latestGames[0] ||
    orbit.games[0];
  const firstCovers = window.innerWidth < 700 ? 3 : 6;
  const discover = tab === "Discover" && !game;
  useEffect(() => {
    if (
      orbit.games.length &&
      document.activeElement === document.body &&
      !document.querySelector('[role="dialog"]')
    )
      document
        .querySelector<HTMLElement>(".browse-grid .game-tile, .hero .primary")
        ?.focus({ preventScroll: true });
  }, [orbit.games.length]);
  useEffect(() => {
    if (orbit.ready && gameId && !game) setGameId(null);
  }, [gameId, game, orbit.ready]);
  useEffect(() => {
    // Returning from a game page puts focus back on the tile that opened it.
    if (game || !returnTo.current) return;
    document
      .querySelector<HTMLElement>(returnTo.current)
      ?.focus({ preventScroll: true });
    returnTo.current = "";
  }, [game]);
  const open = useCallback((g: Game) => {
    const row = document.activeElement
      ?.closest("[data-row]")
      ?.getAttribute("data-row");
    returnTo.current = `${row ? `[data-row="${row}"] ` : ""}[data-game="${g.id}"]`;
    setHandoff(null);
    setGameId(g.id);
  }, []);
  function changePage(next: number) {
    setPageIndex(next);
    requestAnimationFrame(() => {
      const tile = document.querySelector<HTMLElement>(
        ".browse-grid .game-tile",
      );
      tile?.focus({ preventScroll: true });
      document
        .querySelector(".page-heading")
        ?.scrollIntoView({ block: "start" });
    });
  }
  function changeDiscoverPage(next: number) {
    setDiscoverPageIndex(next);
    requestAnimationFrame(() => {
      document
        .querySelector<HTMLElement>(".discover-grid .game-tile")
        ?.focus({ preventScroll: true });
      document.getElementById("all-title")?.scrollIntoView({ block: "start" });
    });
  }
  function go(next: typeof tab) {
    setHandoff(null);
    setGameId(null);
    setTab(next);
    setDownloadTarget(null);
    setLibraryTarget(null);
    if (next === "Browse")
      requestAnimationFrame(() => {
        const first = document.querySelector<HTMLElement>(
          ".browse-grid .game-tile",
        );
        (first || search.current)?.focus({ preventScroll: true });
      });
  }
  function openLibrary(target: Omit<LibraryTarget, "request">) {
    go("Library");
    setStorageOpen(false);
    setLibraryTarget({ ...target, request: Date.now() });
  }
  function openDownload(id?: string) {
    go("Downloads");
    setDownloadTarget(id || null);
  }
  function showSearch() {
    go("Browse");
    requestAnimationFrame(() => search.current?.focus());
  }
  function paired(action: () => void) {
    return () => (orbit.system?.paired ? action() : setPairOpen(true));
  }
  return (
    <div
      className={`app screen-${game ? "details" : tab.toLowerCase()} ${discover ? "discover" : ""} ${consoleBrowser ? "console-browser" : ""}`}
      data-game={game?.id}
    >
      <Backdrop
        games={orbit.games}
        active={game?.id || (discover && focused?.id) || ""}
        settle={discover}
        preferPublisherHero={!!game}
      />
      <header className="header">
        {game && (
          <button
            className="top-icon details-back"
            data-label={t("Back")}
            aria-label={t("Back")}
            onClick={back}
          >
            <Icon name="back" />
            <span>{t("Back")}</span>
          </button>
        )}
        <button
          className="brand"
          onClick={() => go("Discover")}
          aria-label={t("Orbit Store home")}
        >
          <img src="/orbit.svg" alt="" />
          Orbit Store
          <span className="beta-label">{t("Beta")}</span>
        </button>
        {/* A game page is its own screen: no tabs or tools, Circle or Back returns. */}
        {!game && (
          <>
            <nav className="main-nav" aria-label={t("Main navigation")}>
              {(["Discover", "Browse", "Library", "Downloads"] as const).map(
                (name) => (
                  <button
                    key={name}
                    className={tab === name ? "active" : ""}
                    aria-current={tab === name ? "page" : undefined}
                    onClick={() => go(name)}
                  >
                    {t(name)}
                    {name === "Downloads" && active > 0 && (
                      <span className="queue-count">{active}</span>
                    )}
                  </button>
                ),
              )}
            </nav>
            <div className="utilities">
              <button
                className="top-icon"
                data-label={t("Search")}
                data-command="search"
                aria-label={t("Search games")}
                onClick={showSearch}
              >
                <Icon name="search" />
              </button>
              <button
                className="top-icon"
                data-label={t("App settings")}
                aria-label={t("App settings")}
                onClick={paired(() => setAutoOpen(true))}
              >
                <Icon name="settings" />
              </button>
            </div>
          </>
        )}
      </header>
      <NativeAppNotice
        version={orbit.system?.version}
        hidden={autoOpen || pairOpen || sourcesOpen || storageOpen}
      />
      <UpdateNotice
        enabled={!!orbit.system?.paired && orbit.system.platform !== "desktop"}
        hidden={autoOpen || pairOpen || sourcesOpen || storageOpen}
        onViewUpdate={() => setAutoOpen(true)}
      />
      {orbit.error && (
        <div className="connection-error" role="status">
          {orbit.error}
          <button onClick={() => location.reload()}>{t("Reconnect")}</button>
        </div>
      )}
      {orbit.system && !orbit.system.stateHealthy && (
        <div className="connection-error" role="alert">
          {t(
            "Orbit cannot save its queue. Check free space on the console’s internal storage.",
          )}
        </div>
      )}
      <BrowserNotice
        session={orbit.browser}
        cancel={orbit.cancelProvider}
        openQueue={openDownload}
        hidden={autoOpen || pairOpen || sourcesOpen || storageOpen}
      />
      <main>
        {handoffError && (
          <div className="notice" role="alert">
            {handoffError}
            <button onClick={() => setHandoffError("")}>{t("Dismiss")}</button>
          </div>
        )}
        {handoffHash.startsWith("#download?") &&
          orbit.ready &&
          !orbit.system?.paired && (
            <div className="notice" role="status">
              {t(
                "Pair with your PS5 to continue with the game selected in the app.",
              )}
              <button onClick={() => setPairOpen(true)}>
                {t("Pair with your PS5")}
              </button>
            </div>
          )}
        {tab !== "Downloads" &&
        tab !== "Library" &&
        orbit.sources &&
        !orbit.sources.acknowledged ? (
          <section className="page source-setup">
            <Sources
              settings={orbit.sources}
              paired={!!orbit.system?.paired}
              pair={() => setPairOpen(true)}
              save={orbit.saveSources}
            />
          </section>
        ) : game ? (
          <Details
            key={`${game.id}:${handoff?.releaseId || ""}:${handoff?.storageId || ""}`}
            game={game}
            back={back}
            initialSelection={handoff?.gameId === game.id ? handoff : undefined}
            drives={orbit.storage}
            system={orbit.system}
            jobs={orbit.jobs}
            pair={() => setPairOpen(true)}
            download={orbit.download}
            browser={orbit.browser}
            openProvider={orbit.openProvider}
            openQueue={openDownload}
            library={library.stale ? [] : library.snapshot?.games || []}
            openLibrary={openLibrary}
            favorite={favorites.has(game.id)}
            setFavorite={(value) => orbit.favorite(game.id, value)}
            configureDebrid={(onReturn) => {
              settingsReturn.current = onReturn;
              setSettingsSection("debrid");
              setAutoOpen(true);
            }}
          />
        ) : tab === "Discover" ? (
          focused ? (
            <>
              <section className="hero" aria-live="polite">
                <p className="hero-eyebrow">{t("Discover")}</p>
                <h1>{focused.title}</h1>
                <p
                  className="tagline"
                  aria-hidden={!focused.tagline || undefined}
                >
                  {focused.tagline}
                </p>
                <div className="hero-actions">
                  <button className="primary" onClick={() => open(focused)}>
                    {t("View game")}
                  </button>
                  <button
                    className="hero-favorite icon-button"
                    aria-label={
                      favorites.has(focused.id)
                        ? t("Remove from favourites")
                        : t("Add to favourites")
                    }
                    aria-pressed={favorites.has(focused.id)}
                    disabled={heroFavoriteBusy}
                    onClick={paired(async () => {
                      setHeroFavoriteBusy(true);
                      setHeroFavoriteError("");
                      try {
                        await orbit.favorite(
                          focused.id,
                          !favorites.has(focused.id),
                        );
                      } catch (error) {
                        setHeroFavoriteError((error as Error).message);
                      } finally {
                        setHeroFavoriteBusy(false);
                      }
                    })}
                  >
                    <Icon name="heart" />
                  </button>
                </div>
                {heroFavoriteError && (
                  <p className="hero-action-error" role="alert">
                    {heroFavoriteError}
                  </p>
                )}
              </section>
              <section
                className="collection"
                data-row="latest"
                aria-labelledby="latest-title"
              >
                <div className="section-heading">
                  <h2 id="latest-title">{t("Latest releases")}</h2>
                </div>
                {latestGames.length ? (
                  <GameRail
                    games={latestGames}
                    focusedId={focused.id}
                    showDate={false}
                    firstCovers={firstCovers}
                    memory={rails.current.latest}
                    states={collectionStates}
                    favorites={favorites}
                    open={open}
                    select={setFocusedId}
                    instantScroll={consoleBrowser}
                  />
                ) : (
                  <p className="muted">
                    {t("Release dates aren’t available for these games yet.")}
                  </p>
                )}
              </section>
              {newlyAdded.length > 0 && (
                <section className="collection" data-row="added" aria-labelledby="added-title">
                  <div className="section-heading">
                    <h2 id="added-title">{t("New on Orbit")}</h2>
                  </div>
                  <GameRail
                    games={newlyAdded}
                    focusedId={focused.id}
                    showDate={false}
                    firstCovers={0}
                    memory={rails.current.added}
                    states={collectionStates}
                    favorites={favorites}
                    open={open}
                    select={setFocusedId}
                    instantScroll={consoleBrowser}
                  />
                </section>
              )}
              <section
                className="collection discover-all"
                aria-labelledby="all-title"
              >
                <div className="section-heading">
                  <h2 id="all-title">{t("All games")}</h2>
                  <span>
                    {tn(discoverGames.length, "{count} game", "{count} games")}
                  </span>
                </div>
                <div className="browse-grid discover-grid" data-row="all">
                  {discoverVisible.map((g) => (
                    <GameTile
                      key={g.id}
                      game={g}
                      status={collectionStates.get(g.id)?.label}
                      favorite={favorites.has(g.id)}
                      open={open}
                      onFocus={setFocusedId}
                    />
                  ))}
                </div>
                {discoverPageCount > 1 && (
                  <nav
                    className="catalog-pages"
                    aria-label={t("Discover catalogue pages")}
                  >
                    <button
                      disabled={discoverPage === 0}
                      onClick={() => changeDiscoverPage(discoverPage - 1)}
                    >
                      {t("Previous")}
                    </button>
                    <span aria-live="polite">
                      {t("Page {page} of {pages}", {
                        page: formatNumber(discoverPage + 1),
                        pages: formatNumber(discoverPageCount),
                      })}
                    </span>
                    <button
                      disabled={discoverPage + 1 === discoverPageCount}
                      onClick={() => changeDiscoverPage(discoverPage + 1)}
                    >
                      {t("Next")}
                    </button>
                  </nav>
                )}
              </section>
            </>
          ) : (
            <div className="empty">
              {orbit.sources?.acknowledged ? (
                <Icon name="sources" />
              ) : (
                <Loader />
              )}
              <h1>
                {orbit.error
                  ? t("Orbit isn’t reachable")
                  : orbit.sources?.acknowledged
                    ? orbit.sources.enabled.length
                      ? t("No games from your selected sources")
                      : t("No sources enabled")
                    : t("Opening Orbit Store…")}
              </h1>
              {orbit.error ? (
                <p>
                  {t(
                    "Check that Orbit is running on your PS5, then reconnect.",
                  )}
                </p>
              ) : (
                orbit.sources?.acknowledged && (
                  <>
                    <p>
                      {orbit.sources.enabled.length
                        ? t(
                            "There are no compatible releases from these sources in this build. Choose another source to browse.",
                          )
                        : t("Choose a download source to see its games.")}
                    </p>
                    <button
                      className="primary"
                      onClick={() => setSourcesOpen(true)}
                    >
                      {t("Choose sources")}
                    </button>
                  </>
                )
              )}
            </div>
          )
        ) : tab === "Browse" ? (
          <section className="page browse">
            <div className="page-heading">
              <h1>{t("Browse")}</h1>
              <span>
                {tn(filtered.length, "{count} game", "{count} games")}
              </span>
              <label className="search-field">
                <Icon name="search" />
                <input
                  ref={search}
                  type="search"
                  placeholder={t("Search games or title ID")}
                  aria-label={t("Search games or title ID")}
                  aria-controls="browse-results"
                  value={query}
                  onChange={(e) => {
                    filter("query", e.target.value);
                  }}
                />
              </label>
            </div>
            <div className="browse-controls">
              <div
                className="collection-tabs"
                role="group"
                aria-label={t("Browse collection")}
              >
                <button
                  aria-pressed={filters.collection === "all"}
                  onClick={() => filter("collection", "all")}
                >
                  {t("All games")}
                </button>
                <button
                  aria-pressed={filters.collection === "favorites"}
                  onClick={() =>
                    orbit.system?.paired
                      ? filter("collection", "favorites")
                      : setPairOpen(true)
                  }
                >
                  {t("Favourites")}
                </button>
              </div>
              <button
                className="filter-toggle"
                aria-expanded={filtersOpen}
                aria-controls="browse-filters"
                onClick={() => setFiltersOpen((value) => !value)}
              >
                {t("Filters and sort")}
                {filters.source ||
                filters.format ||
                filters.region ||
                filters.size ||
                filters.sort !== defaultFilters.sort
                  ? ` · ${t("Applied")}`
                  : ""}{" "}
                {filtersOpen ? "▴" : "▾"}
              </button>
              <div
                id="browse-filters"
                className={`browse-selects ${filtersOpen ? "expanded" : ""}`}
              >
                <Select
                  label={t("Source")}
                  value={filters.source}
                  onChange={(value) => filter("source", value)}
                  options={[
                    { value: "", label: t("All sources") },
                    ...Array.from(
                      new Map(
                        orbit.games.flatMap((g) =>
                          g.releases.map(
                            (r) => [r.sourceId, r.provider] as const,
                          ),
                        ),
                      ),
                    ).map(([value, label]) => ({ value, label })),
                  ]}
                />
                <Select
                  label={t("Format")}
                  value={filters.format}
                  onChange={(value) => filter("format", value)}
                  options={[
                    { value: "", label: t("All formats") },
                    ...Array.from(
                      new Set(
                        orbit.games.flatMap((g) =>
                          g.releases.map((r) => r.format),
                        ),
                      ),
                    ).map((value) => ({ value, label: value })),
                  ]}
                />
                <Select
                  label={t("Region")}
                  value={filters.region}
                  onChange={(value) => filter("region", value)}
                  options={[
                    { value: "", label: t("All regions") },
                    ...["EUR", "USA", "JPN", "ASIA"].map((value) => ({ value, label: value })),
                    { value: "unknown", label: t("Unknown region") },
                  ]}
                />
                <Select
                  label={t("Download size")}
                  value={filters.size}
                  onChange={(value) => filter("size", value)}
                  options={[
                    { value: "", label: t("Any size") },
                    { value: "small", label: t("Under 10 GB") },
                    { value: "medium", label: t("10–50 GB") },
                    { value: "large", label: t("50–100 GB") },
                    { value: "huge", label: t("100 GB or more") },
                  ]}
                />
                <Select
                  label={t("Sort by")}
                  value={filters.sort}
                  onChange={(value) => filter("sort", value)}
                  options={[
                    { value: "release", label: t("Newest first") },
                    { value: "title", label: t("Title A–Z") },
                    { value: "title-desc", label: t("Title Z–A") },
                    { value: "size", label: t("Smallest download") },
                    { value: "size-desc", label: t("Largest download") },
                  ]}
                />
                {(filters.source ||
                  filters.format ||
                  filters.region ||
                  filters.size ||
                  query ||
                  filters.collection !== "all" ||
                  filters.sort !== defaultFilters.sort) && (
                  <button className="reset-filters" onClick={resetFilters}>
                    {t("Reset filters")}
                  </button>
                )}
              </div>
              {(filters.sort === "size" || filters.sort === "size-desc") && (
                <p className="fine">
                  {t(
                    "Sorted by the smallest download option that matches your filters.",
                  )}
                </p>
              )}
            </div>
            {filtered.length ? (
              <>
                <div
                  id="browse-results"
                  className="game-rail browse-grid"
                  data-row="browse"
                >
                  {visibleGames.map((g, index) => (
                    <GameTile
                      key={g.id}
                      game={g}
                      status={collectionStates.get(g.id)?.label}
                      favorite={favorites.has(g.id)}
                      priority={index < firstCovers}
                      open={open}
                    />
                  ))}
                </div>
                {pageCount > 1 && (
                  <nav
                    className="catalog-pages"
                    aria-label={t("Catalogue pages")}
                  >
                    <button
                      disabled={currentPage === 0}
                      onClick={() => changePage(currentPage - 1)}
                    >
                      {t("Previous")}
                    </button>
                    <span aria-live="polite">
                      {t("Page {page} of {pages}", {
                        page: currentPage + 1,
                        pages: pageCount,
                      })}
                    </span>
                    <button
                      disabled={currentPage + 1 === pageCount}
                      onClick={() => changePage(currentPage + 1)}
                    >
                      {t("Next")}
                    </button>
                  </nav>
                )}
              </>
            ) : (
              <div className="empty compact">
                <h2>{t("No games found")}</h2>
                <p>
                  {orbit.games.length
                    ? filters.collection === "favorites"
                      ? t(
                          "Save a game from its details page, or adjust your filters.",
                        )
                      : t("Try another title, title ID or filter.")
                    : t("Your selected sources have no games in this build.")}
                </p>
                <button
                  onClick={() => {
                    if (!orbit.games.length) setSourcesOpen(true);
                    else {
                      resetFilters();
                    }
                  }}
                >
                  {orbit.games.length
                    ? t("Reset filters")
                    : t("Choose sources")}
                </button>
              </div>
            )}
          </section>
        ) : tab === "Library" ? (
          <Library
            model={library}
            target={libraryTarget}
            paired={!!orbit.system?.paired}
            pair={() => setPairOpen(true)}
            drives={orbit.storage}
          />
        ) : !orbit.system?.paired ? (
          <section className="page">
            <h1>{t("Downloads")}</h1>
            <div className="empty">
              <Icon name="download" />
              <h2>{t("See your PS5’s downloads")}</h2>
              <p>{t("Pair this device using the code shown on your PS5.")}</p>
              <button className="primary" onClick={() => setPairOpen(true)}>
                {t("Pair with your PS5")}
              </button>
            </div>
          </section>
        ) : (
          <Downloads
            jobs={orbit.jobs}
            games={orbit.games}
            drives={orbit.storage}
            action={orbit.action}
            clearHistory={orbit.clearHistory}
            browse={() => go("Browse")}
            targetId={downloadTarget}
            openLibrary={openLibrary}
          />
        )}
      </main>
      <footer>
        {orbit.system?.platform === "fixture" && (
          <span>{t("Local test fixture. No console connected.")}</span>
        )}
        <div className="controller-hints">
          <span>
            <b>×</b>
            {!game && tab !== "Downloads" ? t("Open game") : t("Select")}
          </span>
          {!game && tab === "Browse" && (
            <button onClick={showSearch}>
              <b>△</b>
              {t("Search")}
            </button>
          )}
          {!game && (
            <span>
              <kbd>L1</kbd>
              <kbd>R1</kbd>
              {t("Tabs")}
            </span>
          )}
          {game && (
            <button onClick={back}>
              <b>○</b>
              {t("Back")}
            </button>
          )}
        </div>
      </footer>
      {storageOpen && (
        <Modal
          title={t("Storage")}
          close={() => setStorageOpen(false)}
          className="storage-modal"
        >
          <h1>{t("Storage")}</h1>
          <StorageOverview
            feedback
            model={library}
            openGame={(g) =>
              openLibrary({ titleId: g.titleId, path: g.path, title: g.title })
            }
          />
          <h2 className="download-storage-heading">
            {t("Download destinations")}
          </h2>
          <p>
            {t("Downloads save inside the selected drive’s homebrew folder.")}
          </p>
          <StorageRows drives={orbit.storage} />
          <div className="dialog-actions">
            <button
              onClick={() => {
                void orbit.refresh();
              }}
            >
              {t("Refresh storage")}
            </button>
            <button
              onClick={() => {
                setStorageOpen(false);
                setPairOpen(true);
              }}
            >
              {t("Pair another device")}
            </button>
          </div>
        </Modal>
      )}
      {autoOpen && (
        <AutoStart system={orbit.system} close={() => closeSettings()} initialSection={settingsSection}>
          <div className="settings-shortcuts">
            <button
              onClick={() => {
                closeSettings(false);
                setPairOpen(true);
              }}
            >
              {t("Pair devices")}
            </button>
            <button
              onClick={() => {
                closeSettings(false);
                setSourcesOpen(true);
              }}
            >
              {t("Sources")}
            </button>
            <button
              onClick={() => {
                closeSettings(false);
                setStorageOpen(true);
              }}
            >
              {t("Storage")}
            </button>
          </div>
        </AutoStart>
      )}
      {sourcesOpen && orbit.sources && (
        <Modal
          title={t("Download sources")}
          close={() => setSourcesOpen(false)}
          className="sources-modal"
        >
          <Sources
            settings={orbit.sources}
            paired={!!orbit.system?.paired}
            pair={() => {
              setSourcesOpen(false);
              setPairOpen(true);
            }}
            save={orbit.saveSources}
            saved={() => {
              setSourcesOpen(false);
              go("Browse");
            }}
          />
        </Modal>
      )}
      {pairOpen && (
        <Pairing
          system={orbit.system}
          pair={orbit.pair}
          close={() => setPairOpen(false)}
        />
      )}
    </div>
  );
}
