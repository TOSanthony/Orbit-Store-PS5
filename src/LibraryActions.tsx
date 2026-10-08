import { useEffect, useState } from "react";
import { Icon } from "./components";
import { Select } from "./Select";
import { bytes, type LibraryGame } from "./types";
import type { LibraryModel } from "./useLibrary";
import { formatNumber, placeName, t, tn } from "./i18n";

// Whole sentences per operation, so each language can phrase them its own way.
const outcomes = {
  delete: {
    operation: /* i18n */ "Delete",
    completed: /* i18n */ "Deleted {title}",
    cancelled: /* i18n */ "Delete cancelled: {title}",
    failed: /* i18n */ "Delete failed: {title}",
  },
  move: {
    operation: /* i18n */ "Move",
    completed: /* i18n */ "Moved {title}",
    cancelled: /* i18n */ "Move cancelled: {title}",
    failed: /* i18n */ "Move failed: {title}",
  },
  copy: {
    operation: /* i18n */ "Copy",
    completed: /* i18n */ "Copied {title}",
    cancelled: /* i18n */ "Copy cancelled: {title}",
    failed: /* i18n */ "Copy failed: {title}",
  },
  other: {
    operation: /* i18n */ "Storage operation",
    completed: /* i18n */ "Completed {title}",
    cancelled: /* i18n */ "Storage operation cancelled: {title}",
    failed: /* i18n */ "Storage operation failed: {title}",
  },
};
const percent = (value: number) =>
  formatNumber(value / 100, {
    style: "percent",
    minimumFractionDigits: 1,
    maximumFractionDigits: 1,
  });

export function LibraryFeedback({
  model,
  titleId,
}: {
  model: LibraryModel;
  titleId?: string;
}) {
  const action = model.snapshot?.action;
  if (!action || (titleId && action.titleId !== titleId)) return null;
  return (
    <p
      className={
        action.state === "error"
          ? "form-error library-feedback"
          : "notice library-feedback"
      }
      role={action.state === "error" ? "alert" : "status"}
    >
      {t(action.message)}
    </p>
  );
}

export function StorageProgress({ model }: { model: LibraryModel }) {
  const [error, setError] = useState("");
  const [expanded, setExpanded] = useState<number | null>(null);
  const job = model.snapshot?.storageJob?.id ? model.snapshot.storageJob : null;
  const capacityError = model.snapshot?.storageError;
  if (!job && !model.snapshot?.storageBusy && !capacityError) return null;
  const terminal =
    !!job &&
    !job.active &&
    ["completed", "failed", "cancelled"].includes(job.state);
  const completed = job?.state === "completed";
  const details = !!job && expanded === job.id;
  const progress = job?.totalBytes
    ? Math.min(100, (100 * job.processedBytes) / job.totalBytes)
    : 0;
  const title =
    model.snapshot?.games.find((g) => g.titleId === job?.titleId)?.title ||
    job?.titleId ||
    t("Storage operation");
  const outcome =
    job?.operation === "delete" ||
    job?.operation === "move" ||
    job?.operation === "copy"
      ? outcomes[job.operation]
      : outcomes.other;
  const operation = t(outcome.operation);
  const result = t(
    completed
      ? outcome.completed
      : job?.state === "cancelled"
        ? outcome.cancelled
        : outcome.failed,
    { title },
  );
  return (
    <>
      {(job || model.snapshot?.storageBusy) && (
        <section
          className={`storage-job${terminal ? " storage-job-result" : ""}`}
          aria-label={
            terminal
              ? t("Storage operation result")
              : t("Storage operation progress")
          }
        >
          {terminal ? (
            <div className="storage-result-row">
              <span
                className={`storage-result-icon${completed ? " success" : ""}`}
              >
                <Icon name={completed ? "check" : "close"} />
              </span>
              <div className="storage-result-copy" role="status">
                <h3>{result}</h3>
                <p>
                  {completed
                    ? `${bytes(job.processedBytes)} · ${t("Complete")}`
                    : job.state === "cancelled"
                      ? t("Operation stopped")
                      : t("Operation could not finish")}
                </p>
              </div>
              <button
                className="storage-details-button"
                aria-expanded={details}
                onClick={() => setExpanded(details ? null : job.id)}
              >
                {details ? t("Hide details") : t("Details")}
              </button>
            </div>
          ) : (
            <div className="storage-job-heading">
              <div>
                <p className="library-eyebrow">{t("STORAGE ACTIVITY")}</p>
                <h3>{title}</h3>
              </div>
              <span className="library-label">
                {job
                  ? t(
                      {
                        idle: /* i18n */ "Idle",
                        preparing: /* i18n */ "Preparing",
                        measuring: /* i18n */ "Measuring",
                        transferring: /* i18n */ "Transferring",
                        finalizing: /* i18n */ "Finishing",
                      }[job.state] || job.state,
                    )
                  : t("Checking status")}
              </span>
            </div>
          )}
          {job && (!terminal || details) && (
            <div className={terminal ? "storage-result-details" : undefined}>
              <p>
                {operation} ·{" "}
                {job.active && !job.totalBytes
                  ? t("Measuring…")
                  : `${bytes(job.processedBytes)} / ${bytes(job.totalBytes)}`}
              </p>
              {!terminal && (
                <>
                  <progress
                    max={100}
                    value={progress}
                    aria-label={t("Storage operation progress")}
                  />
                  <div className="transfer-meta">
                    <span>{percent(progress)}</span>
                    <span>
                      {job.cancelRequested
                        ? t("Cancellation requested")
                        : job.active && job.speed > 0
                          ? `${bytes(job.speed)}/s`
                          : ""}
                    </span>
                  </div>
                </>
              )}
              <p className="library-path">{job.source}</p>
              {job.destination && (
                <p className="library-path">
                  {t("To {path}", { path: job.destination })}
                </p>
              )}
            </div>
          )}
          {job?.error && (
            <p className="form-error" role="alert">
              {t(job.error)}
            </p>
          )}
          {job?.active && (
            <button
              disabled={
                model.actionBusy ||
                !job.cancellable ||
                job.cancelRequested ||
                !model.can("storage_job_cancel")
              }
              onClick={() => {
                setError("");
                void model
                  .run("cancel", { jobId: job.id })
                  .catch((e) => setError((e as Error).message));
              }}
            >
              {job.cancelRequested
                ? t("Cancelling…")
                : job.cancellable
                  ? t("Cancel operation")
                  : t("Finishing, cannot cancel")}
            </button>
          )}
          {error && (
            <p className="form-error" role="alert">
              {error}
            </p>
          )}
        </section>
      )}
      {capacityError && (
        <div className="storage-capacity-notice" role="status">
          <Icon name="storage" />
          <div>
            <strong>{t("Drive space unavailable")}</strong>
            <p>{t(capacityError)}</p>
            <p>{t("Refresh drives in Storage to check available space.")}</p>
          </div>
        </div>
      )}
    </>
  );
}

export function GameActions({
  game,
  model,
}: {
  game: LibraryGame;
  model: LibraryModel;
}) {
  const [mode, setMode] = useState<"copy" | "move" | "unmount" | null>(null);
  const [destination, setDestination] = useState("");
  const [error, setError] = useState("");
  const [sending, setSending] = useState(false);
  useEffect(() => {
    setMode(null);
    setError("");
    setDestination("");
  }, [game.sourceKey]);
  const targets = (model.storage?.destinations || []).filter(
    (d) =>
      !d.readOnly &&
      d.path !== game.path &&
      !d.path.startsWith(`${game.path}/`) &&
      d.path !== game.path.slice(0, game.path.lastIndexOf("/")),
  );
  const target = targets.find((d) => d.id === destination);
  const storageFresh =
    !!model.storage?.updatedAt && !model.storage.stale && !model.storageError;
  const disabled = model.working || model.stale || sending || !game.onDrive;
  const transfer = game.managed && game.canManageSource && game.onDrive;
  const sharedTitles =
    model.snapshot?.games.filter((g) => g.path === game.path).length || 1;
  async function run(action: string) {
    setSending(true);
    setError("");
    try {
      await model.run(action, {
        titleId: game.titleId,
        sourceKey: game.sourceKey,
        ...(target ? { destinationId: target.id } : {}),
        confirmed: true,
      });
      setMode(null);
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setSending(false);
    }
  }
  return (
    <div className="library-game-actions">
      <LibraryFeedback model={model} titleId={game.titleId} />
      {model.snapshot?.storageJob?.titleId === game.titleId && (
        <StorageProgress model={model} />
      )}
      <div className="dialog-actions">
        {game.managed &&
          model.can(game.mounted ? "unmount_game" : "mount_game") && (
            <button
              className="primary"
              disabled={disabled}
              onClick={() =>
                game.mounted ? setMode("unmount") : void run("mount")
              }
            >
              {game.mounted ? t("Unmount game") : t("Mount game")}
            </button>
          )}
        {transfer &&
          model.can("copy_game_source") &&
          model.can("storage_job_status") && (
            <button
              disabled={disabled || game.mounted}
              onClick={() => {
                setMode("copy");
                void model.readStorage(true);
              }}
            >
              {t("Copy to drive")}
            </button>
          )}
        {transfer &&
          model.can("move_game_source") &&
          model.can("storage_job_status") && (
            <button
              disabled={disabled || game.mounted}
              onClick={() => {
                setMode("move");
                void model.readStorage(true);
              }}
            >
              {t("Move to drive")}
            </button>
          )}
      </div>
      {transfer && game.mounted && (
        <p className="fine">
          {t("Unmount this game before copying or moving its source.")}
        </p>
      )}
      {!game.managed && (
        <p className="fine">
          {t(
            "This item supports viewing details only. ShadowMount does not manage its source.",
          )}
        </p>
      )}
      {mode && (
        <section
          className="library-action-confirm"
          aria-label={
            mode === "unmount"
              ? t("Unmount game confirmation")
              : mode === "move"
                ? t("Move game confirmation")
                : t("Copy game confirmation")
          }
        >
          <h3>
            {mode === "unmount"
              ? t("Unmount this game?")
              : mode === "copy"
                ? t("Copy {title}", { title: game.title })
                : t("Move {title}", { title: game.title })}
          </h3>
          <p>
            {mode === "unmount"
              ? t("Close the game on your PS5 before unmounting its source.")
              : mode === "move"
                ? t(
                    "Move the source to another location. The original is removed only after the move succeeds.",
                  )
                : t(
                    "Create a second copy on the selected drive. The original stays in place.",
                  )}
          </p>
          {mode !== "unmount" && (
            <>
              {sharedTitles > 1 && (
                <p className="notice">
                  {tn(
                    sharedTitles,
                    "This source is shared by {count} Library title. This operation applies to the whole source.",
                    "This source is shared by {count} Library titles. This operation applies to the whole source.",
                  )}
                </p>
              )}
              <p className="library-path">
                {t("From {path}", { path: game.path })}
              </p>
              <Select
                label={t("Destination drive")}
                value={target?.id || ""}
                onChange={setDestination}
                options={[
                  {
                    value: "",
                    label: t("Choose a destination"),
                    disabled: true,
                  },
                  ...targets.map((d) => ({
                    value: d.id,
                    label: `${placeName(d.label)} · ${d.path} · ${t("{size} free", { size: bytes(d.freeBytes) })}`,
                  })),
                ]}
                disabled={disabled || !storageFresh}
              />
              {target && (
                <p className="library-path">
                  {t("To {path}", {
                    path: `${target.path}/${game.path.split("/").pop()}`,
                  })}
                </p>
              )}
              {!storageFresh && (
                <p className="notice">
                  {t(
                    model.storage?.error ||
                      model.storageError ||
                      "Checking available destinations…",
                  )}
                </p>
              )}
              {storageFresh && !targets.length && (
                <p className="notice">
                  {t(
                    "Connect another writable drive, or configure another scan location in ShadowMount.",
                  )}
                </p>
              )}
              <p className="fine">
                {t(
                  "Orbit checks the source and free space before starting. Pause active and queued downloads first. Existing destination files are not overwritten.",
                )}
              </p>
            </>
          )}
          <div className="dialog-actions">
            <button
              className="primary"
              disabled={
                disabled || (mode !== "unmount" && (!target || !storageFresh))
              }
              onClick={() => void run(mode)}
            >
              {sending
                ? t("Starting…")
                : mode === "unmount"
                  ? t("Confirm unmount")
                  : mode === "copy"
                    ? t("Start copy")
                    : t("Confirm move")}
            </button>
            <button disabled={sending} onClick={() => setMode(null)}>
              {t("Back")}
            </button>
          </div>
        </section>
      )}
      {error && (
        <p className="form-error" role="alert">
          {error}
        </p>
      )}
    </div>
  );
}

export function StorageOverview({
  model,
  openGame,
  feedback = false,
}: {
  model: LibraryModel;
  openGame: (g: LibraryGame) => void;
  feedback?: boolean;
}) {
  const [selected, setSelected] = useState("");
  const [error, setError] = useState("");
  const drives = model.storage?.drives || [];
  const drive = drives.find((d) => d.id === selected) || drives[0];
  const games = (model.snapshot?.games || [])
    .filter(
      (g) =>
        g.onDrive &&
        drive &&
        ((drive.mountPoint === "/user" && g.path.startsWith("/data/")) ||
          g.path === drive.mountPoint ||
          g.path.startsWith(`${drive.mountPoint}/`)),
    )
    .sort(
      (a, b) =>
        (b.sizeBytes ?? -1) - (a.sizeBytes ?? -1) ||
        a.title.localeCompare(b.title),
    );
  const sizesKnown = games.filter((g) => g.sizeBytes !== null).length;
  return (
    <section className="storage-overview" aria-label={t("Game storage")}>
      <div className="storage-overview-heading">
        <div>
          <h2>{t("Your drives")}</h2>
          <p>{t("See available space and manage where your games live.")}</p>
        </div>
        <button
          disabled={!!model.storage?.busy}
          onClick={() => void model.readStorage(true)}
        >
          {model.storage?.busy ? t("Refreshing…") : t("Refresh drives")}
        </button>
      </div>
      {(model.storageError || model.storage?.error) && (
        <p className="notice" role="status">
          {t(model.storageError || model.storage?.error || "")}
        </p>
      )}
      {model.storage?.stale && !!model.storage.updatedAt && (
        <p className="notice">
          {t(
            "Saved drive information. Reconnect and refresh before managing files.",
          )}
        </p>
      )}
      {!drives.length ? (
        <div className="empty compact">
          <Icon name="usb" />
          <h3>
            {model.storage?.busy
              ? t("Reading drives…")
              : t("No drives reported")}
          </h3>
          <p>{t("Drive management uses ShadowMount’s storage API.")}</p>
        </div>
      ) : (
        <>
          <div
            className="drive-grid"
            role="group"
            aria-label={t("Select drive")}
          >
            {drives.map((d) => (
              <button
                key={d.id}
                className="drive-card"
                aria-pressed={drive?.id === d.id}
                onClick={() => setSelected(d.id)}
              >
                <span className="drive-card-title">
                  <Icon name="usb" />
                  <strong>{placeName(d.label)}</strong>
                </span>
                <span>
                  {d.path}
                  {d.readOnly ? ` · ${t("Read only")}` : ""}
                </span>
                <span className="storage-meter">
                  <span
                    style={{
                      width: `${d.totalBytes ? Math.min(100, (100 * d.usedBytes) / d.totalBytes) : 0}%`,
                    }}
                  />
                </span>
                <strong>
                  {t("{size} free", { size: bytes(d.freeBytes) })}
                </strong>
                <span>
                  {t("{used} used of {total}", {
                    used: bytes(d.usedBytes),
                    total: bytes(d.totalBytes),
                  })}
                </span>
              </button>
            ))}
          </div>
          <div className="storage-overview-heading">
            <div>
              <h3>
                {t("Games on {drive}", {
                  drive: placeName(drive?.label || ""),
                })}
              </h3>
              <p>
                {tn(games.length, "{count} game", "{count} games")} ·{" "}
                {tn(
                  sizesKnown,
                  "{count} size measured",
                  "{count} sizes measured",
                )}
              </p>
            </div>
            <button
              disabled={model.working || !model.can("list_games")}
              onClick={() => {
                setError("");
                void model
                  .run("measure")
                  .catch((e) => setError((e as Error).message));
              }}
            >
              {model.snapshot?.action?.action === "measure" && model.actionBusy
                ? t("Measuring…")
                : t("Measure game sizes")}
            </button>
          </div>
          <p className="fine">
            {t(
              "Sizes are measured on request. Shared source files can appear under multiple titles; drive usage includes other files.",
            )}
          </p>
          <div className="storage-game-list">
            {games.map((g) => (
              <button
                className="storage-game"
                key={g.titleId}
                onClick={() => openGame(g)}
              >
                <span>
                  <strong>{g.title}</strong>
                  <small>
                    {g.format} ·{" "}
                    {g.mounted
                      ? t("Mounted")
                      : g.installed
                        ? t("Installed")
                        : t("On drive")}
                  </small>
                </span>
                <span>
                  {g.sizeBytes !== null
                    ? bytes(g.sizeBytes)
                    : g.sizeStatus === "unavailable"
                      ? t("Size unavailable")
                      : t("Not measured")}
                </span>
                <span aria-hidden="true">›</span>
              </button>
            ))}
          </div>
          {!games.length && (
            <p className="notice">
              {t("No Library games currently match this drive.")}
            </p>
          )}
        </>
      )}
      {error && (
        <p className="form-error" role="alert">
          {error}
        </p>
      )}
      {feedback && <LibraryFeedback model={model} />}
      <StorageProgress model={model} />
    </section>
  );
}
