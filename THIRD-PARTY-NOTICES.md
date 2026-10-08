# Third-party notices

Orbit Store is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version. See [LICENSE](LICENSE). It is distributed without any warranty.

Each release includes a source and licence bundle for both `orbit_store.elf` and `PPSA99177.ffpkg`. It contains the application source from the exact build commit, the corresponding sources of the copyleft components below, other linked library sources, and rebuild instructions. See [SOURCE-BUNDLE.md](SOURCE-BUNDLE.md) and the generated `SOURCES.json` for the archive inventory and pinned hashes.

## Components in orbit_store.elf

| Component | Version | Licence | Licence text | Source |
|---|---|---|---|---|
| PS5 payload SDK: startup code and libraries linked by its toolchain | v0.43, commit `d9c9519116944a7f1c22d262012c53b80b3520b7` | GPL-3.0-or-later (files in `include/freebsd` are BSD) | [licenses/GPL-3.0.txt](licenses/GPL-3.0.txt) | `ps5-payload-sdk-d9c9519.tar.gz` in each release; [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) |
| GNU libmicrohttpd | 1.0.1 | LGPL-2.1-or-later | [licenses/LGPL-2.1.txt](licenses/LGPL-2.1.txt) | `libmicrohttpd-1.0.1.tar.gz` in each release; [gnu.org](https://www.gnu.org/software/libmicrohttpd/) |
| curl (libcurl) | 8.18.0 | curl licence | [licenses/curl.txt](licenses/curl.txt) | `curl-8.18.0.tar.xz` in the source bundle; [curl.se](https://curl.se/download/curl-8.18.0.tar.xz) |
| OpenSSL | 3.5.2 | Apache-2.0 | [licenses/Apache-2.0.txt](licenses/Apache-2.0.txt) | `openssl-3.5.2.tar.gz` in the source bundle; [openssl.org](https://github.com/openssl/openssl/releases/tag/openssl-3.5.2) |
| cJSON | 1.7.19 | MIT | [licenses/cJSON-MIT.txt](licenses/cJSON-MIT.txt) | Exact files in `build-inputs/cjson/`; [DaveGamble/cJSON](https://github.com/DaveGamble/cJSON/tree/v1.7.19) |
| React and React DOM | 19.3.0 | MIT | [licenses/React-MIT.txt](licenses/React-MIT.txt) | [facebook/react](https://github.com/facebook/react) |
| scheduler | 0.28.0 | MIT | [licenses/scheduler-MIT.txt](licenses/scheduler-MIT.txt) | [facebook/react](https://github.com/facebook/react) |
| Vite runtime helper (module preload) | 6.4.3 | MIT | [licenses/Vite-MIT.txt](licenses/Vite-MIT.txt) | [vitejs/vite](https://github.com/vitejs/vite) |
| Mozilla CA certificate bundle, via curl's CA Extract | bundle fetched by `tools/bootstrap.py` | MPL-2.0 | [licenses/MPL-2.0.txt](licenses/MPL-2.0.txt) | Embedded unmodified; [curl.se/docs/caextract.html](https://curl.se/docs/caextract.html) |

libmicrohttpd is linked statically. Its complete source and Orbit's complete source are both provided, so you can rebuild `orbit_store.elf` with a modified libmicrohttpd using the steps in [BUILDING.md](BUILDING.md).

## Components in the TV app (`app/`, PPSA99177)

The native TV app is a separate program with its own build (`app/README.md`). It is GPL-3.0-or-later.

| Component | Version | Licence | Licence text | Source |
|---|---|---|---|---|
| ps5-homebrew-ui kit (renderer, widgets, input, sound, PS5 platform layer, native build and FSELF tooling), stored in `app/` | commit `4bd942579dd981b3df9c740438489ca6a614ddc0` | GPL-3.0-or-later; native tooling derived from SharpProspero (GPL-3.0) | [licenses/GPL-3.0.txt](licenses/GPL-3.0.txt) | In this repository; [blackbearreloaded/ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui) |
| ps5-opengl SDK (OpenGL 4.6 runtime; contains Mesa and OpenGNM PSBC components) | 1.0.0 | GPL-3.0-or-later; bundled components under their own licences (shipped in the SDK's `LICENSES/`) | [licenses/GPL-3.0.txt](licenses/GPL-3.0.txt) | `ps5-opengl-sdk-1.0.0.tar.gz` in the source bundle contains its sources, patches and dependency archives; [blackbearreloaded/ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl) |
| PS5 payload SDK headers and import stubs | v0.42, commit `4eb701204fc3f8d31e84cf8ca272974e2be9c867` | GPL-3.0-or-later and BSD-labelled files | [licenses/GPL-3.0.txt](licenses/GPL-3.0.txt) | `ps5-payload-sdk-4eb7012.tar.gz` in the source bundle |
| libwebp (WebP decoding), via PacBrew | 1.4.0 (PacBrew v0.40.2) | BSD-3-Clause, with Google's patent grant | [licenses/libwebp-BSD-3-Clause.txt](licenses/libwebp-BSD-3-Clause.txt), [licenses/libwebp-PATENTS.txt](licenses/libwebp-PATENTS.txt) | `libwebp-1.4.0.tar.gz` and matching `pacbrew-repo-1687e82.tar.gz` recipes in the source bundle |
| libc++, libc++abi and libunwind | LLVM 18.1.8, built by PS5 SDK v0.42 | Apache-2.0 with LLVM exceptions and retained legacy notices | `licenses/libcxx-LLVM.txt`, `licenses/libcxxabi-LLVM.txt`, `licenses/libunwind-LLVM.txt` | Matching LLVM source archives and CMake modules in the source bundle |
| compiler-rt builtins (x86-64) | LLVM 18.1.3, Ubuntu package `18.1.3-1ubuntu1` | Apache-2.0 with LLVM exceptions | `licenses/compiler-rt-LLVM.txt` | `compiler-rt-18.1.3.src.tar.xz` in the source bundle; pinned binary package in `app/Dockerfile` |
| stb_image, stb_vorbis | stb commit `2c980bb59875b0d32144a71867fbdebb2f77cd20` | Public domain or MIT | `app/src/third_party/stb/LICENSE` | [nothings/stb](https://github.com/nothings/stb) |
| Inter typeface | 4.1 | SIL Open Font License 1.1 | `app/third_party/fonts/Inter-LICENSE.txt` | [rsms/inter](https://github.com/rsms/inter/releases/tag/v4.1) |
| Interface sound effects (glass set) | from the kit above | GPL-3.0-or-later (BlackBearReloaded) | [licenses/GPL-3.0.txt](licenses/GPL-3.0.txt) | In this repository |

The app's package also carries Orbit's payload, `orbit_store.elf`, so it includes every component listed above for the payload.

The complete ps5-opengl archive carries ps5-opengl, Mesa 26.2.0, OpenGNM, PSBC, SPIR-V and Vulkan headers at its recorded revisions, together with its patches and build recipes. Its full notices are copied into `licenses/ps5-opengl/`, including per-component licence texts. The actual linked `libPS5OpenGL.a` is verified against that release's manifest; sample UI integrations contained in the upstream archive are not automatically linked into Orbit.

The native build uses zlib 1.3.2 for its host-side packaging tool; its source is included. The generated `libc.prx` shim and native FSELF tool sources are in `app/tooling/`. UFS2Tool is a build-only BSD-2-Clause tool fetched at the commit pinned in `app/tools/setup-packaging-dependencies.sh`; it is not distributed inside the TV app.

The packaging command checks every pinned upstream source hash, preserves nested licence files and requires an extraction check of the built FFPKG to verify that it carries the intended payload before staging a release. Both binaries must be distributed with `orbit-store-<version>-source-bundle.zip` or the equivalent complete set of sources and notices.

## Not covered by these licences

Game names, cover art and screenshots shown by Orbit belong to their respective owners. Artwork is loaded from external catalogue URLs at runtime; it is not part of Orbit's licensed source and is not licensed under the GPL.
