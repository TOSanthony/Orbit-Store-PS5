# PS5 browser capture capability test

Experimental, not an Orbit feature or release artifact. Phone/computer browser
support is deferred at the owner's request. The first revision ran once on the
PS5 with owner approval on 2026-10-04: the page opened and the owner selected Start.
Its result disappeared at shutdown, so URL observation remains unconfirmed.
The corrected revision ran once with fresh owner approval. The owner reported
`state: marker-observed`, `markerObserved: true` and `fileRequested: true`. This
confirms the controlled URL-observation primitive on this console. Real provider
flows and further console changes still require their own validation and approval.

The unresolved question is whether Orbit can observe a selected download URL in
the PS5 browser without modifying its processes. The SDK documents opening the
browser, but supplies no documented download callback. This test checks one
possible read-only observation path before attempting provider integration.

## What running it would do

1. Start a temporary HTTP server bound only to `127.0.0.1`, on an OS-assigned port.
2. Open that local test page using `sceSystemServiceLaunchWebBrowser`.
3. Wait for the user to select **Start one-byte test**. Before that, no browser
   process memory is read. A fetch follows a redirect to a fresh random test-file
   URL and receives exactly one byte, keeping the result page open. No external
   website is contacted.
4. Identify one `SceNKNetworkProcess` or `SceNKNetworkProc` through SDK-documented
   process metadata. Those names are hypotheses from prior static research, not
   verified firmware compatibility. No match, multiple matches or invalid
   metadata ends the attempt without reading process memory.
5. Search a single pass through eligible anonymous, readable/writable,
   non-executable user-memory mappings for **that exact generated URL only**,
   in ASCII or UTF-16LE. The full mapping bytes are transiently read into a
   64 KiB buffer; unrelated browser data is not extracted, returned or saved.
   `NOCOREDUMP` regions are excluded. Process name, PID and start time are checked
   before each read and after a match. A changed process is never adopted.
6. Show a fixed status and numeric read counts. The scan has a 128 MiB attempt
   budget, roughly 2 MiB/s maximum read rate and a monotonic 85-second scan deadline.
   Cancellation and timeout are checked between bounded reads; this is not a
   guarantee that an underlying OS call cannot stall. There are no automatic
   retries. The temporary server allows five more seconds to report the final
   result, then exits by its 90-second deadline, or earlier on cancellation.

**Cancel test** stops the attempt. **Return to Orbit** navigates back to Orbit's
normal loopback address if it is already running; it does not start or restart
Orbit. A terminal result stays visible after the server exits. If the connection
ends before a terminal result arrives, the page keeps the last observed values
and explicitly says completion may be unconfirmed.

The experiment does not install an icon, save a payload-manager copy, write files,
change DNS, copy cookies, patch or terminate browser processes, or queue games.
Its native adapter calls `kernel_proc_copyout` for reads; it makes no process-memory
writes. Normal payload-loader/SDK startup behavior still applies.

## Local validation

From the repository root, with the existing Docker build image:

```sh
docker run --rm --network none \
  -v "$PWD:/work" -w /work orbit-build:0.1 test-browser-probe browser-probe
```

- 238 marker scenarios pass, including every ASCII/UTF-16 chunk split, an
  unreadable hole, changed session marker, exhausted budget and cancellation
  during a read. Undefined-behavior sanitizer traps are enabled. AddressSanitizer
  is not available in this build image.
- Four host integration tests pass for user-initiated redirect/file observation,
  Host/Origin/session rejection, cancellation, and absent/ambiguous processes.
  They simulate the native adapter and never read another process.
- The actual native implementation cross-compiles against the pinned PS5 SDK.
  Compilation does not validate PS5 process layouts or runtime behavior.
- A local Chrome check of the host simulator verified Start, the one-byte fetch,
  both success flags, and retained output after stopping the simulator. The page
  stayed open and reported no console errors. This validates the reporting fix,
  not real PS5 memory access.

Artifact: `build/orbit_browser_probe.elf`. It is an explicit Make target, outside
the normal `host` and `payload` builds. Release packaging selects
`build/orbit_store.elf` explicitly and does not pick up this test ELF.

## First console run

The original artifact had SHA-256 prefix `59e366290598`. The direct loader at
port 9021 refused the connection. Payload Manager accepted a temporary upload and
one launch; the owner confirmed that the page opened and Start was selected.
Only manager upload/launch events were available remotely, not the probe's stdout.
The owner saw an intermediate result followed by "Test ended", so neither the
marker result nor the file-request flag was recorded. Treat this as inconclusive.

The temporary ELF was deleted and no matching saved payload or named test process
remained in Payload Manager. Its empty `orbit_browser_probe` directory remained;
the FTP service checked for non-recursive empty-directory cleanup was unavailable.
Orbit and other payloads were not stopped or replaced.

## Corrected console run

Artifact SHA-256: `8f259437725eedc0224f381869985c2ff83199392ecd8a2d47c7c8c7167d4039`.
On 2026-10-04, the owner approved one further isolated run through Payload Manager.
The owner selected Start and explicitly reported `marker-observed` with both
`markerObserved` and `fileRequested` true. Those values were reported from the
console screen, not retrieved through the remote manager's logs. Byte counts and
the firmware version were not recorded.

The hash-named temporary ELF was removed after the test window and absence from
the saved payload list was verified. The previous empty test folder remains.
This was a loopback one-byte fixture: no real Vikingfile page, human verification,
signed file response, production queue handoff or game transfer was exercised.
The next step is integrating session-bound wrapper capture with Orbit's existing
file validation and queue, followed by a separately approved real-provider test.

## Required evidence before proceeding

On a separately approved console run, both `markerObserved` and `fileRequested`
must become true. A marker match only proves that the URL was present in a
network-process buffer. It does not prove a download event callback, control of
provider navigation, prevention of an unwanted browser download, Cloudflare
compatibility, or automatic return to Orbit.

If this fails, do not broaden to unrelated processes, guess private kernel offsets
or restart browser services. Record the fixed status and counts and reassess the
adapter. The next implementation step, after a successful proof, is session-bound
provider candidate validation and queue handoff. A production browser flow also
needs provider host/filename/size validation, stale-candidate rejection,
cancellation races, source consent, drive removal and resume-identity checks.

Human verification stays under the user's control. This test does not implement
or test Cloudflare/CAPTCHA handling.

References: [SDK browser sample](https://github.com/ps5-payload-dev/sdk/blob/master/samples/browser/main.c),
[SDK process sample](https://github.com/ps5-payload-dev/sdk/blob/master/samples/ps/main.c),
[SDK kernel API](https://github.com/ps5-payload-dev/sdk/blob/master/include/ps5/kernel.h).
