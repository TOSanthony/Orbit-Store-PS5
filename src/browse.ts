import type { Game } from "./types";
export const CATALOG_PAGE_SIZE = 96;
export interface BrowseFilters {
  query: string;
  source: string;
  format: string;
  region: string;
  size: string;
  collection: "all" | "favorites";
  sort: string;
}
export const defaultFilters: BrowseFilters = {
  query: "",
  source: "",
  format: "",
  region: "",
  size: "",
  collection: "all",
  sort: "release",
};
// Choose catalogue additions first, then display that selection by game release date.
export function newlyAddedGames(games: Game[]): Game[] {
  return games
    .map((game) => ({ game, added: Date.parse(game.addedAt || "") }))
    .filter(({ added }) => Number.isFinite(added))
    .sort((a, b) => b.added - a.added || a.game.title.localeCompare(b.game.title))
    .slice(0, 20)
    .map(({ game }) => game)
    .sort((a, b) => (b.releaseDate || "").localeCompare(a.releaseDate || "") || a.title.localeCompare(b.title));
}
export function browseGames(
  games: Game[],
  filters: BrowseFilters,
  favorites: ReadonlySet<string>,
): Game[] {
  const query = filters.query.trim().toLowerCase();
  const matches = games.flatMap((game) => {
    if (
      query &&
      !`${game.title} ${game.releases.map((r) => r.titleId).join(" ")}`
        .toLowerCase()
        .includes(query)
    )
      return [];
    if (filters.collection === "favorites" && !favorites.has(game.id))
      return [];
    const releases = game.releases.filter((r) => {
      if (filters.source && r.sourceId !== filters.source) return false;
      if (filters.format && r.format !== filters.format) return false;
      if (filters.region && (r.region || "unknown") !== filters.region) return false;
      const gb = r.sizeBytes / 1e9;
      return (
        !filters.size ||
        (filters.size === "small"
          ? gb < 10
          : filters.size === "medium"
            ? gb >= 10 && gb < 50
            : filters.size === "large"
              ? gb >= 50 && gb < 100
              : gb >= 100)
      );
    });
    return releases.length
      ? [{ game, size: Math.min(...releases.map((r) => r.sizeBytes)) }]
      : [];
  });
  matches.sort((a, b) => {
    const title = a.game.title.localeCompare(b.game.title);
    switch (filters.sort) {
      case "title-desc":
        return -title;
      case "release":
        return (
          (b.game.releaseDate || "").localeCompare(a.game.releaseDate || "") ||
          title
        );
      case "size":
        return a.size - b.size || title;
      case "size-desc":
        return b.size - a.size || title;
      default:
        return title;
    }
  });
  return matches.map((x) => x.game);
}
