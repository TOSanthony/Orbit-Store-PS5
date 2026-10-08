import { useCallback, useEffect, useRef, useState } from "react";
import { api } from "./api";
import { bytes, type Release } from "./types";
import { t } from "./i18n";
import { rich } from "./Rich";
export interface DebridStatus {
  provider: "torbox";
  available: boolean;
  connected: boolean;
  busy: boolean;
  error: string;
  checkedAt: number;
  retryAt: number;
  revision: number;
  hosts: { sourceId: string; available: boolean; maxFileSize: number }[];
}
export function torboxAvailable(status: DebridStatus | null, release: Release) {
  const host = status?.hosts.find((h) => h.sourceId === release.sourceId);
  return (
    !!status?.connected &&
    !!status.available &&
    !status.busy &&
    !!host?.available &&
    (!host.maxFileSize || release.sizeBytes <= host.maxFileSize)
  );
}
export function useDebrid(enabled = true) {
  const [status, setStatus] = useState<DebridStatus | null>(null);
  const [error, setError] = useState("");
  const alive = useRef(false),
    loading = useRef(false);
  const refresh = useCallback(async () => {
    if (!alive.current || loading.current) return;
    loading.current = true;
    try {
      const next = await api<DebridStatus>("/debrid");
      if (alive.current) {
        setStatus((current) =>
          JSON.stringify(current) === JSON.stringify(next) ? current : next,
        );
        setError("");
      }
    } catch (e) {
      if (alive.current) {
        setStatus(null);
        setError((e as Error).message);
      }
    } finally {
      loading.current = false;
    }
  }, []);
  useEffect(() => {
    alive.current = enabled;
    if (!enabled) return;
    void refresh();
    const timer = setInterval(() => {
      if (document.visibilityState !== "hidden") void refresh();
    }, 5000);
    return () => {
      alive.current = false;
      clearInterval(timer);
    };
  }, [enabled, refresh]);
  return { status, error, refresh };
}
export function Debrid() {
  const { status, error: connectionError, refresh } = useDebrid();
  const [key, setKey] = useState(""),
    [sending, setSending] = useState(false);
  const [error, setError] = useState(""),
    [confirmDisconnect, setConfirmDisconnect] = useState(false);
  async function act(action: "connect" | "refresh" | "disconnect") {
    setSending(true);
    setError("");
    const apiKey = key.trim();
    setKey("");
    try {
      await api<DebridStatus>("/debrid", {
        action,
        ...(action === "connect" ? { apiKey } : {}),
      });
      setConfirmDisconnect(false);
      await refresh();
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setSending(false);
    }
  }
  const disabled = sending || !!status?.busy || !status?.available;
  return (
    <section
      className="updates-panel debrid-settings"
      aria-labelledby="debrid-heading"
      tabIndex={-1}
    >
      <h2 id="debrid-heading">{t("Debrid")}</h2>
      <p>{t("Connect TorBox to download supported links through your own account. Choose TorBox on a game's download page; your files still save to the selected PS5 drive.")}</p>
      <p className="fine">{t("TorBox plan, host and file-size limits apply. Orbit does not purchase a plan or remove files from your TorBox account.")}</p>
      <p role="status">
        {status?.busy
          ? t("Checking TorBox…")
          : status?.connected
            ? t("TorBox connected")
            : t("TorBox not connected")}
      </p>
      <form
        onSubmit={(e) => {
          e.preventDefault();
          void act("connect");
        }}
      >
        <label className="field" htmlFor="torbox-key">{t("TorBox API key")}</label>
        <input
          id="torbox-key"
          type="password"
          autoComplete="off"
          spellCheck={false}
          value={key}
          onChange={(e) => setKey(e.target.value)}
          disabled={disabled}
          placeholder={
            status?.connected ? t("Enter a replacement key") : t("Paste your API key")
          }
        />
        <p className="fine">{rich("Get your key from {settings}. The key is saved on your PS5, never in browser storage or the catalogue.", {
            settings: <a href="https://torbox.app/settings" target="_blank" rel="noreferrer">{t("TorBox settings")}</a>,
          })}
        </p>
        <button type="submit" disabled={disabled || !key.trim()}>
          {status?.connected ? t("Replace connection") : t("Connect TorBox")}
        </button>
      </form>
      {status?.connected && (
        <>
          <ul>
            {status.hosts.map((host) => (
              <li key={host.sourceId}>
                {host.sourceId === "archive" ? "Archive.org" : "Vikingfile"}:{" "}
                {host.available ? t("Available") : t("Unavailable")}
                {host.maxFileSize > 0 &&
                  ` · ${t("up to {size} per file", { size: bytes(host.maxFileSize) })}`}
              </li>
            ))}
          </ul>
          <div className="dialog-actions">
            <button disabled={disabled} onClick={() => void act("refresh")}>{t("Refresh TorBox")}</button>
            <button
              disabled={disabled}
              onClick={() => setConfirmDisconnect(true)}
            >{t("Disconnect")}</button>
          </div>
        </>
      )}
      {confirmDisconnect && (
        <div className="notice">
          <p>{t("Disconnect TorBox and pause its unfinished Orbit downloads? Partial files and your TorBox account downloads will be kept.")}</p>
          <button disabled={disabled} onClick={() => void act("disconnect")}>{t("Disconnect TorBox")}</button>
          <button
            disabled={disabled}
            onClick={() => setConfirmDisconnect(false)}
          >{t("Keep connected")}</button>
        </div>
      )}
      {(error || connectionError || status?.error) && (
        <p className="form-error" role="alert">
          {t(error || connectionError || status?.error || "")}
        </p>
      )}
    </section>
  );
}
