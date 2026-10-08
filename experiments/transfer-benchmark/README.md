# PS5 transfer benchmark

This is an owner-approved, developer-only diagnostic, outside normal payload and
release builds. It uses `backend/transfer.c` for HTTPS, redirects, source identity,
range validation and curl scheduling, and `backend/transfer_writer.c` for the
bounded buffers, writes, flushes and durable state checkpoints.

The dedicated ELF runs alongside an idle Orbit service on port 34179. It does not
start Orbit's launcher, catalogue updater, browser capture, library scanner or
download worker. The control endpoint requires a per-build random bearer token
and rejects browser origins. It accepts curated release IDs, never arbitrary URLs.

Each sample requests a bounded prefix, retaining the actual remote file size and
ETag checks. The prefix's partitions are written into an exclusive temporary file
on internal storage. That file is unlinked before networking starts, so closing
the descriptor or exiting frees its bytes. Checkpoints live in a fresh private
`/data/orbit-speedtest-<random>` directory and never read or write the app's state.
Only that diagnostic directory's known checkpoint files are removed at shutdown.

Limits:

- 512 MiB of file data per run, no game-sized sparse files. The initial matrix
  uses a lower 128 MiB cap; longer confirmation uses the full 512 MiB cap.
- A configured transfer window of 1–90 seconds; preflight requests retain Orbit's
  30-second timeouts. Initial comparisons use 20 seconds; longer confirmation 60.
- Up to 24 runs and 20 minutes of service lifetime, with shutdown after the current
  bounded request unwinds. HTTP 500/502/504 may be retried once after at
  least 60 seconds, honoring Retry-After. Rate limits and failed validation stop
  the matrix; there are no rate-limit bypasses.
- Two, four, eight or sixteen connections, with the same 8 MiB total application
  buffer. Eight/sixteen are diagnostic-only; the app remains at four.
- Optional per-socket receive buffer requests of 256 KiB or 1 MiB and an optional
  256 KiB curl buffer. Actual socket sizes and option errors are recorded.

Results include validated per-range byte counts, serving hostnames without paths
or signed queries, DNS/connect/first-byte timings, durable throughput, file-write
and flush times, buffer waits and cleanup status. Fully downloaded prefixes also
get a SHA-256 digest; this is not verification of the full game file. Results for
time-limited samples naturally include curl's timeout/abort code. A valid timed
sample must still contain bytes and validated range responses from every handle.
The longer-run build also records network byte counts about once per second,
final-ten-second network throughput and successful final flushing. This separates
startup effects from later speed without counting buffered bytes as durable data.

## Build and execute

Create `build/generated/benchmark-secret.h` locally with a random 64-hex
`ORBIT_BENCHMARK_SECRET` and 16-hex `ORBIT_BENCHMARK_ID`. Keep it private; `build/`
is ignored. Build with the existing Docker image:

```sh
docker run --rm --network none -e BUILD_JOBS=2 \
  -v "$PWD:/work" orbit-build:0.1 \
  test-transfer-benchmark transfer-benchmark-ps5
```

After explicit console authorization:

```sh
.venv/bin/python tools/run-console-benchmark.py \
  --host 192.168.1.8 --confirm-console-execution
```

The runner reuses the owner's private pairing token only to read Orbit's queue.
It refuses active downloads or browser capture, uploads a uniquely named ELF to
Payload Manager and discovers the actual path from the changed inventory. It
launches once, runs the comparison and stops/removes only its diagnostic. Every
attempt is recorded under ignored `.state/console-tests/`; a prior attempt blocks
automatic relaunch. Queue and other-daemon preservation are checked afterward.

The initial matrix is two/four connections with default buffers, then four with
a 256 KiB TCP receive buffer, then four with both TCP and curl buffers at 256 KiB.
Each is repeated in reverse order. Eight/default and eight/best-buffer settings
are repeated next, followed by four Vikingfile samples comparing the default with
the Archive winner. Samples stop on failed validation or an unexpected failure.

`--extended` compares four/eight/sixteen connections with default and 256 KiB TCP
buffers, plus four connections with a 1 MiB TCP buffer. It repeats the seven
configurations in reverse order, then runs the four Vikingfile comparisons.

`--long-confirm` tests four/eight/sixteen with a 1 MiB TCP receive-buffer request,
twice each at 60 seconds or 512 MiB. The PS5 measured so far clamps that request to
512 KiB; results record the actual size. `--skip-viking` avoids repeating a known
invalid provider link. `--attempt N` allows a new explicitly invoked session only
after the previous attempt's shutdown and temporary-ELF removal are confirmed.

These short sequential tests include connection startup. Server caching, routing,
Wi-Fi and changing server load can affect results; they do not establish sustained
whole-file performance or a universal optimum. The test runner does not promote
its winner into the app automatically.
