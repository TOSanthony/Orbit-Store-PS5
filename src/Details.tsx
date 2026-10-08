import { useEffect, useRef, useState } from "react";
import { Icon } from "./components";
import { GameSheet, PadSymbol, type SheetView } from "./GameSheet";
import { useDebrid, torboxAvailable } from "./Debrid";
import {
  bytes,
  type Game,
  type BrowserSession,
  type Job,
  type Storage,
  type System,
  type LibraryGame,
  type LibraryTarget,
} from "./types";
import { downloadSpacePlan, releaseState } from "./collection";
import type { BrowserSelection } from "./browserHandoff";
import { formatDate, genreName, placeName, t } from "./i18n";
import { rich } from "./Rich";
import "./details.css";
export function Details({
  initialSelection,
  back,
  game,
  drives,
  system,
  jobs,
  pair,
  download,
  openQueue,
  browser,
  openProvider,
  favorite,
  setFavorite,
  configureDebrid,
  library,
  openLibrary,
}: {
  initialSelection?: BrowserSelection;
  back: () => void;
  game: Game;
  drives: Storage[];
  system: System | null;
  jobs: Job[];
  pair: () => void;
  download: (
    id: string,
    storage: string,
    delivery?: "direct" | "torbox",
  ) => Promise<void>;
  openQueue: (id?: string) => void;
  browser: BrowserSession | null;
  openProvider: (releaseId: string, storageId: string) => Promise<void>;
  favorite: boolean;
  setFavorite: (value: boolean) => Promise<void>;
  configureDebrid: (onReturn: () => void) => void;
  library: LibraryGame[];
  openLibrary: (target: Omit<LibraryTarget, "request">) => void;
}) {
  const preferred =
    drives.find((d) => d.id === system?.preferredStorage) ||
    drives.find((d) => d.external) ||
    drives[0];
  const [destination, setDestination] = useState(
      initialSelection?.storageId || preferred?.id || "",
    ),
    [releaseId, setReleaseId] = useState(
      initialSelection?.releaseId || game.releases[0].id,
    ),
    [pending, setPending] = useState(false),
    [error, setError] = useState("");
  const [downloadAgain, setDownloadAgain] = useState(false);
  const [sheet, setSheet] = useState<SheetView | null>(
    initialSelection ? "downloads" : null,
  );
  const page = useRef<HTMLElement>(null);
  const tagline = game.tagline?.trim();
  const description = game.description?.trim();
  const debrid = useDebrid(!!system?.paired);
  const [delivery, setDelivery] = useState<"direct" | "torbox">("direct");
  // A source can be switched off by another paired device while this page is open.
  const release =
      game.releases.find((r) => r.id === releaseId) || game.releases[0],
    drive = drives.find((d) => d.id === (destination || preferred?.id)),
    // Jobs are per option and drive. A finished one can be downloaded again once its file is gone.
    existing = jobs.find(
      (job) => job.releaseId === release.id && job.storageId === drive?.id,
    ),
    space = drive ? downloadSpacePlan(release, drive, jobs) : null,
    planned = space?.planned,
    afterDownload = space?.after ?? 0,
    enough = space?.enough;
  const torbox = delivery === "torbox";
  const canTorbox = torboxAvailable(debrid.status, release);
  const useBrowser = !!release.browserAvailable && !torbox;
  const collection = releaseState(release, jobs, library);
  const libraryGame = collection?.libraryGame;
  const showDownload =
    downloadAgain || !collection || collection.kind === "related";
  const downloadUnavailable =
    !game.releases.some((option) => option.id === releaseId) ||
    pending ||
    (torbox && !canTorbox) ||
    !!browser?.active ||
    !enough ||
    !(
      system?.platform === "ps5" ||
      (system?.platform === "fixture" && release.id === "ppsa04029-ffpfsc")
    );
  useEffect(() => {
    setError("");
    setDownloadAgain(false);
    setDelivery("direct");
  }, [releaseId]);
  useEffect(() => {
    window.scrollTo(0, 0);
    if (initialSelection) return;
    page.current
      ?.querySelector<HTMLElement>(".hub-primary")
      ?.focus({ preventScroll: true });
  }, [game.id, initialSelection]);
  async function start() {
    setPending(true);
    setError("");
    try {
      if (!drive) return;
      await download(release.id, drive.id, delivery);
      openQueue();
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setPending(false);
    }
  }
  async function verifyProvider() {
    if (!drive) return;
    setPending(true);
    setError("");
    try {
      await openProvider(release.id, drive.id);
      setSheet(null);
      requestAnimationFrame(() => {
        document
          .querySelector<HTMLButtonElement>(
            ".provider-browser-notice button:not([disabled])",
          )
          ?.focus();
      });
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setPending(false);
    }
  }
  function chooseRelease(id: string) {
    setReleaseId(id);
    setSheet("downloads");
  }
  function toggleFavorite() {
    if (!system?.paired) {
      pair();
      return;
    }
    setPending(true);
    setError("");
    void setFavorite(!favorite)
      .catch((e: Error) => setError(e.message))
      .finally(() => setPending(false));
  }
  function viewLibrary() {
    if (libraryGame)
      openLibrary({
        titleId: libraryGame.titleId,
        path: libraryGame.path,
        title: libraryGame.title,
      });
  }
  function primaryAction(review = false) {
    if (!system?.paired) {
      setSheet(null);
      pair();
    } else if (review) setSheet("downloads");
    else if (!downloadAgain && collection?.job) openQueue(collection.job.id);
    else if (!downloadAgain && collection?.kind === "library" && libraryGame)
      viewLibrary();
    else void (useBrowser ? verifyProvider() : start());
  }
  const actionLabel = !system?.paired
    ? t("Pair with your PS5")
    : !downloadAgain && collection?.job
      ? t("View download")
      : !downloadAgain && collection?.kind === "library" && libraryGame
        ? t("View in Library")
        : pending
          ? t("Please wait…")
          : torbox
            ? t("Download via TorBox")
            : useBrowser
              ? t("Open download page on PS5")
              : existing || downloadAgain
                ? t("Download again")
                : t("Download to PS5");
  const methodLabel = torbox
    ? "TorBox"
    : t("{provider} directly", { provider: release.provider });
  const methodNote = torbox
    ? canTorbox
      ? t(
          "Uses your TorBox account. Preparation may take time; follow it in Downloads.",
        )
      : t(
          "TorBox is unavailable for this option. Choose the original source or check App settings.",
        )
    : !canTorbox
      ? t(
          "TorBox is unavailable for this host or file size. Refresh TorBox in App settings to check again.",
        )
      : t("TorBox is available for this option.");
  const instructions = (
    <div
      className="provider-browser-option"
      id="provider-browser-steps"
      tabIndex={0}
      data-controller-scroll
      data-initial-focus
      role="region"
      aria-label={t("Vikingfile: start here")}
    >
      <strong>{t("Vikingfile: start here")}</strong>
      <ol>
        <li>
          {rich("First, choose your drive and select {button}.", {
            button: <strong>{t("Open download page on PS5")}</strong>,
          })}
        </li>
        <li>
          {rich(
            "On Vikingfile, complete any verification and select the site’s {button} button.",
            { button: <strong>{t("Download")}</strong> },
          )}
        </li>
        <li>
          {rich(
            "Return to {app}. Open {downloads} to follow progress once Orbit has checked the file.",
            {
              app: <strong>Orbit Store</strong>,
              downloads: <strong>{t("Downloads")}</strong>,
            },
          )}
        </li>
      </ol>
      {!browser?.available && (
        <p>{t("Connect to Orbit running on your PS5 to use this option.")}</p>
      )}
    </div>
  );
  const storageBudget = drive && (
    <dl className="sheet-budget" aria-label={t("Download storage estimate")}>
      <div>
        <dt>{t("Free now")}</dt>
        <dd>{bytes(drive.freeBytes)}</dd>
      </div>
      <div>
        <dt>{t("Unfinished downloads")}</dt>
        <dd>{bytes(drive.pendingBytes)}</dd>
      </div>
      <div>
        <dt>
          {planned ? t("After your queue") : t("After queue + this download")}
        </dt>
        <dd>
          {afterDownload < 0
            ? t("{size} short", { size: bytes(-afterDownload) })
            : bytes(afterDownload)}
        </dd>
      </div>
    </dl>
  );
  const downloadSettings = (
    <div className="sheet-settings">
      <button
        className="sheet-setting"
        data-setting="source"
        data-initial-focus
        disabled={pending || browser?.active}
        onClick={() => setSheet("source")}
      >
        <span className="sheet-setting-icon">
          <Icon name="sources" />
        </span>
        <span>
          <small>{t("Source")}</small>
          <strong>
            {release.provider} · {release.format}
          </strong>
          <small>
            {release.version &&
              `${t("version {version}", { version: release.version })} · `}
            {bytes(release.sizeBytes)}
          </small>
        </span>
        <Icon name="chevron" />
      </button>
      {showDownload && (
        <>
          <button
            className="sheet-setting"
            data-setting="delivery"
            disabled={pending || browser?.active}
            onClick={() => setSheet("delivery")}
          >
            <span className="sheet-setting-icon">
              <Icon name="download" />
            </span>
            <span>
              <small>{t("Download using")}</small>
              <strong>{methodLabel}</strong>
              {!torbox && canTorbox && <small>{t("TorBox available")}</small>}
            </span>
            <Icon name="chevron" />
          </button>
          {system?.paired && (
            <button
              className="sheet-setting"
              data-setting="storage"
              disabled={pending || browser?.active}
              onClick={() => setSheet("storage")}
            >
              <span className="sheet-setting-icon">
                <Icon name="usb" />
              </span>
              <span>
                <small>{t("Save to")}</small>
                <strong>
                  {drive ? placeName(drive.label) : t("Select storage")}
                </strong>
                {drive && (
                  <small>
                    {t("{size} free", {
                      size: bytes(drive.freeBytes),
                    })}
                  </small>
                )}
              </span>
              <Icon name="chevron" />
            </button>
          )}
        </>
      )}
    </div>
  );
  return (
    <section
      ref={page}
      className="product native-game"
      aria-labelledby="product-title"
    >
      <div className="hub-content" inert={sheet ? true : undefined}>
        <div className="hub-heading">
          <h1
            id="product-title"
            className={game.title.length > 34 ? "long-title" : undefined}
          >
            {game.title}
          </h1>
          {tagline && <p className="hub-tagline">{tagline}</p>}
          {description && <p className="hub-description">{description}</p>}
          <p className="hub-release">
            {release.provider}
            <span>·</span>
            {release.format}
            <span>·</span>
            {bytes(release.sizeBytes)}
            <span>·</span>
            <span>PS5</span>
            {game.genre && (
              <>
                <span>·</span>
                <span>{genreName(game.genre)}</span>
              </>
            )}
          </p>
        </div>
        <div className="hub-launch">
          <div className="hub-actions">
            <button
              className="primary hub-primary"
              data-long-label={actionLabel.length > 34 || undefined}
              disabled={pending}
              onClick={() => primaryAction(true)}
              aria-haspopup={
                showDownload && system?.paired ? "dialog" : undefined
              }
            >
              <Icon name="download" />
              <span>{actionLabel}</span>
            </button>
            <button
              className="hub-round"
              data-command="favorite"
              aria-label={
                favorite ? t("Saved to favourites") : t("Add to favourites")
              }
              aria-pressed={favorite}
              disabled={pending}
              onClick={toggleFavorite}
            >
              <Icon name="heart" />
            </button>
            <button
              className="hub-round"
              data-command="details"
              aria-label={t("About this game")}
              aria-haspopup="dialog"
              onClick={() => setSheet("about")}
            >
              <Icon name="more" />
            </button>
          </div>
          {!sheet && error && (
            <p className="form-error" role="alert">
              {error}
            </p>
          )}
        </div>
        <div className="game-pad-hints" aria-label={t("Controls")}>
          <span>
            <PadSymbol name="cross" />
            {t("Select")}
          </span>
          <span>
            <PadSymbol name="circle" />
            {t("Back")}
          </span>
          <span>
            <PadSymbol name="triangle" />
            {t("Favourite")}
          </span>
          <span>
            <PadSymbol name="square" />
            {t("Details")}
          </span>
        </div>
      </div>
      {sheet && (
        <GameSheet
          view={sheet}
          title={
            sheet === "about"
              ? t("About this game")
              : sheet === "storage"
                ? t("Save to")
                : sheet === "source"
                  ? t("Source")
                  : sheet === "delivery"
                    ? t("Download using")
                    : t("Download options")
          }
          close={() => setSheet(null)}
          back={() =>
            setSheet(
              sheet === "downloads" || sheet === "about"
                ? null
                : "downloads",
            )
          }
        >
          {sheet === "downloads" && downloadSettings}
          <div className="sheet-scroll" data-scroll-region key={sheet}>
            {sheet === "about" ? (
              <div
                className="game-about"
                tabIndex={0}
                data-controller-scroll
                data-initial-focus
              >
                <p className="about-title">{game.title}</p>
                {game.description && (
                  <p className="description">{game.description}</p>
                )}
                {(game.publisher || game.releaseDate) && (
                  <dl className="facts game-facts">
                    {game.publisher && (
                      <div>
                        <dt>{t("Publisher")}</dt>
                        <dd>{game.publisher}</dd>
                      </div>
                    )}
                    {game.releaseDate && (
                      <div>
                        <dt>{t("PS5 release")}</dt>
                        <dd>
                          {formatDate(new Date(game.releaseDate), {
                            dateStyle: "medium",
                            timeZone: "UTC",
                          })}
                        </dd>
                      </div>
                    )}
                  </dl>
                )}
                <dl className="facts release-facts">
                  <div>
                    <dt>{t("Format")}</dt>
                    <dd>{release.format}</dd>
                  </div>
                  <div>
                    <dt>{t("Title ID")}</dt>
                    <dd>{release.titleId}</dd>
                  </div>
                  <div>
                    <dt>{t("Source")}</dt>
                    <dd>{release.provider}</dd>
                  </div>
                  {release.version && (
                    <div>
                      <dt>{t("Version")}</dt>
                      <dd>{release.version}</dd>
                    </div>
                  )}
                  <div>
                    <dt>{t("Download size")}</dt>
                    <dd>{bytes(release.sizeBytes)}</dd>
                  </div>
                </dl>
              </div>
            ) : sheet === "source" ? (
              <div className="sheet-choices">
                <p className="sheet-note">
                  {game.releases.length > 1
                    ? t("Choose a source and format for this game.")
                    : t("Available from your enabled sources.")}
                </p>
                {game.releases.map((option) => {
                  const presence = releaseState(option, jobs, library);
                  return (
                    <button
                      key={option.id}
                      className="sheet-choice release-option"
                      aria-pressed={release.id === option.id}
                      data-initial-focus={release.id === option.id || undefined}
                      disabled={pending || browser?.active}
                      onClick={() => chooseRelease(option.id)}
                    >
                      <span className="sheet-choice-check">
                        {release.id === option.id && <Icon name="check" />}
                      </span>
                      <span>
                        <strong>
                          {option.provider} · {option.format}
                        </strong>
                        <small>
                          {option.titleId}
                          {option.version &&
                            ` · ${t("version {version}", { version: option.version })}`}
                        </small>
                        <small>
                          {bytes(option.sizeBytes)}
                          {presence && ` · ${t(presence.label)}`}
                        </small>
                      </span>
                    </button>
                  );
                })}
              </div>
            ) : sheet === "delivery" ? (
              <div className="sheet-choices">
                <button
                  className="sheet-choice delivery-choice"
                  aria-pressed={!torbox}
                  data-delivery="direct"
                  data-initial-focus={!torbox || undefined}
                  disabled={pending || browser?.active}
                  onClick={() => {
                    setDelivery("direct");
                    setSheet("downloads");
                  }}
                >
                  <span className="sheet-choice-check">
                    {!torbox && <Icon name="check" />}
                  </span>
                  <strong>
                    {t("{provider} directly", { provider: release.provider })}
                  </strong>
                </button>
                {!debrid.status?.connected && !torbox ? (
                  <button
                    className="sheet-choice delivery-choice"
                    data-delivery="setup-torbox"
                    disabled={pending || browser?.active}
                    onClick={() => {
                      setSheet(null);
                      configureDebrid(() => {
                        void debrid.refresh();
                        setSheet("delivery");
                      });
                    }}
                  >
                    <span className="sheet-choice-check"><Icon name="settings" /></span>
                    <span>
                      <strong>TorBox</strong>
                      <small>{t("Connect your TorBox account in App settings.")}</small>
                      <small>{t("Set up TorBox")}</small>
                    </span>
                  </button>
                ) : (
                  <button
                    className="sheet-choice delivery-choice"
                    aria-pressed={torbox}
                    data-delivery="torbox"
                    data-initial-focus={torbox || undefined}
                    disabled={pending || browser?.active || !canTorbox}
                    onClick={() => {
                      setDelivery("torbox");
                      setSheet("downloads");
                    }}
                  >
                    <span className="sheet-choice-check">
                      {torbox && <Icon name="check" />}
                    </span>
                    <strong>TorBox</strong>
                  </button>
                )}
                {(debrid.status?.connected || torbox) && (
                  <p className="sheet-note">{methodNote}</p>
                )}
              </div>
            ) : sheet === "storage" ? (
              <div className="sheet-choices">
                {drives.length ? (
                  drives.map((option) => (
                    <button
                      key={option.id}
                      className="sheet-choice"
                      aria-pressed={drive?.id === option.id}
                      data-initial-focus={drive?.id === option.id || undefined}
                      disabled={pending || browser?.active}
                      onClick={() => {
                        setDestination(option.id);
                        setSheet("downloads");
                      }}
                    >
                      <span className="sheet-choice-check">
                        {drive?.id === option.id && <Icon name="check" />}
                      </span>
                      <span>
                        <strong>{placeName(option.label)}</strong>
                        <small>
                          {t("{size} free", { size: bytes(option.freeBytes) })}
                        </small>
                        <small>
                          {t("Saves to {path}", { path: option.path })}
                        </small>
                      </span>
                    </button>
                  ))
                ) : (
                  <p className="sheet-note">
                    {t("Connect a writable drive to your PS5.")}
                  </p>
                )}
                {storageBudget}
                {drive && (
                  <meter
                    className="sheet-meter"
                    min={0}
                    max={Math.max(1, drive.totalBytes)}
                    value={Math.max(0, drive.totalBytes - drive.freeBytes)}
                    aria-label={t("Storage used")}
                  />
                )}
                <p className="sheet-note">
                  {t(
                    "Estimates include paused and failed downloads. Other apps can change free space.",
                  )}
                </p>
              </div>
            ) : (
              <>
                {initialSelection && (
                  <p className="notice">
                    {t(
                      "Your game and source are selected from the app. Check the save location, then open the download page below.",
                    )}
                  </p>
                )}
                {!game.releases.some((option) => option.id === releaseId) && (
                  <p className="form-error" role="alert">
                    {t(
                      "The selected source is no longer available. Choose another download option to continue.",
                    )}
                  </p>
                )}
                {destination && !drive && system?.paired && (
                  <p className="form-error" role="alert">
                    {t(
                      "Your selected drive is unavailable. Connect it or choose a save location to continue.",
                    )}
                  </p>
                )}

                {showDownload && useBrowser && instructions}
                {showDownload &&
                  (torbox || (debrid.status?.connected && !canTorbox)) && (
                    <p className="sheet-note">{methodNote}</p>
                  )}
                {system?.paired && (
                  <>
                    {libraryGame && (
                      <p className="collection-presence">
                        {collection?.kind === "related"
                          ? t(
                              "A related copy is in Library. Its format, filename or version does not confirm a match for this option.",
                            )
                          : t("In your Library on {location}.", {
                              location: placeName(libraryGame.location),
                            })}
                        {collection?.kind === "related" && (
                          <button
                            onClick={() =>
                              openLibrary({
                                titleId: libraryGame.titleId,
                                path: libraryGame.path,
                                title: libraryGame.title,
                              })
                            }
                          >
                            {t("View related copy")}
                          </button>
                        )}
                      </p>
                    )}
                    {!downloadAgain &&
                      (collection?.kind === "library" ||
                        collection?.job?.status === "complete") && (
                        <button
                          className="download-another"
                          onClick={() => setDownloadAgain(true)}
                        >
                          {t("Download another copy")}
                        </button>
                      )}
                    {collection?.job?.status === "complete" && (
                      <p className="fine">
                        {t(
                          "Download history records a completed transfer. Find it in Library to check its current location.",
                        )}
                      </p>
                    )}
                    {showDownload &&
                      system?.paired &&
                      !torbox &&
                      release.browserAvailable &&
                      release.directAvailable !== false && (
                        <div className="provider-direct-option">
                          <button
                            onClick={() => void start()}
                            disabled={downloadUnavailable}
                          >
                            {t("Download directly")}
                          </button>
                        </div>
                      )}
                    {showDownload &&
                      system.platform !== "desktop" &&
                      (!drive ? (
                        <p className="notice">
                          {t("Connect a writable drive to your PS5.")}
                        </p>
                      ) : (
                        !enough && (
                          <p className="notice">
                            {t(
                              "Not enough space after unfinished downloads. Free space or cancel an item in Downloads.",
                            )}
                          </p>
                        )
                      ))}
                  </>
                )}
              </>
            )}
            {error && sheet !== "downloads" && (
              <p className="form-error" role="alert">
                {error}
              </p>
            )}
          </div>
          {sheet === "downloads" && (
            <div className="sheet-confirm">
              {error && (
                <p className="form-error" role="alert">
                  {error}
                </p>
              )}
              {system?.paired && showDownload && drive && (
                <div className="sheet-estimate">
                  <span>
                    {planned
                      ? t("After your queue")
                      : t("After queue + this download")}
                  </span>
                  <strong>
                    {afterDownload < 0
                      ? t("{size} short", { size: bytes(-afterDownload) })
                      : bytes(afterDownload)}
                  </strong>
                </div>
              )}
              <button
                className="primary sheet-primary"
                data-long-label={actionLabel.length > 34 || undefined}
                disabled={
                  system?.paired && showDownload
                    ? downloadUnavailable || (useBrowser && !browser?.available)
                    : pending
                }
                onClick={() => primaryAction()}
                aria-describedby={
                  showDownload && useBrowser
                    ? "provider-browser-steps"
                    : undefined
                }
              >
                {actionLabel}
              </button>
              <p className="sheet-footnote">
                {t(
                  "Saves a file to your console. Installation and launching are separate.",
                )}
              </p>
            </div>
          )}
        </GameSheet>
      )}
    </section>
  );
}
