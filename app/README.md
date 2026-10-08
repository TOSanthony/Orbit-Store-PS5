# Orbit Store for TV (native app)

Native TV app **1.2.2**, bundled with the **Orbit Store 0.9.1 service**.

A full-screen PS5 app for Orbit's storefront, drawn by the GPU instead of
the console browser. It is a client of the Orbit backend payload. Downloads,
the queue, storage and the catalogue still live in the payload, which keeps
running when the app is closed. The app talks to it on `127.0.0.1:34177` and
opens the console's own session, so no pairing code is needed.

The app carries a copy of Orbit. When nothing answers on Orbit's port, it
starts Orbit through the ELF loader the jailbreak runs on `127.0.0.1:9021`,
so a separate payload launch is unnecessary when the loader is available.

It follows the browser storefront's design and rules (`src/`)
screen for screen:

- **Discover** (the opening page): the focused game's art behind the hero,
  then Latest releases, New on Orbit and an All games grid, largest downloads first,
  with 96 games per page.
- **Browse**: search, All games or Favourites, Source,
  Format, Region, Download size and Sort by (release date, newest first, by default),
  Reset filters, and a grid of covers with each game's state.
- **Library**: the games ShadowMount reports on the console and its drives,
  with the status views, search, Location and Format; a game's details with
  Mount/Unmount and Copy/Move to drive (the same confirmations as the web);
  Storage with each drive's games and Measure game sizes; Scan for games.
- **Downloads**: Active, Finished, Failed and Cancelled, with pause, resume,
  retry, cancel (keep or delete the partial file) and remove from history.
- **Game page**: full-width artwork, the tagline and description, a primary
  download button, Favourite and About. The primary opens Download options;
  Source, Download using and Save to each open a choice panel. Back restores
  the previous control, and the enabled inner download button starts selected.
  View download / View in Library / Download another copy, storage estimates,
  metadata and Vikingfile instructions remain available.
- **App settings** (the gear in the top bar): Download sources, Storage (the
  default drive), Pair a device, Game catalogue and Updates. See below.

Connect TorBox in the browser's **App settings → Debrid**, then select
**Download using → TorBox** on a supported game's native download page.
Without an account, **Set up TorBox** opens the browser with your game, source and destination preserved. The queue labels these jobs **via TorBox**. Account connection and host/file
limits still apply. See [TorBox connection and recovery](../docs/debrid.md).

Options that download through the PS5 web browser (Vikingfile file pages)
are listed. **Open browser version** carries the selected game, download option
and save location into Orbit’s browser page. Check the selection, then choose
**Open download page on PS5** and follow the provider steps. Opening the page
does not start a transfer. If a source or drive is no longer available, choose
an available option explicitly.
This handoff requires the accompanying browser payload; older payloads open Browse.
Auto-start, payload managers and diagnostics stay in the browser version;
App settings → More settings opens it.

## App settings

A page of its own: sections on the left (Up/Down), the chosen one on the
right (Right or Cross to enter it, Circle or Left to leave).

- **Download sources**: the browser's source choice and download notice.
  On first run, Browse and Discover show **Choose sources**, which opens
  this section; saving it goes on to Discover.
- **Storage**: the drive new downloads start with under Save to
  (`POST /api/v1/storage/preferred`), or Choose automatically. Starting a
  download on another drive still makes that drive the default, as before.
  An Orbit older than this endpoint says it needs updating.
- **Pair a device**: the console's address (`http://<PS5 IP>:34177`, from
  the console session) and the six-digit code, read again whenever the
  section opens and after Orbit restarts.
- **Game catalogue**: games, catalogue revision, when it was last checked,
  and Refresh catalogue (with Orbit's cooldown).
- **Updates**: two cards, because the two parts update separately.
  - **TV app**: this app's version (from `contentVersion`, `01.000.000` is
    1.0.0), the latest release and **Update TV app**. A copy placed by hand
    in `/data/homebrew` is replaced only after **Replace with this
    release** is confirmed; a folder copy, or a copy outside
    `/data/homebrew`, is left alone with instructions. Once installed, the
    card says to close the app and open it again, and to restart the
    console if the old version still opens.
  - **Download service** (Orbit): running, saved for next start and latest
    versions; **Install update**; and **Restart download service**, after a
    confirmation, because it pauses downloads. The app asks Orbit to stop,
    waits for its port to close and four seconds more, then sends the saved
    copy to the ELF loader, as when it starts Orbit (it tries once more if
    Orbit doesn't answer within 15 seconds). Without a loader it says to run
    `/data/orbit-store/orbit_store.elf` from a payload manager, and
    reconnects by itself. A toast names the version that came back.
- **More settings**: Open browser version, and both versions side by side.

When the app opens it asks Orbit to look for updates of both parts (the
same automatic check the browser makes, with a shared six-hour cooldown).
A newer release puts a dot on the gear and on Updates and shows a toast
once. Nothing is downloaded or installed until it is chosen.

## What the console needs

- A jailbreak that loads **kstuff** (fake-signed apps) and **ShadowMountPlus**
  (mounts the app and adds its tile). The usual Relapse loader chain loads
  both. Without them the app does not start; the browser icon still works.
- An **ELF loader on port 9021**, as the jailbreak's loader chain provides,
  for the app to start Orbit. Without one, start Orbit from a payload
  manager; the app then waits for it and offers **Try again**.
- Orbit with the artwork endpoint (`/api/v1/art`, 0.6.0 and later), which the app
  brings with it. A running Orbit without it (0.5.1 and older releases) works
  too, but every cover shows its title's initial. To use the app's copy,
  stop the running Orbit (or restart the console) before opening the app.

### Starting Orbit

When the first status request is refused (nothing listens on port 34177),
the app sends Orbit to the loader once, shows **Starting Orbit…** and waits up
to 30 seconds for it to answer. It sends the copy Orbit saved in
`/data/orbit-store/orbit_store.elf`, which payload managers start and Orbit's
updater keeps current, unless the copy in the package (`/app0/orbit/`) is a
newer release or the saved one can't be read. A slow answer is not a refusal
and never triggers a start, and losing Orbit later is not answered with
another payload: the offline screen's **Try again** starts it on request.
Orbit never replaces a newer saved copy with an older one, so an older
package can't downgrade an Orbit you updated. The log records each
attempt (`[ORBIT] start: ...`).

## Install on the PS5

1. Stop a running Orbit first (App settings, Stop Orbit, in the browser
   version, or restart the console and run the jailbreak). A second copy of
   the payload does not replace a running one.
2. Optional: send `build/orbit_store.elf` yourself. The app starts its own
   copy of Orbit when none is running.
3. Copy **one** of these to the console:
   - `dist/PPSA99177.ffpkg` into `/data/homebrew/`, or
   - the folder `dist/PPSA99177/` to `/data/homebrew/PPSA99177/`.

   ShadowMountPlus registers it about ten seconds after the copy finishes.
4. Open **Orbit Store** from the Games row. The icon is the Solid Planet; the
   browser shortcut keeps its own icon in Media.
5. Discover opens first. If Orbit is not running you see
   **Starting Orbit…**, then Discover. If the app can't start it you see
   "Orbit isn't running" with the reason; start Orbit from a payload manager
   and the app reconnects by itself, or press **Try again**.

Title ID `PPSA99177`, content version `01.002.002`. To update, replace the
image or folder and, as ShadowMountPlus stages `param.json` and the icon
once, also replace its staged copies if the tile text or icon changed.

Keep `downloadDataSize` in `sce_sys/param.json` at 256, the kit's package value.

The app writes only its own log (`/download0/hui/dev/app.log` inside its
sandbox; `/mnt/sandbox/PPSA99177_000/download0/hui/dev/app.log` over FTP
while it runs). Close it with the PS button and Close Application.

## Interface language

Orbit follows the PS5 system language when the app starts. English, German, Spanish, French, Italian, Dutch, Polish, Brazilian Portuguese, Russian and Turkish are supported; other languages use English. Close and reopen the app after changing the console language. The language preference in the browser version applies only to that browser.

The interface translations share `i18n/` with the browser. The build packages them under `assets/i18n/`. Game names and descriptions keep the catalogue’s original text. For translation checks, see [BUILDING.md](../BUILDING.md#interface-languages).

## Controls

| Button | Does |
| --- | --- |
| D-pad / left stick | Move the focus. Up from a screen reaches the top bar. |
| Cross | Select |
| Circle | Back: leaves a game page, closes the keyboard or a dialog |
| L1 / R1 | Previous / next tab |
| Square | In search: delete. In Browse's field: clear the search. In Library: cancel a running copy or move. On a game page: About. |
| Triangle | In search: space. In the Browse grid: search again. On a game page: toggle Favourite. |

Dropdowns (filters, Save to, a copy's destination) open with Cross; move with
the D-pad, choose with Cross, close with Circle.

## Build

Everything runs in the app's build image (Ubuntu 24.04 with Clang 18,
Mesa, .NET 8). On Apple silicon the image adds the x86-64 compiler builtins
from Ubuntu's archive at a pinned checksum.

Build Orbit first: the package carries `build/orbit_store.elf` (see
[BUILDING.md](../BUILDING.md)). The commands mount the repository root so the
app build can read it.

```sh
docker build -t orbit-app-build:0.1 app
docker run --rm -v "$PWD:/work" -w /work/app orbit-app-build:0.1 make ffpkg   # dist/PPSA99177{,.ffpkg,.zip}
docker run --rm -v "$PWD:/work" -w /work/app orbit-app-build:0.1 make preview # build/preview/pictures/*.png
docker run --rm -v "$PWD:/work" -w /work/app orbit-app-build:0.1 make test    # unit tests, sanitizers on
```

`ORBIT_PAYLOAD=<path>` bundles another build of Orbit, and
`ORBIT_PAYLOAD=none` builds an app without one, which only waits for Orbit.
The version written next to the copy comes from its `Orbit-Store/<version>`
user agent.

The first build fetches pinned dependencies into `app/.deps/` (ignored): the
PS5 payload SDK v0.42, the ps5-opengl 1.0.0 SDK, PacBrew v0.40.2 (for
libwebp), zlib 1.3.2, GoogleTest 1.17.0 and, for `.ffpkg`, UFS2Tool. Each is
checked against a SHA-256 or commit in `tools/`.


### Previews without a console

`make preview` runs the real app code on the PC (Mesa, surfaceless EGL) with
scripted controller input and writes a picture per step. It uses a built-in
stand-in backend with invented games and generated art (`host/fixture.cpp`).
To preview against a running Orbit backend instead, run the desktop backend
and give the preview its network:

```sh
docker run -d --name orbit-tv-backend -v "$PWD:/work" --entrypoint /work/build/orbit-host \
  orbit-build:0.1 --state /work/build/live-state
docker run --rm --network container:orbit-tv-backend -v "$PWD:/work" -w /work/app \
  -e ORBIT_BACKEND=127.0.0.1:34177 orbit-app-build:0.1 bash tools/preview.sh build/preview/live
```

Choose sources once in that backend (as on first run) or the app shows its
setup screen. Pictures are the PC's rendering; frame rate, sound and
controller feel are console checks.

## How it is built

```
src/orbit/       the app: API client, state sync, artwork, screens
src/main.cpp     console entry point
host/            PC preview and its stand-in backend
src/gfx, ui, core, audio, platform, runtime, third_party
                 the ps5-homebrew-ui kit (see Provenance)
tooling/, tools/ the native PS5 build: compile, link, FSELF signing, packaging
```

- **Starting Orbit.** `orbit/starter` picks the copy to send and streams it
  to the loader; `orbit/store` decides when (see Starting Orbit above).
- **Network.** `orbit/http` is a small HTTP/1.1 client for the loopback
  backend. `orbit/store` polls status, sources, the catalogue (only when its
  revision changes), the queue, storage, favourites and the Library (Orbit
  answers that from its cache and limits ShadowMount reads itself) on one
  worker thread (every second while Downloads is open, else every three)
  and carries out actions in order. Library drives are read only while the
  Library needs them. Screens read a snapshot; nothing on the frame waits
  for the network.
- **Rules.** `orbit/model` mirrors the browser's: grouping, Latest releases,
  Browse's filters and order (`src/browse.ts`), and where an option stands
  (`src/collection.ts`: an unfinished download, an exact Library copy, a
  finished download, or a related copy).
- **Artwork.** `GET /api/v1/art/<gameId>/<cover|hero>` on the backend fetches
  the catalogue's own HTTPS URL on its workers and caches it (see
  [the API](../docs/api.md)). The app decodes WebP with libwebp (scaled while
  decoding) and PNG/JPEG with stb_image, at drawing size, on its own worker,
  then uploads at most two textures per frame. The heap is fixed at 128 MiB,
  so at most 64 covers and 4 banners stay resident.
- **Look.** The browser storefront's tokens and measurements (`src/styles.css`
  at 1920 x 1080, 1 rem = 20 px) in Inter, with the kit's SDF renderer: the
  same header, pill tabs, 3 px white focus ring with its glow behind the
  element, covers with names and dates, dropdowns, dialogs and veils over
  the art. The mark is rasterised on the CPU at start-up so its cut-outs
  work over art.

## Installing from Orbit

Once a release carries the app, people install it from the browser version:
**App settings → TV app → Install on this PS5**, and update it there or in
the app itself (**App settings → Updates**). Orbit downloads the image,
checks it and places it in `/data/homebrew/` (see the TV app installer in
[docs/api.md](../docs/api.md)). The same panel links to the latest GitHub
release for installing by hand.

Orbit finds the app through `tv-app.json` at the root of the public
repository's `main` branch:

`npm run release:package` generates the exact feed. It contains `name`,
`filename`, the app's own semantic `version` (1.2.2), the official FFPKG
release `url`, its SHA-256 `checksum` and byte `size`. The app version is
derived from content version `01.002.002`; the release/service version is
0.9.1. Do not hand-copy example hashes or sizes into the live feed.

Upload `dist/PPSA99177.ffpkg` to the release first, then publish the feed.
Until the feed exists, the panel says the app hasn't been published yet.
The package command generates the feed with the exact image size and checksum.

## Provenance and licences

The app is GPL-3.0-or-later, like Orbit.

- `src/gfx`, `src/ui` (with `components`), `src/core`, `src/audio`,
  `src/platform`, `src/runtime`, `src/third_party/stb/{vorbis.c,stb_vorbis.inc}`,
  `tooling/`, `runtime/`, `host/platform_host.cpp`, `third_party/stb/` and
  most of `tools/` are from
  [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui) at
  commit `4bd942579dd981b3df9c740438489ca6a614ddc0` (GPL-3.0-or-later;
  BlackBearReloaded; its native tooling derives from SharpProspero, GPL-3.0).
  They are unmodified except `tools/bake-fonts.sh` (Orbit's font table),
  `tools/build-tests.sh` (links libwebp, adds `host/`), `tools/build.sh`
  (copies the staged Orbit payload into the package) and the new
  `tools/preview.sh` and `tools/stage-payload.sh`.
- `assets/audio/sfx/glass` are the kit's interface sounds (BlackBearReloaded,
  distributed under the kit's licence).
- Inter 4.1 (`third_party/fonts`, SIL Open Font License 1.1); the baked atlases
  in `assets/fonts` carry the licence beside them.
- `src/third_party/stb/stb_image.inc` is stb_image (public domain or MIT) at
  the kit's pinned stb commit.
- Linked at build time, not stored here: the ps5-opengl SDK
  (GPL-3.0-or-later, with Mesa and OpenGNM PSBC under their own licences),
  libwebp 1.4.0 from PacBrew (BSD-3-Clause, `../licenses/libwebp-*`), and the
  kit's clean-room `libc.prx` shim generated from `tooling/`.
- Carried in the package: Orbit's own `orbit_store.elf`, with the components
  listed in [THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md).

`npm run release:package` includes the exact app and payload source, the full
ps5-opengl release with its source archives and patches, both payload SDK
versions, the linked WebP and C++ runtime sources, and their notices. See
[THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md) and
[BUILDING.md](../BUILDING.md) for the inventory and rebuild instructions.
