# Building Orbit Store 0.9.1

Orbit 0.9.1 fixes internal and M.2 storage discovery after 0.9.0 and checks actual write access when downloads start or resume. See [TorBox setup](https://github.com/saawant12/orbit-store-ps5/blob/main/guides/downloads.md#optional-download-through-torbox) for account and queue compatibility.

This source builds the Orbit **0.9.1** service/browser payload and the native TV app **1.2.2** (`PPSA99177`, content version `01.002.002`). The TV app includes the payload built from this tree. Game metadata and external artwork URLs come from the included catalogue snapshot; builds do not fetch game artwork.

Requirements: Node.js 22+, Python 3, a host C compiler, OpenSSL development files and Docker. On macOS use the Command Line Tools and `brew install openssl@3`; the build container already supplies Clang and OpenSSL. Run host Python tools through a project virtual environment. Catalogue tools compile a small host helper against OpenSSL; no extra Python packages are required.

The source archive includes `catalog/games.enc`, `catalog/releases.enc` and `catalog/catalog.enc`, together with all readers, build tools and required decoding material. `catalog:check` and `build` read these inputs directly. After editing catalogue inputs, run `npm run catalog:build` and commit the updated snapshots. Ordinary rebuilds reuse unchanged snapshot bytes.

The shared service supplies catalogue data to both interfaces. Existing catalogue caches are migrated automatically. Queues, pairing state and queue backups keep their existing persistence format; TorBox job compatibility is described in [TorBox setup](https://github.com/saawant12/orbit-store-ps5/blob/main/guides/downloads.md#optional-download-through-torbox). Orbit 0.9.1 uses the same catalogue feed as 0.9.0.

## Payload and browser interface

```sh
python3 -m venv .venv
.venv/bin/python tools/bootstrap.py
npm ci
npm run build
docker build -f tools/Dockerfile.sdk -t orbit-sdk:0.43 .
docker build -t orbit-build:0.1 .
docker run --rm --cpus 2 --memory 6g -v "$PWD:/work" orbit-build:0.1 payload
```

The result is `build/orbit_store.elf`. It carries `build/orbit_runtime.elf`, the runtime saved on the console. Run `npm run build` before rebuilding the payload whenever the browser interface or catalogue changes.

The source bundle includes the exact cJSON and CA files used for the published build. To reuse them, copy its `build-inputs/cjson/` and `build-inputs/cacert.pem` into `.deps/` before bootstrap.

To rebuild with modified libmicrohttpd, curl or OpenSSL, unpack the included source archive, adapt `tools/build-deps.sh` to use it, rebuild `orbit-build:0.1`, and rebuild the payload. The same source permits relinking the statically linked libmicrohttpd.

## Native TV app

Build the payload first, then:

```sh
docker build -t orbit-app-build:0.1 app
docker run --rm --cpus 2 --memory 6g -v "$PWD:/work" -w /work/app -e BUILD_JOBS=2 orbit-app-build:0.1 make ffpkg
docker run --rm --cpus 2 --memory 6g -v "$PWD:/work" -w /work/app orbit-app-build:0.1 bash tools/verify-package.sh
```

Outputs are `app/dist/PPSA99177.ffpkg`, the unpacked app folder and its ZIP. The verification command extracts the actual FFPKG and compares its bundled ELF, version, executable and metadata with the build inputs. In an unpacked source archive without Git metadata, verification uses the commit in the adjacent source bundle's `SOURCES.json` (or pass `ORBIT_SOURCE_COMMIT` explicitly).

The app uses pinned ps5-opengl 1.0.0, PS5 SDK v0.42, PacBrew's libwebp 1.4.0 and the LLVM runtime libraries listed in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). Build tooling, dependency hashes and runtime shim source are included. See [app/README.md](app/README.md) and [SOURCE-BUNDLE.md](SOURCE-BUNDLE.md) for component rebuild instructions.

## Interface languages

The browser interface and the TV app share the catalogues in `i18n/`. English text is the key, so a string without a translation shows in English. After adding or changing interface text (`t()`, `tn()` or `rich()` in `src/`, `tr()` or `trn()` in `app/src/orbit/`, or a fixed message in `backend/`), refresh the list and see what each language is missing:

```sh
node tools/i18n.mjs extract   # rewrites i18n/en.json
node tools/i18n.mjs check     # lists missing, unused and mismatched translations
```

`npm test` fails until every language has every string with the same `{placeholders}`. The TV app follows the PS5's system language; the browser follows the browser's language unless one is chosen in App settings.

## Checks and release packaging

```sh
npm test
npm run test:catalog
docker run --rm --cpus 2 --memory 6g --network none -v "$PWD:/work" orbit-build:0.1 test-art test-tv-app test-updates
docker run --rm --cpus 2 --memory 6g --network none -v "$PWD:/work" -w /work/app orbit-app-build:0.1 make test
```

Run tests in a checkout; root development tests are omitted from the release source archive. Native app tests remain with its vendored source. None of these commands contact a console.

From a clean release commit, rebuild both binaries, run the package verification, then `npm run release:package`. The command checks release evidence, version agreement, artifact freshness, package identities and dependency source hashes. It stages both binaries, their checksums, exact source and licence bundle, release notes and both official feeds under `build/release-0.9.1/`. It never uploads or installs anything. Publish feed changes only after their binary URLs are live and their hashes verified.

For local preparation before the console smoke check, use `npm run release:package -- --review`. This creates `build/review-0.9.1/` with the remaining release gates recorded; it does not publish or waive them.
