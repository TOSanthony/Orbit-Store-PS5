import type { Game, Job, LibraryGame, Release, Storage } from "./types";

export function downloadSpacePlan(
  release: Release,
  drive: Storage,
  jobs: Job[],
) {
  const planned = jobs.some(
    (job) =>
      job.releaseId === release.id &&
      job.storageId === drive.id &&
      job.status !== "complete" &&
      job.status !== "cancelled",
  );
  // Download again starts from zero. A cancelled record is not reusable data:
  // its file may have been deleted manually or belong to a disconnected drive.
  const after = drive.projectedFreeBytes - (planned ? 0 : release.sizeBytes);
  return { planned, after, enough: after >= 16 * 1024 ** 2 };
}

export interface CollectionState {
  // English, marked for the catalogue; translated where it is shown.
  label: string;
  kind: "download" | "library" | "related";
  job?: Job;
  libraryGame?: LibraryGame;
}
const rank: Record<Job["status"], number> = {
  downloading: 0,
  verifying: 1,
  queued: 2,
  retrying: 3,
  paused: 4,
  error: 5,
  complete: 6,
  cancelled: 7,
};
const labels: Record<Job["status"], string> = {
  downloading: /* i18n */ "Downloading",
  verifying: /* i18n */ "Verifying",
  queued: /* i18n */ "Queued",
  retrying: /* i18n */ "Retrying",
  paused: /* i18n */ "Paused",
  error: /* i18n */ "Needs attention",
  complete: /* i18n */ "Downloaded",
  cancelled: /* i18n */ "Cancelled",
};
export function releaseState(
  release: Release,
  jobs: Job[],
  games: LibraryGame[],
): CollectionState | undefined {
  // A catalogue revision must not relabel a saved job as a different file.
  const job = jobs
    .filter(
      (j) =>
        j.releaseId === release.id &&
        j.titleId === release.titleId &&
        j.format === release.format &&
        j.filename === release.filename &&
        j.total === release.sizeBytes &&
        j.status !== "cancelled",
    )
    .sort((a, b) => rank[a.status] - rank[b.status])[0];
  if (job && job.status !== "complete")
    return { label: labels[job.status], kind: "download", job };
  const candidates = games.filter(
    (g) =>
      g.titleId === release.titleId &&
      g.platform === "ps5" &&
      (g.onDrive || g.installed),
  );
  // ShadowMount does not expose a trustworthy version. Do not suppress a versioned
  // option on the strength of its title ID, a display name or an old download record.
  const exact = candidates.find(
    (g) =>
      g.onDrive &&
      g.format === release.format &&
      !release.version &&
      g.path.split("/").pop() === release.filename &&
      (g.sizeBytes === null || g.sizeBytes === release.sizeBytes),
  );
  if (exact)
    return {
      label: /* i18n */ "In Library",
      kind: "library",
      libraryGame: exact,
    };
  if (job) return { label: /* i18n */ "Downloaded", kind: "download", job };
  if (candidates.length)
    return {
      label: /* i18n */ "Related copy in Library",
      kind: "related",
      libraryGame: candidates[0],
    };
}
export function gameState(
  game: Game,
  jobs: Job[],
  library: LibraryGame[],
): CollectionState | undefined {
  const states = game.releases
    .map((r) => releaseState(r, jobs, library))
    .filter((s): s is CollectionState => !!s);
  return (
    states.find((s) => s.job && s.job.status !== "complete") ||
    states.find((s) => s.kind === "library") ||
    states.find((s) => s.kind === "download") ||
    states[0]
  );
}
