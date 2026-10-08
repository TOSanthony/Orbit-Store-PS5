import { Select } from "./Select";
import {
  browserLanguage,
  languages,
  savedLanguage,
  setLanguage,
  t,
  type Language,
} from "./i18n";

// Language for this device. Automatic follows the browser, which on a PS5 is
// the console's own language.
export function LanguageSetting() {
  const automatic = languages.find((l) => l.code === browserLanguage())!.name;
  return (
    <section className="updates-panel language-setting">
      <h2>{t("Language")}</h2>
      <p>{t("Choose the language Orbit uses on this device.")}</p>
      <Select
        label={t("Language")}
        value={savedLanguage()}
        onChange={(value) => setLanguage(value as Language | "")}
        options={[
          {
            value: "",
            label: t("Automatic ({language})", { language: automatic }),
          },
          ...languages.map((l) => ({ value: l.code, label: l.name })),
        ]}
      />
    </section>
  );
}
