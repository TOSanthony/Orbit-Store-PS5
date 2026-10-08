export interface UpdateStatus {
  available: boolean;
  updateAvailable: boolean;
  automaticCheckAfter: number;
  runningVersion: string;
  savedVersion: string;
  latestVersion: string;
  checksum: string;
  phase: string;
  busy: boolean;
  error: string;
  restartRequired: boolean;
  received: number;
  retryAt: number;
  checkedAt: number;
  checkAfter: number;
}
