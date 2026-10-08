import { useEffect, useState } from "react";
import { api } from "./api";
import type { UpdateStatus } from "./updateStatus";
import { t } from "./i18n";

const dismissedKey = "orbit.dismissed-update.v1";
export function UpdateNotice({
  enabled,
  hidden,
  onViewUpdate,
}: {
  enabled: boolean;
  hidden: boolean;
  onViewUpdate: () => void;
}) {
  const [status, setStatus] = useState<UpdateStatus | null>(null);
  const [dismissed, setDismissed] = useState(() => {
    try {
      return sessionStorage.getItem(dismissedKey) || "";
    } catch {
      return "";
    }
  });
  useEffect(() => {
    if (!enabled) {
      setStatus(null);
      return;
    }
    let active = true,
      loading = false;
    let timer: ReturnType<typeof setTimeout>;
    const refresh = async () => {
      if (loading || document.visibilityState === "hidden") return;
      loading = true;
      clearTimeout(timer);
      let delay = 60_000;
      try {
        // The backend coalesces this across tabs and devices. It never installs a file.
        const next = await api<UpdateStatus>("/updates", {
          action: "check",
          automatic: true,
        });
        if (active) setStatus(next);
        if (next.busy) delay = 2000;
      } catch {
        // Connectivity and provider errors must not interrupt opening the app.
      } finally {
        loading = false;
        if (active) timer = setTimeout(() => void refresh(), delay);
      }
    };
    void refresh();
    const visible = () => {
      if (document.visibilityState === "visible") void refresh();
    };
    document.addEventListener("visibilitychange", visible);
    return () => {
      active = false;
      clearTimeout(timer);
      document.removeEventListener("visibilitychange", visible);
    };
  }, [enabled, hidden]);
  if (
    !enabled ||
    hidden ||
    !status?.updateAvailable ||
    status.latestVersion === dismissed
  )
    return null;
  const saved =
    status.savedVersion === status.latestVersion && status.restartRequired;
  return (
    <aside
      className="update-notice"
      aria-label={t("Orbit update")}
      role="status"
    >
      <div>
        <strong>
          {saved
            ? t("Orbit {version} is ready to start", {
                version: status.latestVersion,
              })
            : t("Orbit {version} is available", {
                version: status.latestVersion,
              })}
        </strong>
        <p>
          {saved
            ? t("Your update is saved. Choose when to restart Orbit.")
            : t(
                "Review the update when you’re ready. Your downloads can keep running.",
              )}
        </p>
      </div>
      <div className="update-notice-actions">
        <button onClick={onViewUpdate}>{t("View update")}</button>
        <button
          aria-label={t("Dismiss update notification")}
          onClick={() => {
            setDismissed(status.latestVersion);
            try {
              sessionStorage.setItem(dismissedKey, status.latestVersion);
            } catch {
              /* This view still dismisses. */
            }
          }}
        >
          {t("Not now")}
        </button>
      </div>
    </aside>
  );
}
