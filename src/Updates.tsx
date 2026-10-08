import { useEffect, useState } from "react";
import { api } from "./api";
import { bytes } from "./types";
import type { UpdateStatus as Status } from "./updateStatus";
import { t } from "./i18n";
import { rich } from "./Rich";
export function Updates({ onStopped }: { onStopped: () => void }) {
  const [status, setStatus] = useState<Status | null>(null);
  const [error, setError] = useState("");
  const [sending, setSending] = useState(false);
  const [confirmStop, setConfirmStop] = useState(false);
  const [stopped, setStopped] = useState(false);
  const [now, setNow] = useState(Date.now());
  useEffect(() => {
    if (stopped) return;
    let active = true;
    let loading = false;
    const refresh = async () => {
      if (loading) return;
      loading = true;
      try {
        const next = await api<Status>("/updates");
        if (active) setStatus(next);
      } catch (e) {
        if (active) setError((e as Error).message);
      } finally {
        loading = false;
      }
    };
    void refresh();
    const timer = setInterval(() => {
      setNow(Date.now());
      void refresh();
    }, 2000);
    return () => {
      active = false;
      clearInterval(timer);
    };
  }, [stopped]);
  async function act(action: string) {
    setSending(true);
    setError("");
    try {
      const next = await api<Status>("/updates", {
        action,
        ...(action === "install"
          ? { version: status?.latestVersion, checksum: status?.checksum }
          : action === "stop"
            ? { confirmed: true }
            : {}),
      });
      setStatus(next);
      if (action === "stop") {
        setStopped(true);
        onStopped();
      }
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setSending(false);
    }
  }
  const fresh = !!status?.checkedAt && now / 1000 - status.checkedAt <= 900;
  const disabled = sending || status?.busy;
  const wait = Math.max(
    0,
    Math.ceil(((status?.retryAt || 0) * 1000 - now) / 1000),
  );
  const checkWait = Math.max(
    wait,
    Math.ceil(((status?.checkAfter || 0) * 1000 - now) / 1000),
  );
  return (
    <section className="updates-panel" aria-labelledby="updates-title">
      <h2 id="updates-title">{t("Update / reinstall")}</h2>
      {stopped ? (
        <div className="notice" role="status">
          <strong>{t("Orbit is stopping.")}</strong>
          <p>
            {rich(
              "Open your payload manager and run {path} once, or use a copy you opted to keep in sync. Then reopen the Orbit icon or reconnect here. Resume your downloads when you’re ready.",
              { path: <b>/data/orbit-store/orbit_store.elf</b> },
            )}
          </p>
          <button onClick={() => location.reload()}>
            {t("Reconnect to Orbit")}
          </button>
        </div>
      ) : !status ? (
        <p>{t("Loading installed version…")}</p>
      ) : (
        <>
          <dl className="update-versions">
            <div>
              <dt>{t("Running")}</dt>
              <dd>{status.runningVersion}</dd>
            </div>
            <div>
              <dt>{t("Saved for next start")}</dt>
              <dd>{status.savedVersion || t("Not recorded yet")}</dd>
            </div>
            {status.latestVersion && (
              <div>
                <dt>{t("Latest release")}</dt>
                <dd>{status.latestVersion}</dd>
              </div>
            )}
          </dl>
          {!status.available ? (
            <p className="notice">
              {t(
                "Open Orbit on your PS5 or pair your phone with it to update the console’s saved app.",
              )}
            </p>
          ) : (
            <>
              <p>
                {t(
                  "Get the official release directly on your PS5. Orbit verifies its SHA-256 checksum and replaces its saved copy. Pairing, sources and your queue stay saved. Only manager copies you opted to sync are refreshed. A manually imported copy needs replacing if you have not enabled sync under Payload managers.",
                )}
              </p>
              <div className="dialog-actions">
                <button
                  disabled={disabled || checkWait > 0}
                  onClick={() => void act("check")}
                >
                  {status.phase === "checking"
                    ? t("Checking…")
                    : checkWait > 0
                      ? t("Check again in {seconds} s", { seconds: checkWait })
                      : t("Check for updates")}
                </button>
                {status.latestVersion && (
                  <button
                    className="primary"
                    disabled={disabled || wait > 0 || !fresh}
                    onClick={() => void act("install")}
                  >
                    {status.phase === "downloading" || status.phase === "saving"
                      ? t("Installing…")
                      : status.latestVersion === status.runningVersion
                        ? t("Reinstall release")
                        : t("Install update")}
                  </button>
                )}
              </div>
              <p className="fine" role="status">
                {status.phase === "downloading"
                  ? t("Downloading Orbit: {size}", {
                      size: bytes(status.received),
                    })
                  : status.phase === "saving"
                    ? t("Saving the verified release…")
                    : wait > 0
                      ? t("Please wait {seconds} s before another request.", {
                          seconds: wait,
                        })
                      : status.phase === "checked" &&
                          status.latestVersion === status.runningVersion
                        ? t(
                            "You’re running the latest published version. You can reinstall it if needed.",
                          )
                        : ""}
              </p>
              {status.latestVersion && !fresh && !status.busy && (
                <p className="fine">
                  {t(
                    "Check for updates to confirm this release before installing.",
                  )}
                </p>
              )}
              {status.restartRequired && (
                <div className="notice" role="status">
                  <strong>{t("Restart needed")}</strong>
                  <p>
                    {t(
                      "The saved version starts the next time you run Orbit. The current session stays open until you stop it.",
                    )}
                  </p>
                </div>
              )}
              {!confirmStop ? (
                <button
                  disabled={disabled}
                  onClick={() => setConfirmStop(true)}
                >
                  {t("Stop Orbit to restart")}
                </button>
              ) : (
                <div className="notice">
                  <p>
                    {rich(
                      "Stop Orbit and pause active downloads? Then run {path} (or a synced copy) from your payload manager and reopen Orbit. Other payloads keep running.",
                      { path: <b>/data/orbit-store/orbit_store.elf</b> },
                    )}
                  </p>
                  <div className="dialog-actions">
                    <button
                      disabled={disabled}
                      onClick={() => void act("stop")}
                    >
                      {t("Stop Orbit")}
                    </button>
                    <button
                      disabled={disabled}
                      onClick={() => setConfirmStop(false)}
                    >
                      {t("Keep running")}
                    </button>
                  </div>
                </div>
              )}
            </>
          )}
          {status.error && (
            <p className="form-error" role="alert">
              {t(status.error)}
            </p>
          )}
        </>
      )}
      {error && (
        <p className="form-error" role="alert">
          {error}
        </p>
      )}
    </section>
  );
}
