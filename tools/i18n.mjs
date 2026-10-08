// Orbit's interface text, gathered for translators and checked before a build.
//
//   node tools/i18n.mjs extract   writes i18n/en.json, every English string in use
//   node tools/i18n.mjs check     fails when a language is missing or mistranslates one
//
// English text is the key. Sources: t(), tn() and rich() calls and /* i18n */
// marked strings in src/; the fixed messages Orbit's service sends (backend/*.c);
// the catalogue's genre names; and tr() calls in the TV app (app/src/orbit).
import { readFileSync, readdirSync, writeFileSync, existsSync } from "node:fs";
import { join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import ts from "typescript";

// fileURLToPath, so a checkout path with spaces or accents still resolves.
const root = fileURLToPath(new URL("..", import.meta.url));
const dir = join(root, "i18n");
export const languages = ["de", "es", "fr", "it", "nl", "pl", "pt-BR", "ru", "tr"];

function files(folder, pattern) {
  if (!existsSync(folder)) return [];
  return readdirSync(folder, { recursive: true })
    .map((name) => join(folder, name))
    .filter((name) => pattern.test(name));
}

// ---- the browser version ----
function fromScript(file, add) {
  const text = readFileSync(file, "utf8");
  const source = ts.createSourceFile(file, text, ts.ScriptTarget.Latest, true);
  const literal = (node) =>
    node && (ts.isStringLiteral(node) || ts.isNoSubstitutionTemplateLiteral(node))
      ? node.text
      : undefined;
  (function visit(node) {
    if (ts.isCallExpression(node) && ts.isIdentifier(node.expression)) {
      const name = node.expression.text;
      const [a, b, c] = node.arguments;
      if ((name === "t" || name === "rich") && literal(a) !== undefined) add(literal(a));
      if (name === "tn" && literal(b) !== undefined && literal(c) !== undefined)
        add(literal(c), { one: literal(b), other: literal(c) });
    }
    ts.forEachChild(node, visit);
  })(source);
  for (const match of text.matchAll(/\/\* i18n \*\/\s*"((?:[^"\\]|\\.)*)"/g))
    add(JSON.parse(`"${match[1]}"`));
}

// ---- C and C++ sources: string literals joined as the compiler joins them ----
const escapes = { n: 10, t: 9, r: 13, 0: 0, "\\": 92, '"': 34, "'": 39, "?": 63 };
// Escapes are bytes ("\xE2\x80\xA6" is an ellipsis in UTF-8), so decode as bytes.
function unescapeC(body) {
  const bytes = [];
  for (let i = 0; i < body.length; ) {
    if (body[i] !== "\\") {
      const point = body.codePointAt(i);
      const char = String.fromCodePoint(point);
      bytes.push(...Buffer.from(char, "utf8"));
      i += char.length;
      continue;
    }
    const rest = body.slice(i + 1);
    const hex = /^x([0-9a-fA-F]{1,2})/.exec(rest);
    const octal = /^([0-7]{1,3})/.exec(rest);
    if (hex) {
      bytes.push(parseInt(hex[1], 16));
      i += 1 + hex[0].length;
    } else if (octal) {
      bytes.push(parseInt(octal[1], 8));
      i += 1 + octal[0].length;
    } else {
      bytes.push(escapes[rest[0]] ?? rest.charCodeAt(0));
      i += 2;
    }
  }
  return Buffer.from(bytes).toString("utf8");
}
// Each run of adjacent literals with the source text before it (comments included).
function cRuns(text) {
  const pieces = [];
  for (let i = 0; i < text.length; ) {
    const c = text[i];
    if (c === "/" && text[i + 1] === "*") i = text.indexOf("*/", i + 2) + 2 || text.length;
    else if (c === "/" && text[i + 1] === "/") i = text.indexOf("\n", i) + 1 || text.length;
    else if (c === "'") i = text.indexOf("'", i + (text[i + 1] === "\\" ? 3 : 2)) + 1;
    else if (c === '"') {
      let j = i + 1;
      while (j < text.length && text[j] !== '"') j += text[j] === "\\" ? 2 : 1;
      pieces.push({ value: unescapeC(text.slice(i + 1, j)), start: i, end: j + 1 });
      i = j + 1;
    } else i++;
  }
  const runs = [];
  let last = 0;
  for (let p = 0; p < pieces.length; p++) {
    const first = pieces[p];
    let value = first.value;
    let end = first.end;
    while (p + 1 < pieces.length && /^\s*$/.test(text.slice(end, pieces[p + 1].start))) {
      value += pieces[++p].value;
      end = pieces[p].end;
    }
    runs.push({ value, start: first.start, end, before: text.slice(last, first.start) });
    last = end;
  }
  return runs;
}

const quiet =
  /\b(printf|fprintf|puts|fputs|perror|notify_console|_Static_assert|strstr|curl_slist_append|diagnostics_stage|DLOG)\b/;
// Orbit's service: fixed sentences in its responses and job states.
function fromService(file, add) {
  const text = readFileSync(file, "utf8");
  for (const run of cRuns(text)) {
    const { value, start, end } = run;
    // A literal joined to a macro (NAME, "..." NAME) is built at run time.
    if (/^\s*[A-Za-z_]/.test(text.slice(end, end + 40))) continue;
    if (!/^[A-Z][a-z']/.test(value) || !value.trim().includes(" ") || value.includes("%")) continue;
    if (/[Bb]enchmark/.test(value)) continue;
    const line = text.slice(text.lastIndexOf("\n", start) + 1, text.indexOf("\n", end));
    if (quiet.test(line)) continue;
    add(value);
  }
}

// The TV app: tr("..."), tr("... {name}", {...}), trn(count, "one", "other")
// and /* i18n */ marked literals.
function fromApp(file, add) {
  const runs = cRuns(readFileSync(file, "utf8"));
  for (let r = 0; r < runs.length; r++) {
    const run = runs[r];
    if (/\/\* i18n \*\/\s*$/.test(run.before) || /\btr\(\s*$/.test(run.before)) add(run.value);
    else if (/\btrn\([^;"]*,\s*$/.test(run.before) && r + 1 < runs.length &&
             /^\s*,\s*$/.test(runs[r + 1].before)) {
      add(runs[r + 1].value, { one: run.value, other: runs[r + 1].value });
      r++;
    }
  }
}

export function extract() {
  const keys = new Map();
  const add = (key, plural) => {
    if (!key.trim()) return;
    const known = keys.get(key);
    if (plural || !known) keys.set(key, plural || key);
  };
  for (const file of files(join(root, "src"), /\.tsx?$/)) fromScript(file, add);
  for (const file of files(join(root, "backend"), /\.c$/))
    if (!/\/main\.c$/.test(file)) fromService(file, add);
  for (const file of files(join(root, "app/src/orbit"), /\.(cpp|hpp)$/)) fromApp(file, add);
  const catalogue = JSON.parse(readFileSync(join(root, "catalog/catalog.json"), "utf8"));
  for (const row of catalogue)
    for (const genre of (row.genre || "").split(" / ")) if (genre) add(genre);
  return Object.fromEntries([...keys].sort(([a], [b]) => a.localeCompare(b, "en")));
}

const holes = (text) => new Set([...text.matchAll(/\{(\w+)\}/g)].map((m) => m[1]));
const same = (a, b) => a.size === b.size && [...a].every((x) => b.has(x));

export function check(english = extract()) {
  const problems = [];
  for (const code of languages) {
    const path = join(dir, `${code}.json`);
    const table = existsSync(path) ? JSON.parse(readFileSync(path, "utf8")) : {};
    const categories = new Intl.PluralRules(code).resolvedOptions().pluralCategories;
    const missing = Object.keys(english).filter((key) => !(key in table));
    const stale = Object.keys(table).filter((key) => !(key in english));
    if (missing.length)
      problems.push(`${code}: ${missing.length} missing, e.g. ${JSON.stringify(missing.slice(0, 3))}`);
    if (stale.length)
      problems.push(`${code}: ${stale.length} no longer used, e.g. ${JSON.stringify(stale.slice(0, 3))}`);
    for (const [key, source] of Object.entries(english)) {
      const value = table[key];
      if (value === undefined) continue;
      if (typeof source === "string") {
        if (typeof value !== "string") problems.push(`${code}: "${key}" must be text`);
        else if (!same(holes(source), holes(value)))
          problems.push(`${code}: "${key}" placeholders differ: ${JSON.stringify(value)}`);
        continue;
      }
      if (typeof value !== "object") {
        problems.push(`${code}: "${key}" needs plural forms ${categories.join(", ")}`);
        continue;
      }
      const wanted = holes(source.other);
      for (const category of categories) {
        const form = value[category];
        if (typeof form !== "string") problems.push(`${code}: "${key}" lacks the ${category} form`);
        // A singular form may say "one" in words instead of {count}.
        else {
          const got = holes(form);
          const relaxed = new Set([...wanted].filter((h) => h !== "count" || got.has("count")));
          if (!same(relaxed, got)) problems.push(`${code}: "${key}" ${category} placeholders differ`);
        }
      }
    }
  }
  return problems;
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const command = process.argv[2];
  if (command === "extract") {
    const english = extract();
    writeFileSync(join(dir, "en.json"), `${JSON.stringify(english, null, 2)}\n`);
    console.log(`i18n/en.json: ${Object.keys(english).length} strings`);
  } else if (command === "check") {
    const problems = check();
    for (const problem of problems) console.log(problem);
    if (problems.length) process.exit(1);
    console.log(`All ${languages.length} languages are complete.`);
  } else {
    console.log("usage: node tools/i18n.mjs extract|check");
    process.exit(2);
  }
}
