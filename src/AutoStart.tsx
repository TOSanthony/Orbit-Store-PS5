import { useCallback, useEffect, useState, type ReactNode } from "react";
import { CatalogueUpdates } from "./CatalogueUpdates";
import { TvApp } from "./TvApp";
import { Debrid } from "./Debrid";
import { Updates } from "./Updates";
import { Diagnostics } from "./Diagnostics";
import { api } from "./api";
import { Icon, Modal } from "./components";
import type { AutoStart as Status, System } from "./types";
import { t, tn } from "./i18n";
import { LanguageSetting } from "./LanguageSetting";
function Row({
  title,
  action,
  children,
}: {
  title: string;
  action?: ReactNode;
  children: ReactNode;
}) {
  return (
    <div className="storage-row autostart-row">
      <Icon name="power" />
      <div>
        <h3>{title}</h3>
        {children}
      </div>
      {action}
    </div>
  );
}
export function AutoStart({
  system,
  close,
  children,
  initialSection,
}: {
  system: System | null;
  close: () => void;
  children?: ReactNode;
  initialSection?: "debrid";
}) {
  const [status, setStatus] = useState<Status | null>(null),
    [busy, setBusy] = useState(""),
    [stopped, setStopped] = useState(false),
    [error, setError] = useState("");
  const load = useCallback(async () => {
    try {
      setStatus(await api<Status>("/autoboot"));
    } catch (e) {
      setError((e as Error).message);
    }
  }, []);
  useEffect(() => {
    void load();
  }, [load]);
  useEffect(() => {
    if (initialSection !== "debrid") return;
    const frame = requestAnimationFrame(() => {
      const section = document.querySelector<HTMLElement>(".debrid-settings");
      section?.focus({ preventScroll: true });
      section?.scrollIntoView({ block: "start" });
    });
    return () => cancelAnimationFrame(frame);
  }, [initialSection]);
  async function set(manager: string, enabled: boolean, integration = false) {
    setBusy(`${integration ? "integration-" : ""}${manager}`);
    setError("");
    try {
      setStatus(
        await api<Status>(integration ? "/integrations" : "/autoboot", {
          manager,
          enabled,
        }),
      );
    } catch (e) {
      setError((e as Error).message);
      void load();
    } finally {
      setBusy("");
    }
  }
  const toggle = (manager: string, enabled: boolean, unavailable = false) => (
    <button
      disabled={!!busy || unavailable}
      onClick={() => {
        void set(manager, !enabled);
      }}
    >
      {busy === manager ? t("Saving…") : enabled ? t("Turn off") : t("Turn on")}
    </button>
  );
  const pm = status?.payloadManager,
    hb = status?.homebrewLauncher,
    files = status?.autoloadTxt?.files || [],
    paths = files.map((f) => f.path).join(", "),
    listOn = files.length > 0 && files.every((f) => f.enabled);
  const integrationButton = (
    manager: string,
    managed: boolean,
    listed: boolean,
  ) => (
    <button
      disabled={!!busy || !!status?.preferencesError}
      onClick={() => void set(manager, !(managed && listed), true)}
    >
      {busy === `integration-${manager}`
        ? t("Saving…")
        : managed
          ? listed
            ? t("Stop syncing")
            : t("Retry setup")
          : listed
            ? t("Allow updates to this copy")
            : t("Add Orbit")}
    </button>
  );
  return (
    <Modal title={t("App settings")} close={close}>
      <h1>{t("App settings")}</h1>
      {initialSection === "debrid" && !stopped && <Debrid />}
      {children}
      <LanguageSetting />
      <Updates onStopped={() => setStopped(true)} />
      {!stopped && (
        <>
          <TvApp />
          {initialSection !== "debrid" && <Debrid />}
          <CatalogueUpdates />
          <Diagnostics />
          <h2>{t("Payload managers")}</h2>
          <p>
            {t(
              "Choose where Orbit adds a copy and keeps it up to date. Adding a copy does not turn on auto-start. Stopping sync leaves existing copies and auto-start choices in place.",
            )}
          </p>
          {system?.launcherStatus === "error" && (
            <p className="notice">
              {t(
                "The home-screen icon couldn’t be set up. Orbit still opens at port {port} on this console’s IP address.",
                { port: String(system.httpPort) },
              )}
            </p>
          )}
          {!status ? (
            !error && <p className="muted">{t("Checking your console…")}</p>
          ) : !status.available ? (
            <p className="notice">
              {t("Manager integration is set up from Orbit on your PS5.")}
            </p>
          ) : (
            <>
              {status.preferencesError && (
                <p className="notice" role="alert">
                  {t(status.preferencesError)}
                </p>
              )}
              <div className="storage-list">
                <Row
                  title="Payload Manager"
                  action={
                    pm?.installed &&
                    integrationButton("payload-manager", pm.managed, pm.listed)
                  }
                >
                  <p>
                    {!pm?.installed
                      ? t("Not found on this console.")
                      : pm.managed
                        ? pm.listed
                          ? t(
                              "Orbit keeps this copy up to date when you install an update.",
                            )
                          : t(
                              "Your choice is saved. Retry to finish adding Orbit.",
                            )
                        : pm.listed
                          ? t(
                              "An existing copy is present. Allow updates if you want Orbit to manage it.",
                            )
                          : t("Add Orbit to its payload list when you choose.")}
                  </p>
                </Row>
                {hb?.installed && (
                  <Row
                    title="Homebrew Launcher"
                    action={integrationButton(
                      "homebrew-launcher",
                      hb.managed,
                      hb.listed,
                    )}
                  >
                    <p>
                      {hb.managed
                        ? hb.listed
                          ? t("Orbit keeps this menu entry up to date.")
                          : t(
                              "Your choice is saved. Retry to finish adding Orbit.",
                            )
                        : hb.listed
                          ? t(
                              "An existing copy is present. Allow updates if you want Orbit to manage it.",
                            )
                          : t(
                              "Add Orbit to its homebrew menu when you choose.",
                            )}
                    </p>
                  </Row>
                )}
              </div>
              <h2>{t("Start automatically")}</h2>
              <p>
                {t(
                  "The Orbit Store icon opens Orbit while it’s running. Auto-start is a separate choice. Orbit leaves your manager’s global switch unchanged.",
                )}
              </p>
              <div className="storage-list">
                <Row
                  title={t("Payload Manager auto-start")}
                  action={
                    pm?.installed &&
                    toggle(
                      "payload-manager",
                      pm.enabled,
                      !pm.managed && !pm.enabled,
                    )
                  }
                >
                  <p>
                    {!pm?.installed
                      ? t("Not found on this console.")
                      : !pm.enabled
                        ? pm.managed
                          ? t(
                              "Turn on to add Orbit to the autoload list. Your manager’s global switch controls whether it starts.",
                            )
                          : t(
                              "Add Orbit under Payload managers first, then choose whether it should start automatically.",
                            )
                        : pm.switchOn
                          ? t("Starts Orbit automatically.")
                          : pm.otherEntries
                            ? tn(
                                pm.otherEntries,
                                "Orbit is on its autoload list. Turn on the global Autoload switch in Payload Manager to enable startup for Orbit and the other {count} payload on that list.",
                                "Orbit is on its autoload list. Turn on the global Autoload switch in Payload Manager to enable startup for Orbit and the other {count} payloads on that list.",
                              )
                            : t(
                                "Orbit is on its autoload list. Turn on the global Autoload switch in Payload Manager to enable startup.",
                              )}
                  </p>
                </Row>
                <Row
                  title="autoload.txt"
                  action={files.length > 0 && toggle("autoload-txt", listOn)}
                >
                  <p>
                    {!files.length
                      ? t(
                          "No autoload.txt found. Orbit won’t create one, because a new autoload.txt stops your autoloader from opening Payload Manager.",
                        )
                      : listOn
                        ? t("Starts Orbit from {paths}.", { paths })
                        : t("Turn on to add Orbit to {paths}.", { paths })}
                  </p>
                </Row>
                {status.etaHEN?.installed && (
                  <Row title="etaHEN">
                    <p>
                      {t(
                        "In the etaHEN Toolbox, add {path} as a payload and turn on auto start.",
                        { path: status.savedPath ?? "" },
                      )}
                    </p>
                  </Row>
                )}
              </div>
              <p className="fine">
                {status.saved
                  ? t("Orbit is saved at {path}.", {
                      path: status.savedPath ?? "",
                    })
                  : t(
                      "Orbit’s saved copy is missing. Run orbit_store.elf again to save it.",
                    )}
              </p>
            </>
          )}
          {error && (
            <p className="form-error" role="alert">
              {error}
            </p>
          )}
        </>
      )}
    </Modal>
  );
}
