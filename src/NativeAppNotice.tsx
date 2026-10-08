import { memo, useState } from "react";
import { t } from "./i18n";

const dismissedKey = "orbit.native-app-notice.v1";
const releaseUrl =
  "https://github.com/saawant12/orbit-store-ps5/releases/tag/v0.6.0";

export const NativeAppNotice = memo(function NativeAppNotice({
  version,
  hidden,
}: {
  version: string | undefined;
  hidden: boolean;
}) {
  const [dismissed, setDismissed] = useState(() => {
    try {
      return localStorage.getItem(dismissedKey) === "dismissed";
    } catch {
      return false;
    }
  });

  if (!version?.startsWith("0.6.") || hidden || dismissed) return null;

  return (
    <aside
      className="update-notice"
      aria-label={t("Native PS5 app")}
      role="status"
    >
      <div>
        <strong>{t("Orbit now has a native PS5 app")}</strong>
        <p>
          {t(
            "Open Orbit from your Games row. Get the FFPKG on GitHub; requires kstuff and ShadowMountPlus.",
          )}
        </p>
      </div>
      <div className="update-notice-actions">
        <a
          className="primary notice-link"
          href={releaseUrl}
          target="_blank"
          rel="noopener noreferrer"
        >
          {t("Download from GitHub")}
        </a>
        <button
          aria-label={t("Dismiss native app notification")}
          onClick={() => {
            setDismissed(true);
            try {
              localStorage.setItem(dismissedKey, "dismissed");
            } catch {
              /* Dismiss for this view when persistent storage is unavailable. */
            }
            document
              .querySelector<HTMLElement>('.main-nav [aria-current="page"]')
              ?.focus({ preventScroll: true });
          }}
        >
          {t("Dismiss")}
        </button>
      </div>
    </aside>
  );
});
