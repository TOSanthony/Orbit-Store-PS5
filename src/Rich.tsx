import { Fragment, type ReactNode } from "react";
import { t } from "./i18n";

// A translated sentence with styled parts, so word order can change:
// rich("Open {downloads} to follow progress.", { downloads: <strong>{t("Downloads")}</strong> })
export function rich(
  text: string,
  parts: Record<string, ReactNode | string | number>,
): ReactNode {
  return t(text)
    .split(/\{(\w+)\}/g)
    .map((piece, index) =>
      index % 2 ? (
        <Fragment key={index}>{parts[piece] ?? `{${piece}}`}</Fragment>
      ) : (
        piece
      ),
    );
}
