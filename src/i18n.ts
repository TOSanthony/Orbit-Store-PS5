// Orbit's interface language. The English text is the key: a missing
// translation shows the English, and fixed messages from Orbit's service on
// the PS5 translate the same way. Catalogues live in i18n/<code>.json, shared
// with the TV app.
export type Language =
  "en" | "de" | "es" | "fr" | "it" | "nl" | "pl" | "pt-BR" | "ru" | "tr";

// Each language under its own name, so people can find theirs.
export const languages: { code: Language; name: string }[] = [
  { code: "en", name: "English" },
  { code: "de", name: "Deutsch" },
  { code: "es", name: "Español" },
  { code: "fr", name: "Français" },
  { code: "it", name: "Italiano" },
  { code: "nl", name: "Nederlands" },
  { code: "pl", name: "Polski" },
  { code: "pt-BR", name: "Português (Brasil)" },
  { code: "ru", name: "Русский" },
  { code: "tr", name: "Türkçe" },
];

// A plural entry has a form per CLDR category; the English key is the
// "other" form passed to tn().
type Entry = string | Partial<Record<Intl.LDMLPluralRule, string>>;
export type Catalogue = Record<string, Entry>;
type Vars = Record<string, string | number>;

// Vite splits each catalogue into its own file, so only one is downloaded.
const catalogues: Record<
  Exclude<Language, "en">,
  () => Promise<{ default: Catalogue }>
> = {
  de: () => import("../i18n/de.json"),
  es: () => import("../i18n/es.json"),
  fr: () => import("../i18n/fr.json"),
  it: () => import("../i18n/it.json"),
  nl: () => import("../i18n/nl.json"),
  pl: () => import("../i18n/pl.json"),
  "pt-BR": () => import("../i18n/pt-BR.json"),
  ru: () => import("../i18n/ru.json"),
  tr: () => import("../i18n/tr.json"),
};

const storageKey = "orbit.language.v1";
let current: Language = "en";
let table: Catalogue = {};
let plurals = new Intl.PluralRules("en");
let numbers = new Intl.NumberFormat("en");

function known(code: string | null | undefined): Language | undefined {
  return languages.find((language) => language.code === code)?.code;
}

// The closest supported language for a browser tag: de-AT is German, and
// any Portuguese is Brazilian Portuguese.
export function matchLanguage(tag: string): Language | undefined {
  const lower = tag.toLowerCase();
  if (lower.startsWith("pt")) return "pt-BR";
  return known(lower.split("-")[0]);
}

// The language chosen in App settings on this device, or "" to follow the browser.
export function savedLanguage(): Language | "" {
  try {
    return known(localStorage.getItem(storageKey)) || "";
  } catch {
    return "";
  }
}

// The browser's own preference, ignoring any choice saved in Orbit.
export function browserLanguage(): Language {
  for (const tag of navigator.languages?.length
    ? navigator.languages
    : [navigator.language]) {
    const match = tag && matchLanguage(tag);
    if (match) return match;
  }
  return "en";
}

export function detectLanguage(): Language {
  return savedLanguage() || browserLanguage();
}

// Switches to a catalogue already in hand; loadLanguage fetches it first.
export function applyCatalogue(code: Language, catalogue: Catalogue) {
  table = catalogue;
  current = code;
  plurals = new Intl.PluralRules(code);
  numbers = new Intl.NumberFormat(code);
}

// Called once before the first render.
export async function loadLanguage(code: Language = detectLanguage()) {
  try {
    applyCatalogue(
      code,
      code === "en" ? {} : (await catalogues[code]()).default,
    );
  } catch {
    // A catalogue that cannot load leaves the interface in English.
    applyCatalogue("en", {});
  }
  document.documentElement.lang = current;
}

export function language(): Language {
  return current;
}

// Saves the choice ("" follows the browser) and reloads into it.
export function setLanguage(code: Language | "") {
  try {
    if (code) localStorage.setItem(storageKey, code);
    else localStorage.removeItem(storageKey);
  } catch {
    /* Without storage the choice lasts until the page closes. */
  }
  void loadLanguage(code || detectLanguage()).then(() =>
    window.location.reload(),
  );
}

function fill(text: string, vars?: Vars) {
  if (!vars) return text;
  return text.replace(/\{(\w+)\}/g, (match, name: string) =>
    name in vars
      ? typeof vars[name] === "number"
        ? numbers.format(vars[name] as number)
        : String(vars[name])
      : match,
  );
}

// t("Download to PS5"); t("Saves to {path}", { path }).
export function t(text: string, vars?: Vars): string {
  const entry = table[text];
  return fill(typeof entry === "string" ? entry : text, vars);
}

// tn(count, "{count} game", "{count} games"): {count} is filled in.
export function tn(count: number, one: string, other: string, vars?: Vars) {
  const entry = table[other];
  const all = { ...vars, count };
  if (entry && typeof entry === "object")
    return fill(entry[plurals.select(count)] ?? entry.other ?? other, all);
  return fill(count === 1 ? one : other, all);
}

export function formatNumber(
  value: number,
  options?: Intl.NumberFormatOptions,
) {
  return options
    ? new Intl.NumberFormat(current, options).format(value)
    : numbers.format(value);
}

export function formatDate(
  value: Date | number,
  options?: Intl.DateTimeFormatOptions,
) {
  return new Intl.DateTimeFormat(current, options).format(value);
}

// Catalogue genres combine names: "Adventure / Action".
export function genreName(genre: string) {
  return genre
    .split(" / ")
    .map((part) => t(part))
    .join(" / ");
}

// Drive and location names from Orbit and ShadowMount, some numbered.
export function placeName(name: string) {
  const usb = /^USB storage (\d+)$/.exec(name) || /^USB (\d+)$/.exec(name);
  if (usb) return t("USB storage {number}", { number: usb[1] });
  const extended = /^Extended storage (\d+)$/.exec(name);
  if (extended) return t("Extended storage {number}", { number: extended[1] });
  return t(name);
}
