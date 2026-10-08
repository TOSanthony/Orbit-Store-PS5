import { useEffect, useState } from "react";
import { api } from "./api";
import { bytes } from "./types";
import { t } from "./i18n";
import { rich } from "./Rich";
const releasesUrl =
  "https://github.com/saawant12/orbit-store-ps5/releases/latest";
interface Status {
  available: boolean;
  installed: "none" | "orbit" | "manual" | "folder" | "blocked";
  installedVersion: string;
  latestVersion: string;
  checksum: string;
  size: number;
  updateAvailable: boolean;
  phase: string;
  error: string;
  busy: boolean;
  received: number;
  retryAt: number;
  checkedAt: number;
  checkAfter: number;
}
const installedLabel = (status: Status) =>
  status.installed === "orbit"
    ? status.installedVersion
      ? t("Version {version}", { version: status.installedVersion })
      : t("Installed")
    : status.installed === "manual"
      ? t("A copy you placed yourself")
      : status.installed === "folder"
        ? t("Folder copy in /data/homebrew")
        : status.installed === "blocked"
          ? t("Blocked by another file")
          : t("Not installed");
export function TvApp() {
  const [status, setStatus] = useState<Status | null>(null);
  const [error, setError] = useState("");
  const [sending, setSending] = useState(false);
  const [confirmReplace, setConfirmReplace] = useState(false);
  const [now, setNow] = useState(Date.now());
  useEffect(() => {
    let active = true;
    let loading = false;
    const refresh = async () => {
      if (loading) return;
      loading = true;
      try {
        const next = await api<Status>("/tv-app");
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
  }, []);
  async function act(action: "check" | "install", replace = false) {
    setSending(true);
    setError("");
    setConfirmReplace(false);
    try {
      setStatus(
        await api<Status>("/tv-app", {
          action,
          ...(action === "install"
            ? {
                version: status?.latestVersion,
                checksum: status?.checksum,
                ...(replace ? { replace: true } : {}),
              }
            : {}),
        }),
      );
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setSending(false);
    }
  }
  const fresh = !!status?.checkedAt && now / 1000 - status.checkedAt <= 900;
  const disabled = sending || !!status?.busy;
  const wait = Math.max(
    0,
    Math.ceil(((status?.retryAt || 0) * 1000 - now) / 1000),
  );
  const checkWait = Math.max(
    wait,
    Math.ceil(((status?.checkAfter || 0) * 1000 - now) / 1000),
  );
  const installable =
    status?.installed === "none" || status?.installed === "orbit";
  const installLabel =
    status?.installed === "none"
      ? t("Install on this PS5")
      : status?.updateAvailable
        ? t("Update TV app")
        : t("Reinstall TV app");
  return (
    <section className="updates-panel" aria-labelledby="tv-app-title">
      <h2 id="tv-app-title">{t("TV app")}</h2>
      <p>
        {t(
          "Open Orbit full screen from your PS5’s Games row and browse with your controller. It needs ShadowMountPlus and kstuff running on your PS5. Orbit still does the downloading, and the app starts it if it isn’t running.",
        )}
      </p>
      {status?.available && (
        <dl className="update-versions">
          <div>
            <dt>{t("On this PS5")}</dt>
            <dd>{installedLabel(status)}</dd>
          </div>
          {status.latestVersion && (
            <div>
              <dt>{t("Latest release")}</dt>
              <dd>
                {status.latestVersion} · {bytes(status.size)}
              </dd>
            </div>
          )}
        </dl>
      )}
      {status?.available && (
        <>
          <div className="dialog-actions">
            <button
              disabled={disabled || checkWait > 0}
              onClick={() => void act("check")}
            >
              {status.phase === "checking"
                ? t("Checking…")
                : checkWait > 0
                  ? t("Check again in {seconds} s", { seconds: checkWait })
                  : t("Check for the TV app")}
            </button>
            {status.latestVersion && installable && (
              <button
                className="primary"
                disabled={disabled || wait > 0 || !fresh}
                onClick={() => void act("install")}
              >
                {status.busy && status.phase !== "checking"
                  ? t("Installing…")
                  : installLabel}
              </button>
            )}
            {status.latestVersion &&
              status.installed === "manual" &&
              !confirmReplace && (
                <button
                  disabled={disabled || wait > 0 || !fresh}
                  onClick={() => setConfirmReplace(true)}
                >
                  {t("Replace your copy")}
                </button>
              )}
          </div>
          {confirmReplace && (
            <div className="notice">
              <p>
                {rich(
                  "Replace {path} with release {version}? Close the TV app first if it’s open.",
                  {
                    path: <b>/data/homebrew/PPSA99177.ffpkg</b>,
                    version: status.latestVersion,
                  },
                )}
              </p>
              <div className="dialog-actions">
                <button
                  className="primary"
                  disabled={disabled}
                  onClick={() => void act("install", true)}
                >
                  {t("Replace")}
                </button>
                <button
                  disabled={disabled}
                  onClick={() => setConfirmReplace(false)}
                >
                  {t("Keep my copy")}
                </button>
              </div>
            </div>
          )}
          <p className="fine" role="status">
            {status.phase === "downloading"
              ? t("Downloading the TV app: {received} of {size}", {
                  received: bytes(status.received),
                  size: bytes(status.size),
                })
              : status.phase === "verifying"
                ? t("Checking the download…")
                : status.phase === "installing"
                  ? t("Installing…")
                  : wait > 0 && !status.busy && status.phase !== "installed"
                    ? t("Please wait {seconds} s before another request.", {
                        seconds: wait,
                      })
                    : status.latestVersion && !fresh && !status.busy
                      ? t("Check for the TV app again before installing.")
                      : ""}
          </p>
          {status.phase === "installed" && (
            <div className="notice" role="status">
              <strong>{t("TV app installed")}</strong>
              <p>
                {t(
                  "ShadowMountPlus adds Orbit Store to your Games row shortly. If the tile doesn’t appear, or an update still opens the old version, restart the console.",
                )}
              </p>
            </div>
          )}
          {status.installed === "folder" && (
            <p className="notice">
              {rich(
                "The TV app is installed as the folder {path}. Keep updating that copy yourself, or remove the folder to let Orbit install and update it.",
                { path: <b>/data/homebrew/PPSA99177</b> },
              )}
            </p>
          )}
          {status.installed === "blocked" && (
            <p className="notice">
              {rich(
                "Something other than the TV app is at {path}. Move it away to install the TV app here.",
                { path: <b>/data/homebrew/PPSA99177.ffpkg</b> },
              )}
            </p>
          )}
          {status.error && (
            <p className="form-error" role="alert">
              {t(status.error)}
            </p>
          )}
        </>
      )}
      {status && !status.available && (
        <p className="notice">
          {t(
            "Install it from Orbit running on your PS5, or download it below.",
          )}
        </p>
      )}
      <p className="fine">
        <a
          className="text-link"
          href={releasesUrl}
          target="_blank"
          rel="noreferrer"
        >
          {t("Download from GitHub")}
        </a>
        {". "}
        {rich(
          "To install it yourself, copy {file} to {folder} on your PS5 with FTP or a file manager.",
          {
            file: <b>PPSA99177.ffpkg</b>,
            folder: <b>/data/homebrew/</b>,
          },
        )}
      </p>
      {error && (
        <p className="form-error" role="alert">
          {error}
        </p>
      )}
    </section>
  );
}
