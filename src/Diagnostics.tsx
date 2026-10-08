import { useRef, useState } from "react";
import { api } from "./api";
import { bytes } from "./types";
import { formatDate, t } from "./i18n";

type Event = {
  at: number;
  phase?: string;
  provider: string;
  status: string;
  reason: string;
  download: number;
  httpStatus: number;
  curlCode: number;
  result?: number;
  connections: number;
  received: number;
  total: number;
};
type Report = {
  version: string;
  generatedAt: number;
  platform: string;
  firmware: string;
  stateHealthy: boolean;
  catalogueRevision: number;
  launcher: { status: string; method: string };
  downloads: Event[];
  events: Event[];
};
// Event phases, statuses and reasons stay in English: they are codes for support.
const readable = (text: string) => text.replace(/-/g, " ");
export function Diagnostics() {
  const [report, setReport] = useState<Report | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [feedback, setFeedback] = useState("");
  const [showText, setShowText] = useState(false);
  const text = useRef<HTMLTextAreaElement>(null);
  async function load() {
    setBusy(true);
    setError("");
    setFeedback("");
    try {
      setReport(await api<Report>("/diagnostics"));
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  }
  async function copy() {
    if (!report) return;
    try {
      await navigator.clipboard.writeText(JSON.stringify(report, null, 2));
      setFeedback(
        t("Diagnostic report copied. Share it only when you choose."),
      );
    } catch {
      setShowText(true);
      setFeedback(
        t(
          "Automatic copying is unavailable here. Select the report below to copy it.",
        ),
      );
    }
  }
  const failures = report?.downloads.filter((d) => d.reason !== "none") || [];
  return (
    <section className="diagnostics-panel" aria-labelledby="diagnostics-title">
      <h2 id="diagnostics-title">{t("Diagnostics")}</h2>
      <p>
        {t(
          "Check Orbit’s status and recent download events. Reports exclude pairing codes, access tokens, download links, game names and file paths. Nothing is sent automatically.",
        )}
      </p>
      <div className="dialog-actions">
        <button disabled={busy} onClick={() => void load()}>
          {busy
            ? t("Loading report…")
            : report
              ? t("Refresh report")
              : t("View diagnostics")}
        </button>
        {report && (
          <button disabled={busy} onClick={() => void copy()}>
            {t("Copy diagnostic report")}
          </button>
        )}
      </div>
      {error && (
        <p className="form-error" role="alert">
          {error}
        </p>
      )}
      {feedback && (
        <p className="notice" role="status">
          {feedback}
        </p>
      )}
      {report && (
        <>
          <dl className="update-versions">
            <div>
              <dt>{t("Running version")}</dt>
              <dd>{report.version}</dd>
            </div>
            <div>
              <dt>{t("Platform")}</dt>
              <dd>
                {report.platform === "ps5" ? "PS5" : t("Desktop preview")}
              </dd>
            </div>
            <div>
              <dt>{t("Queue storage")}</dt>
              <dd>
                {report.stateHealthy ? t("Healthy") : t("Needs attention")}
              </dd>
            </div>
            <div>
              <dt>{t("Home-screen icon")}</dt>
              <dd>{readable(report.launcher.status)}</dd>
            </div>
          </dl>
          <h3>{t("Download errors")}</h3>
          {failures.length ? (
            <ul className="diagnostic-events">
              {failures.map((d) => (
                <li key={d.download}>
                  <strong>
                    {t("Download {id}", { id: String(d.download) })} ·{" "}
                    {d.provider}
                  </strong>
                  <span>
                    {readable(d.reason)} · {readable(d.status)}
                  </span>
                </li>
              ))}
            </ul>
          ) : (
            <p className="muted">{t("No errors in the current queue.")}</p>
          )}
          <h3>{t("Recent events")}</h3>
          <p className="fine">
            {t(
              "This session only. The copied report includes up to 64 events.",
            )}
          </p>
          <ul className="diagnostic-events">
            {report.events
              .slice(-8)
              .reverse()
              .map((e, i) => (
                <li key={`${e.at}-${i}`}>
                  <strong>
                    {e.download
                      ? `${t("Download {id}", { id: String(e.download) })} · `
                      : ""}
                    {readable(e.phase || e.status)}
                  </strong>
                  <span>
                    {formatDate(new Date(e.at * 1000), { timeStyle: "medium" })}
                    {e.download
                      ? ` · ${bytes(e.received)} / ${bytes(e.total)}`
                      : ""}
                    {e.httpStatus ? ` · HTTP ${e.httpStatus}` : ""}
                    {!e.download && e.status !== "none"
                      ? ` · ${readable(e.status)}`
                      : ""}
                    {e.result
                      ? ` · ${t("code {code}", { code: String(e.result) })}`
                      : ""}
                    {e.reason !== "none" ? ` · ${readable(e.reason)}` : ""}
                  </span>
                </li>
              ))}
          </ul>
          <button
            aria-expanded={showText}
            aria-controls="diagnostic-report"
            onClick={() => setShowText((v) => !v)}
          >
            {showText ? t("Hide report text") : t("View report text")}
          </button>
          {showText && (
            <div className="diagnostic-report">
              <label htmlFor="diagnostic-report">
                {t("Diagnostic report")}
              </label>
              <textarea
                id="diagnostic-report"
                ref={text}
                readOnly
                spellCheck={false}
                value={JSON.stringify(report, null, 2)}
              />
              <button
                onClick={() => {
                  text.current?.focus();
                  text.current?.select();
                }}
              >
                {t("Select report")}
              </button>
            </div>
          )}
        </>
      )}
    </section>
  );
}
