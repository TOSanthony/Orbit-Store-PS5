# First-run home-screen icon

`orbit_store.elf` includes this installer. On its first successful run it adds the `ORBT00001` icon, which opens the running server. It does not start the backend; payload managers do that from the saved runtime. See the [startup contract](../docs/installation.md).

## Files and recovery

- App manifest and icon: `/user/app/ORBT00001/sce_sys/param.json` and `icon0.png`. The manifest uses `deeplinkUri`, like Payload Manager's own icon; nothing is written under `/system_ex`.
- Ownership and successful-registration records: `/data/orbit-store/launcher-owned-v1` and `launcher-ready-v1`.
- Runtime embedding: `runtime_image.c` places `build/orbit_runtime.elf` inside `orbit_store.elf`; the backend saves it as `/data/orbit-store/orbit_store.elf`.

First-time setup uses the platform application-registration API. Existing verified installations skip it. The installer does not overwrite a pre-existing unknown title or a conflicting file. Partial writes are preserved for diagnosis; complete files from an owned setup can be reused when retrying failed registration.

The installer prefers registration of `ORBT00001` alone. If that native function
is unavailable, it uses `sceAppInstUtilAppInstallAll(NULL)`, matching
[Payload Manager's compatibility path](https://github.com/itsPLK/ps5-payload-manager/blob/main/src/app_installer.c).
That fallback performs a broader app-registration scan and may refresh other app
entries. Orbit still writes only its own manifest, icon, and state records. Native
installer resources are released even when an intermediate filesystem operation
fails.

The manifest URL is generated from `config/network.json`, alongside the server's port constant. An installed manifest that differs from the build is reported as an error; future port or manifest changes need a migration implementation.

`orbit_core.elf` is available for server-only diagnostics and does not include this installer.

## Validation

`GET /api/v1/system` reports `launcherStatus`, `launcherError`, and the selected
`launcherRegistrationMethod` (`title-directory`, `app-scan`, or `not-needed`). On failure,
`launcherError` names the installer step and preserves the filesystem errno or native
registration error code. It contains no pairing code or saved-state contents.

Local tests cover first installation, repeat runs without mutation, registration retry, conflicting titles, symlinks, incomplete setup, damaged metadata, registration fallback, and native API cleanup. On 2026-10-03, an approved PS5 test completed the app-registration fallback and reported the launcher ready. The user confirmed that the Media-tab icon appears and opens Orbit. The reboot/auto-start lifecycle remains a separate acceptance test.
