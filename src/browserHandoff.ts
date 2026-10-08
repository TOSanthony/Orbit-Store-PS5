import type { Game } from "./types";

export type BrowserSelection = {
  gameId: string;
  releaseId: string;
  storageId: string;
};
const identifier = /^[a-zA-Z0-9_-]+$/;

// A local navigation hint, never a provider URL, credential or download command.
export function parseBrowserHandoff(hash: string): BrowserSelection | null {
  if (!hash.startsWith("#download?")) return null;
  const query = new URLSearchParams(hash.slice("#download?".length));
  const fields = ["game", "release", "storage"];
  if (
    hash.length > 256 ||
    [...query.keys()].some((key) => !fields.includes(key))
  )
    return null;
  const values = fields.map((field) => query.get(field) || "");
  if (
    fields.some((field) => query.getAll(field).length !== 1) ||
    values.some((value) => !identifier.test(value)) ||
    values[0].length > 63 ||
    values[1].length > 23 ||
    values[2].length > 63
  )
    return null;
  return { gameId: values[0], releaseId: values[1], storageId: values[2] };
}

export function handoffGame(
  selection: BrowserSelection,
  games: Game[],
): Game | null {
  const game = games.find((entry) => entry.id === selection.gameId);
  return game?.releases.some(
    (release) => release.id === selection.releaseId && release.browserAvailable,
  )
    ? game
    : null;
}
