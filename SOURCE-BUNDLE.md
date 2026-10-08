# Orbit Store source and licence bundle

This bundle supplies the application source for both `orbit_store.elf` and `PPSA99177.ffpkg`, plus the corresponding sources and notices for their copyleft components and other linked libraries. `SOURCES.json` records the exact application commit and every upstream archive's URL and SHA-256. `SOURCE-SHA256SUMS` covers every source-bundle input.

Catalogue snapshots are included as `catalog/games.enc`, `catalog/releases.enc` and `catalog/catalog.enc`. All readers, required decoding material and build tools are included, so the source can build and modify the application and catalogue. See [BUILDING.md](BUILDING.md) for compiler and OpenSSL prerequisites.

## Contents

- `orbit-store-<version>-source.tar.gz`: Orbit's C backend, React browser interface, native C/C++ TV app, vendored UI kit, clean-room runtime shim, FSELF tooling, original app assets, and build scripts. Includes the UI kit's GPL sources, stb sources and Inter font licence.
- `ps5-payload-sdk-d9c9519.tar.gz`: SDK v0.43 used by the payload.
- `ps5-payload-sdk-4eb7012.tar.gz`: SDK v0.42 used by the TV app.
- `libmicrohttpd-1.0.1.tar.gz`, `curl-8.18.0.tar.xz`, `openssl-3.5.2.tar.gz`: the payload's linked HTTP and TLS libraries.
- `ps5-opengl-sdk-1.0.0.tar.gz`: the complete upstream graphics SDK release, including its `sources/` archives, dependency revisions, patches, build tools, notices and licence texts. Includes ps5-opengl, Mesa 26.2.0, OpenGNM, PSBC, SPIR-V and Vulkan headers. Upstream sample sources are preserved too; their presence does not mean Orbit links those samples.
- `libwebp-1.4.0.tar.gz` and `pacbrew-repo-1687e82.tar.gz`: libwebp/libsharpyuv source and the matching PacBrew v0.40.2 build recipes. Orbit does not bundle the entire PacBrew ports collection.
- `libcxx-18.1.8.src.tar.xz`, `libcxxabi-18.1.8.src.tar.xz`, `libunwind-18.1.8.src.tar.xz`, `cmake-18.1.8.src.tar.xz`: the native app's C++ runtime sources and CMake modules, built by the v0.42 SDK's `libcxx.sh` recipe.
- `compiler-rt-18.1.3.src.tar.xz`: compiler builtins used by the native app's pinned x86-64 Ubuntu compiler-runtime archive. Its Apache licence with LLVM exceptions is included.
- `zlib-1.3.2.tar.gz`: the native build tool's compression dependency.
- `build-inputs/cjson/` and `build-inputs/cacert.pem`: the exact cJSON files and CA certificate data used to build the payload.
- `LICENSE`, `licenses/` and `THIRD-PARTY-NOTICES.md`: application and component terms.

## Rebuild or modify

Verify `SOURCE-SHA256SUMS`, unpack the application archive, then follow its `BUILDING.md` and `app/README.md`. The application version and TV app content version are separate. The package build carries the payload built first; `app/tools/verify-package.sh` extracts the final FFPKG and checks that its payload, version, app executable and metadata match the intended inputs.

To use the exact cached bootstrap inputs, copy `build-inputs/cjson/` to the application's `.deps/cjson/` and `build-inputs/cacert.pem` to `.deps/cacert.pem` before running `tools/bootstrap.py`. That script checks the pinned cJSON hashes. Production browser dependencies are locked in `package-lock.json` and fetched by `npm ci`; the source and licence notices identify the embedded runtime components.

To modify a linked library, unpack its source and use the matching build recipe in the application or upstream SDK. For libmicrohttpd, curl and OpenSSL, see `tools/build-deps.sh`. For the native C++ runtimes, use the v0.42 SDK's `libcxx.sh`. For libwebp, use `libwebp/PKGBUILD` in the included PacBrew source. For the graphics stack, unpack the full ps5-opengl archive and unpack `sources/ps5-opengl.tar` and follow its `docs/building.md` and `dependencies.json` (`make source-fetch`, then `make sdk-gl46`); the other `sources/` archives preserve matching dependencies. Retain its patches and dependency revisions.

`app/tools/prepare-opengl.sh` creates the link group from the SDK's `libPS5OpenGL.a`, libc++, libc++abi, libunwind and compiler builtins. For a modified graphics SDK, replace the SDK in `app/.deps/ps5-opengl/` and update the pinned manifest in `app/tools/fetch-opengl-sdk.sh` to your modified build before rebuilding the app. For modified WebP libraries, replace `libwebp.a` and `libsharpyuv.a` in the PacBrew prefix used by `app/tools/setup-pacbrew-dependencies.sh`. Clear the affected app build outputs when replacing libraries.

The source bundle is not an offline mirror of every build tool. Docker image setup and dependency bootstrap may require internet access. It includes the application build scripts and the corresponding sources of the linked copyleft components so they can be rebuilt or changed. No game files or game artwork are bundled.
