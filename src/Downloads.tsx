import { useEffect, useMemo, useState } from "react";
import { Icon, Modal } from "./components";
import { Artwork } from "./Artwork";
import {
  bytes,
  type Game,
  type Job,
  type Storage,
  type LibraryTarget,
} from "./types";
import { formatNumber, placeName, t, tn } from "./i18n";
const statusLabels: Record<Job["status"], string> = {
  queued: "Queued",
  downloading: "Downloading",
  retrying: "Retrying",
  verifying: "Verifying",
  paused: "Paused",
  error: "Needs attention",
  complete: "Complete",
  cancelled: "Cancelled",
};
const inactive = (job: Job) => ["complete", "cancelled"].includes(job.status);
const waiting = (job: Job) =>
  ["queued", "paused", "retrying"].includes(job.status);
export function Downloads({
  jobs,
  games,
  drives,
  action,
  clearHistory,
  browse,
  targetId,
  openLibrary,
}: {
  jobs: Job[];
  games: Game[];
  drives: Storage[];
  action: (id: string, name: string, remove?: boolean) => Promise<void>;
  clearHistory: (status: "complete" | "cancelled") => Promise<void>;
  browse: () => void;
  targetId: string | null;
  openLibrary: (target: Omit<LibraryTarget, "request">) => void;
}) {
  const [view, setView] = useState<
    "Active" | "Finished" | "Failed" | "Cancelled"
  >("Active");
  const [error, setError] = useState(""),
    [busy, setBusy] = useState("");
  const [cancel, setCancel] = useState<Job | null>(null),
    [clear, setClear] = useState(false);
  const [forget, setForget] = useState(false);
  const [message, setMessage] = useState("");
  const targetStatus = jobs.find((j) => j.id === targetId)?.status;
  useEffect(() => {
    if (targetId && targetStatus)
      setView(
        targetStatus === "complete"
          ? "Finished"
          : targetStatus === "cancelled"
            ? "Cancelled"
            : targetStatus === "error"
              ? "Failed"
              : "Active",
      );
  }, [targetId, targetStatus]);
  useEffect(() => {
    if (!targetId) return;
    const row = [...document.querySelectorAll<HTMLElement>("[data-job]")].find(
      (el) => el.dataset.job === targetId,
    );
    row?.scrollIntoView({ block: "center" });
    row
      ?.querySelector<HTMLButtonElement>("button")
      ?.focus({ preventScroll: true });
  }, [targetId, view]);
  const gameByRelease = useMemo(
    () =>
      new Map(games.flatMap((g) => g.releases.map((r) => [r.id, g] as const))),
    [games],
  );
  const groups = {
    Active: jobs.filter((j) => !inactive(j) && j.status !== "error"),
    Finished: jobs.filter((j) => j.status === "complete"),
    Failed: jobs.filter((j) => j.status === "error"),
    Cancelled: jobs.filter((j) => j.status === "cancelled"),
  };
  const queued = jobs.filter(waiting);
  function restoreFocus() {
    requestAnimationFrame(() => {
      if (document.activeElement === document.body)
        document
          .querySelector<HTMLButtonElement>(
            ".download-tabs [aria-pressed=true]",
          )
          ?.focus();
    });
  }
  async function run(job: Job, name: string, remove = false) {
    setBusy(job.id);
    setError("");
    setMessage("");
    try {
      await action(job.id, name, remove);
      setCancel(null);
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy("");
      restoreFocus();
    }
  }
  async function clearSelectedHistory() {
    setBusy("history");
    setError("");
    try {
      await clearHistory(view === "Cancelled" ? "cancelled" : "complete");
      setClear(false);
      setMessage(
        view === "Cancelled"
          ? t(
              "Cancelled history cleared. Kept partial files and disconnected downloads stay listed so you can recover them.",
            )
          : t(
              "Finished history cleared. Your downloaded files stay on the drive.",
            ),
      );
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy("");
      restoreFocus();
    }
  }
  return (
    <section className="page downloads">
      <div className="page-heading">
        <h1>{t("Downloads")}</h1>
        <p>
          {tn(groups.Active.length, "{count} active", "{count} active")} ·{" "}
          {tn(
            groups.Failed.length,
            "{count} needs attention",
            "{count} need attention",
          )}
        </p>
      </div>
      <div className="download-toolbar">
        <div
          className="download-tabs"
          role="group"
          aria-label={t("Download views")}
        >
          {(
            [
              /* i18n */ "Active",
              /* i18n */ "Finished",
              /* i18n */ "Failed",
              /* i18n */ "Cancelled",
            ] as const
          ).map((v) => (
            <button
              key={v}
              aria-pressed={view === v}
              onClick={() => {
                setView(v);
                setMessage("");
              }}
            >
              {t(v)} <span>{groups[v].length}</span>
            </button>
          ))}
        </div>
        {(view === "Finished" || view === "Cancelled") &&
          groups[view].length > 0 && (
            <button disabled={!!busy} onClick={() => setClear(true)}>
              {view === "Cancelled"
                ? t("Clear cancelled history")
                : t("Clear finished history")}
            </button>
          )}
      </div>
      {error && (
        <p className="form-error" role="alert">
          {error}
        </p>
      )}
      {message && (
        <p className="notice" role="status">
          {message}
        </p>
      )}
      {!groups[view].length ? (
        <div className="empty compact">
          <Icon name={view === "Finished" ? "check" : "download"} />
          <h2>
            {view === "Active"
              ? t("Nothing in progress")
              : view === "Failed"
                ? t("No failed downloads")
                : view === "Cancelled"
                  ? t("No cancelled downloads")
                  : t("No finished downloads")}
          </h2>
          <p>
            {view === "Active"
              ? t("Choose a game to add it to your queue.")
              : view === "Failed"
                ? t("Downloads that need your attention appear here.")
                : view === "Cancelled"
                  ? t(
                      "Cancelled downloads appear here. Kept partial files can be resumed.",
                    )
                  : t("Successfully completed downloads appear here.")}
          </p>
          <button className="primary" onClick={browse}>
            {t("Browse games")}
          </button>
        </div>
      ) : (
        <div className="queue">
          {groups[view].map((j) => {
            const g = gameByRelease.get(j.releaseId),
              r = g?.releases.find((r) => r.id === j.releaseId);
            const title = g?.title || j.title || j.filename;
            const active = [
              "queued",
              "downloading",
              "retrying",
              "verifying",
            ].includes(j.status);
            const progress =
              j.total > 0
                ? Math.min(100, Math.max(0, (100 * j.received) / j.total))
                : 0;
            const position = queued.findIndex((q) => q.id === j.id);
            const seconds = Math.max(
              0,
              Math.ceil(j.retryAt - Date.now() / 1000),
            );
            const drive = drives.find((d) => d.id === j.storageId);
            return (
              <article
                className="queue-row"
                key={j.id}
                data-job={j.id}
                aria-label={title}
              >
                <Artwork
                  key={g?.cover || "/orbit.svg"}
                  src={g?.cover || "/orbit.svg"}
                  fallbackSrc={g?.coverFallback}
                  className={g ? undefined : "queue-placeholder"}
                  alt=""
                  decoding="async"
                  referrerPolicy="no-referrer"
                />
                <div className="queue-info">
                  <div className="queue-title">
                    <h2>{title}</h2>
                    <span className={`status ${j.status}`}>
                      {j.status === "complete" && <Icon name="check" />}{" "}
                      {t(statusLabels[j.status] || j.status)}
                    </span>
                  </div>
                  <p>
                    {r && `${r.provider} · ${r.format} · `}
                    {j.delivery === "torbox" && `${t("via TorBox")} · `}
                    {drive ? placeName(drive.label) : j.storageId}
                    {!drive && j.status !== "complete"
                      ? ` · ${t("Reconnect this drive")}`
                      : ""}
                  </p>
                  <progress
                    max="100"
                    value={progress}
                    aria-label={t("{title} progress", { title })}
                  />
                  <div className="transfer-meta">
                    <span>
                      {bytes(j.received)} / {bytes(j.total)} ·{" "}
                      {formatNumber(progress / 100, {
                        style: "percent",
                        minimumFractionDigits: 1,
                        maximumFractionDigits: 1,
                      })}
                    </span>
                    <span>
                      {j.status === "retrying"
                        ? seconds > 0
                          ? t("Retrying in {seconds} s", { seconds })
                          : t("Waiting to retry…")
                        : j.speed > 0
                          ? t("{speed}/s · {minutes} min remaining", {
                              speed: bytes(j.speed),
                              minutes: Math.ceil(
                                (j.total - j.received) / j.speed / 60,
                              ),
                            })
                          : j.status === "complete"
                            ? j.verification === "sha256"
                              ? t("SHA-256 verified")
                              : t("Size verified")
                            : t("{size} remaining", {
                                size: bytes(Math.max(0, j.total - j.received)),
                              })}
                    </span>
                  </div>
                  {j.deliveryPhase &&
                    ["queued", "downloading"].includes(j.status) && (
                      <p className="fine">{t(j.deliveryPhase)}</p>
                    )}
                  {j.error && <p className="job-error">{t(j.error)}</p>}
                  {j.status === "cancelled" && j.received > 0 && (
                    <p className="fine">
                      {t(
                        "Partial file kept. Resume it, or delete the partial file before removing this entry.",
                      )}
                    </p>
                  )}
                  <div className="queue-actions">
                    {j.status === "complete" && (
                      <button
                        className="primary"
                        onClick={() =>
                          openLibrary({
                            titleId: j.titleId,
                            path: j.path,
                            title,
                          })
                        }
                      >
                        {t("Find in Library")}
                      </button>
                    )}
                    {j.status !== "complete" ? (
                      <>
                        <button
                          disabled={!!busy || (!active && !drive)}
                          onClick={() =>
                            void run(
                              j,
                              active
                                ? "pause"
                                : j.status === "error"
                                  ? "retry"
                                  : "resume",
                            )
                          }
                        >
                          {active
                            ? t("Pause")
                            : j.status === "error"
                              ? t("Retry download")
                              : j.status === "cancelled" && !j.received
                                ? t("Start again")
                                : t("Resume")}
                        </button>
                        <button
                          disabled={!!busy}
                          onClick={() => {
                            setForget(false);
                            setError("");
                            setCancel(j);
                          }}
                        >
                          {j.status === "cancelled"
                            ? t("Delete download")
                            : t("Cancel")}
                        </button>
                      </>
                    ) : null}
                    {waiting(j) && (
                      <div
                        className="queue-order"
                        role="group"
                        aria-label={t("Queue position for {title}", { title })}
                      >
                        <button
                          disabled={!!busy || position <= 0}
                          aria-label={t("Move {title} up", { title })}
                          onClick={() => void run(j, "move-up")}
                        >
                          {t("↑ Move up")}
                        </button>
                        <button
                          disabled={!!busy || position === queued.length - 1}
                          aria-label={t("Move {title} down", { title })}
                          onClick={() => void run(j, "move-down")}
                        >
                          {t("↓ Move down")}
                        </button>
                      </div>
                    )}
                    {inactive(j) &&
                      (j.status === "complete" || j.received === 0) && (
                        <button
                          disabled={!!busy}
                          onClick={() => void run(j, "remove")}
                        >
                          {t("Remove from history")}
                        </button>
                      )}
                  </div>
                </div>
              </article>
            );
          })}
        </div>
      )}
      {cancel && (
        <Modal
          title={
            cancel.status === "cancelled"
              ? t("Delete download")
              : t("Cancel download")
          }
          close={() => !busy && setCancel(null)}
        >
          <h2>
            {cancel.status === "cancelled"
              ? forget
                ? t("Remove from history?")
                : t("Delete this cancelled download?")
              : t("Cancel this download?")}
          </h2>
          <p>
            {cancel.status === "cancelled"
              ? forget
                ? t(
                    "Only the history entry will be removed. Any partial file stays on its drive and must be deleted before downloading there again.",
                  )
                : t(
                    "Delete the partial file and remove this entry. If the file is already gone, the entry will still be removed. Completed files are kept.",
                  )
              : t(
                  "Keep the partial file to resume later, or delete only the partial file. Downloaded files are never deleted through history cleanup.",
                )}
          </p>
          {error && (
            <p className="job-error" role="alert">
              {t(error)}
            </p>
          )}
          <div className="dialog-actions">
            {cancel.status === "cancelled" ? (
              <>
                <button
                  className="danger"
                  disabled={!!busy}
                  onClick={() => void run(cancel, forget ? "forget" : "delete")}
                >
                  {forget ? t("Remove from history") : t("Delete download")}
                </button>
                {!forget && (
                  <button
                    disabled={!!busy}
                    onClick={() => {
                      setForget(true);
                      setError("");
                    }}
                  >
                    {t("Remove from history")}
                  </button>
                )}
              </>
            ) : (
              <>
                <button
                  onClick={() => void run(cancel, "cancel")}
                  disabled={!!busy}
                >
                  {t("Keep partial file")}
                </button>
                <button
                  className="danger"
                  onClick={() => void run(cancel, "cancel", true)}
                  disabled={!!busy}
                >
                  {t("Delete partial file")}
                </button>
              </>
            )}
            <button disabled={!!busy} onClick={() => setCancel(null)}>
              {t("Go back")}
            </button>
          </div>
        </Modal>
      )}
      {clear && (
        <Modal
          title={
            view === "Cancelled"
              ? t("Clear cancelled history")
              : t("Clear finished history")
          }
          close={() => !busy && setClear(false)}
        >
          <h2>
            {view === "Cancelled"
              ? t("Clear cancelled history?")
              : t("Clear finished history?")}
          </h2>
          <p>
            {view === "Cancelled"
              ? t(
                  "Remove cancelled entries with no partial file. Kept partials stay listed for recovery.",
                )
              : t(
                  "Remove completed entries from history. Your downloaded files stay on the drive.",
                )}
          </p>
          <div className="dialog-actions">
            <button
              className="primary"
              disabled={!!busy}
              onClick={() => void clearSelectedHistory()}
            >
              {t("Clear history")}
            </button>
            <button disabled={!!busy} onClick={() => setClear(false)}>
              {t("Keep history")}
            </button>
          </div>
        </Modal>
      )}
    </section>
  );
}
