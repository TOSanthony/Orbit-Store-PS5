import { useState } from "react";
import type { BrowserSession } from "./types";
import { t } from "./i18n";

const dismissedKey = "orbit.dismissed-verification.v1";

export function BrowserNotice({
  session,
  cancel,
  openQueue,
  hidden,
}: {
  session: BrowserSession | null;
  cancel: (id: string) => Promise<void>;
  openQueue: (id?: string) => void;
  hidden: boolean;
}) {
  const [dismissed, setDismissed] = useState(() => {
    try {
      return localStorage.getItem(dismissedKey) || "";
    } catch {
      return "";
    }
  });
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<{ id: string; message: string } | null>(
    null,
  );
  if (!session?.id || hidden || (!session.active && dismissed === session.id))
    return null;
  function dismiss(id: string) {
    setDismissed(id);
    try {
      // The backend retains its last result. Remember this specific result
      // across reloads without hiding a new or still-active verification.
      localStorage.setItem(dismissedKey, id);
    } catch {
      /* Dismissal still works for this view if browser storage is unavailable. */
    }
  }
  const heading =
    session.state === "queued"
      ? t("Added to Downloads")
      : session.state === "failed"
        ? t("Verification couldn't finish")
        : session.state === "cancelled"
          ? t("Verification cancelled")
          : session.state === "validating"
            ? t("Checking your file")
            : t("Press Download on Vikingfile, then return to Orbit");
  return (
    <aside
      className="update-notice provider-browser-notice"
      aria-label={t("Provider verification")}
    >
      <div role="status">
        <strong>
          {heading}
          {session.title ? ` · ${session.title}` : ""}
        </strong>
        <p>{t(session.message)}</p>
        {error?.id === session.id && <p role="alert">{error.message}</p>}
      </div>
      <div className="update-notice-actions">
        {session.active ? (
          <button
            disabled={pending || session.state === "cancelled"}
            onClick={async () => {
              setPending(true);
              setError(null);
              try {
                await cancel(session.id);
              } catch (e) {
                setError({ id: session.id, message: (e as Error).message });
              } finally {
                setPending(false);
              }
            }}
          >
            {pending || session.state === "cancelled"
              ? t("Cancelling…")
              : t("Cancel verification")}
          </button>
        ) : (
          <>
            {session.state === "queued" && (
              <button
                onClick={() => {
                  openQueue(session.jobId);
                  dismiss(session.id);
                }}
              >
                {t("View download")}
              </button>
            )}
            <button onClick={() => dismiss(session.id)}>
              {t("Dismiss")}
            </button>
          </>
        )}
      </div>
    </aside>
  );
}
