# Startup diagnostics

The server-only trace build logs SDK initialization, import resolution, entry to
`main`, application startup stages, and the return code over UDP to a specified
private LAN address. It excludes the icon installer and auto-start setup, and
exits after 20 seconds if startup succeeds. It reads normal Orbit state and can
create the normal state directory and instance lock. The helper itself writes no
console log files. No pairing codes, tokens, or saved-state contents are logged.

Prepare the exact SDK sources locally, then build using Docker:

```sh
git clone --depth 1 --branch v0.43 https://github.com/ps5-payload-dev/sdk.git .deps/sdk-v0.43
cp -R .deps/sdk-v0.43/crt .deps/sdk-trace
docker run --rm -v "$PWD:/work" --entrypoint python3 orbit-build:0.1 \
  tools/build-startup-trace.py <receiver-lan-ip> --port 34178
```

Start a UDP receiver on that address and port **before** explicitly loading
`build/orbit_startup_trace.elf`. The build command never uploads or executes a
payload. Obtain the console owner's approval for console tests. This artifact
embeds the diagnostic receiver address and must not be distributed as a release.
Generated SDK copies, objects, and ELFs stay under ignored `.deps/` and `build/`.

The SDK source copies retain their upstream GPL notices. The script modifies
copies under `build/startup-trace`; the installed SDK and production entry code
remain unchanged. Rebuild generated UI/catalogue assets before creating the
trace, just as for the normal payload.

## First-launch failure found on 2026-10-03

The initial trace reached `main`, initialized HTTPS successfully, then returned
1 from saved-state loading. With no `state.json` present, SDK v0.43's raw `openat`
stub returned `ENOENT` (2) as a positive descriptor. Orbit consequently inspected
descriptor 2, a socket with zero size, and rejected it as invalid state.

`backend/ps5_fs.c` and the PS5 linker wrap flags now translate the kernel carry
flag into POSIX `-1` plus `errno` for `openat`, `mkdirat`, `fstatat`, `linkat`,
`renameat`, and `unlinkat`. Do not infer failure from a small positive return
value: that can be a valid descriptor. Desktop builds continue using host libc.

The diagnostic also runs `fs_probe.c`: successful directory opens/stats,
missing-path `ENOENT`, and invalid-descriptor `EBADF` on all six wrapped calls.
The latter use relative paths with descriptor -1 and cannot mutate files.
Production builds do not include this probe, UDP logging, or timed shutdown.

## Isolating the icon library

`--launcher-imports-only` builds `build/orbit_launcher_import_trace.elf`. It imports
the same `libSceAppInstUtil.sprx` entry points as the full payload, traces each SDK
module open/initialization, and returns as soon as it reaches `main`. It does not
call the installer, start Orbit, read or write Orbit state, or save its runtime.
Loading a library can itself stall before `main`; the probe cannot guarantee a
timed exit in that case. It still requires separate console-execution approval.

The fixed server-only payload passed console startup. The subsequent full-payload
test and library probe stalled while opening `libSceAppInstUtil.sprx`, before
`main`. The probe confirmed the stop point, not the exact native operation inside
that library. Stop requests were accepted, but both test PIDs remained listed.

The full build was missing the explicit `libSceIpmi.sprx` dependency used by
AppInst. The linker now retains Ipmi before AppInst, following the dependency
ordering documented in [BFpilot's build](https://github.com/ItsBlurf/BFpilot/blob/main/Makefile).
`make test-payload-imports` checks that order in the actual full and saved-runtime
ELFs, and keeps both libraries out of the server-only ELF. The corrected full
payload was loaded once on 2026-10-03: its API and app page returned HTTP 200,
state was healthy, and the user saw its startup notification. Icon setup returned
`error`. A subsequent local build added
`launcherError` to the status endpoint to identify the failing step and error code.

That build was subsequently tested after stopping only Orbit's running process.
It reported `resolve-install-title-function`, before calling AppInst initialization
or writing the launcher files. The kernel lookup returned no address for the
targeted title-registration NID. The local follow-up adds an exact-NID fallback
through the SDK runtime linker's cached symbol tables (`dlsym(RTLD_DEFAULT, ...)`).
That build kept registration scoped to `ORBT00001` without the broad
`AppInstallAll` fallback. The user subsequently uploaded and ran it manually.
Its status response confirmed `resolve-install-title-function: errno=2`, so the
cached lookup did not resolve the function either.

The lookup behavior is documented in the SDK's
[runtime linker](https://github.com/ps5-payload-dev/sdk/blob/v0.43/crt/rtld_dlfcn.c)
and [SPRX resolver](https://github.com/ps5-payload-dev/sdk/blob/v0.43/crt/rtld_sprx.c).
The next build added Payload Manager's `sceAppInstUtilAppInstallAll(NULL)`
compatibility path when neither lookup finds the targeted registration function.
This can refresh registration for other app folders and requires a separately
approved console test. The status API now reports the selected registration
method. A failure from an available targeted function does not trigger the broad
scan. Native installer resources are also released after filesystem failures.

Local platform tests cover both lookup paths, missing module handles, registration
scan success and failure, initialization errors, targeted registration errors,
filesystem failure cleanup, and already-ready installations.

The user approved the broader registration fallback and its console test. The
full build with SHA-256
`72daeb2e06a53ce9d3292cc22c37f35b491e933bd02e93a1043899cebc886294`
was uploaded into the existing Orbit entry. Only the identified old Orbit
process was stopped. The replacement started once and reported `ready`, an empty
`launcherError`, and `launcherRegistrationMethod: app-scan` after approximately
eight seconds. Follow-up status, HTML, JavaScript, and CSS requests returned HTTP
200. The user confirmed that the Media-tab icon appears and opens Orbit. This
verifies initial icon installation and opening the running app; it does not
complete the remaining release acceptance tests, such as the reboot/auto-start
lifecycle and console downloads.

Use one temporary diagnostic entry for future authorized console tests and remove
it afterward. Seven earlier diagnostic ELF files and their metadata sidecars were
removed from Payload Manager; only the normal `orbit_store.elf` entry was kept.
