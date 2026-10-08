import { useEffect, useState } from "react";
import { api } from "./api";
import { formatDate, t, tn } from "./i18n";
interface Status {
  available: boolean;
  busy: boolean;
  cached: boolean;
  revision: number;
  gameCount: number;
  checkedAt: number;
  checkAfter: number;
  error: string;
}
export function CatalogueUpdates() {
  const [status, setStatus] = useState<Status | null>(null);
  const [error, setError] = useState("");
  const [sending, setSending] = useState(false);
  const [now, setNow] = useState(Date.now());
  useEffect(() => {
    let active = true;
    let loading = false;
    const read = async () => {
      if (loading) return;
      loading = true;
      try {
        const next = await api<Status>("/catalog/updates");
        if (active) {
          setStatus(next);
          setError("");
        }
      } catch (e) {
        if (active) setError((e as Error).message);
      } finally {
        loading = false;
      }
    };
    void read();
    const timer = setInterval(() => {
      setNow(Date.now());
      void read();
    }, 2000);
    return () => {
      active = false;
      clearInterval(timer);
    };
  }, []);
  async function refresh() {
    setSending(true);
    setError("");
    try {
      setStatus(await api<Status>("/catalog/updates", { action: "refresh" }));
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setSending(false);
    }
  }
  const wait = Math.max(
    0,
    Math.ceil(((status?.checkAfter || 0) * 1000 - now) / 1000),
  );
  return (
    <section
      className="updates-panel"
      aria-labelledby="catalogue-updates-title"
    >
      <h2 id="catalogue-updates-title">{t("Game catalogue")}</h2>
      <p>
        {t(
          "New games arrive without reinstalling Orbit. Your PS5 checks on startup and every six hours, and keeps a saved catalogue for offline browsing.",
        )}
      </p>
      {status && (
        <p className="muted">
          {tn(status.gameCount, "{count} game", "{count} games")} ·{" "}
          {t("Catalogue {revision}", { revision: String(status.revision) })}
          {status.checkedAt
            ? ` · ${t("Last checked {time}", {
                time: formatDate(new Date(status.checkedAt * 1000), {
                  hour: "2-digit",
                  minute: "2-digit",
                }),
              })}`
            : ""}
        </p>
      )}
      <div className="dialog-actions">
        <button
          disabled={!status?.available || status.busy || sending || wait > 0}
          onClick={() => void refresh()}
        >
          {sending || status?.busy
            ? t("Refreshing catalogue…")
            : t("Refresh catalogue")}
        </button>
        {wait > 0 && !status?.busy && (
          <span className="fine">
            {t("Refresh available in {seconds} s", { seconds: wait })}
          </span>
        )}
      </div>
      {status && !status.available && (
        <p className="fine">
          {t(
            "Refresh from Orbit running on your PS5. This preview uses its bundled catalogue.",
          )}
        </p>
      )}
      {(error || status?.error) && (
        <p className="notice" role="status">
          {error || t(status?.error || "")}
        </p>
      )}
    </section>
  );
}
