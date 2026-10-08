# Orbit Store TV app - build entry points (run inside the app build image).
# Adapted from ps5-homebrew-ui / ps5-native-app-boilerplate (GPL-3.0-or-later).
#
#   docker build -t orbit-app-build:0.1 app
#   docker run --rm -v "$PWD:/work" -w /work/app orbit-app-build:0.1 make ffpkg

SHELL := /bin/bash
.DEFAULT_GOAL := app

# The kit's build reads these from the environment.
APP_DEFINITIONS ?=
APP_INCLUDE_PATHS ?=
APP_STATIC_ARCHIVES ?=
APP_IMPORT_STUBS ?=
APP_RUNTIME_MODULES ?=
PS5_OPENGL_PREFIX ?=
OPENGL_SDK := .deps/ps5-opengl/current
override APP_DEFINITIONS := $(strip GL_GLEXT_PROTOTYPES=1 $(APP_DEFINITIONS))
override APP_INCLUDE_PATHS := $(strip src $(OPENGL_SDK)/include $(APP_INCLUDE_PATHS))
override APP_STATIC_ARCHIVES := $(strip .deps/ps5-opengl/libps5opengl-group.a $(APP_STATIC_ARCHIVES))
override APP_IMPORT_STUBS := $(strip $(OPENGL_SDK)/lib/libSceAgc.so \
	$(OPENGL_SDK)/lib/libSceAgcDriver.so $(APP_IMPORT_STUBS))
# WebP (the catalogue's artwork format) from the pinned PacBrew ports. The
# archives are named directly: the port's pkg-config output carries a
# compiler flag (-D_THREAD_SAFE) that the linker rejects.
PACBREW_PACKAGES ?=
PACBREW_INCLUDE_PATHS ?= include
PACBREW_STATIC_ARCHIVES ?= lib/libwebp.a lib/libsharpyuv.a
BUILD_JOBS ?= $(shell nproc 2>/dev/null || echo 2)
USE_CCACHE ?= 0
export BUILD_JOBS USE_CCACHE
export APP_DEFINITIONS APP_INCLUDE_PATHS APP_STATIC_ARCHIVES APP_IMPORT_STUBS APP_RUNTIME_MODULES
export PS5_OPENGL_PREFIX PACBREW_PACKAGES PACBREW_INCLUDE_PATHS PACBREW_STATIC_ARCHIVES

# Orbit itself, carried in the package so the app can start it when it isn't
# running (src/orbit/starter.hpp). Build it first with `make payload` in the
# repository root, or pass ORBIT_PAYLOAD=none for an app that only waits.
ORBIT_PAYLOAD ?= ../build/orbit_store.elf
PAYLOAD_STAGE := build/package-extra
export APP_EXTRA_DIR := $(PAYLOAD_STAGE)

RUNTIME := runtime/libc.prx
RUNTIME_INPUTS := tools/rebuild-libc.sh tools/build-host-tools.sh tools/ninja-build.sh \
	$(wildcard tooling/native/*.cpp tooling/native/*.hpp) \
	$(wildcard tooling/native/runtime/*.txt)

.PHONY: app ffpkg ffpfsc packages deps opengl libc payload-stage preview test fonts clean distclean help

app: $(RUNTIME) opengl payload-stage
	@printf '%s\n' '==> [app] Compiling, linking, signing and assembling the app folder'
	@bash tools/build.sh Folder

ffpkg: $(RUNTIME) opengl payload-stage
	@printf '%s\n' '==> [ffpkg] Building the app folder and its UFS2 image'
	@bash tools/build.sh Ffpkg

ffpfsc: $(RUNTIME) opengl payload-stage
	@printf '%s\n' '==> [ffpfsc] Building the app folder and its compressed image'
	@bash tools/build.sh Ffpfsc

packages: $(RUNTIME) opengl payload-stage
	@bash tools/build.sh All

deps:
	@bash tools/setup-test-dependencies.sh >/dev/null
	@bash tools/setup-native-dependencies.sh
	@bash tools/setup-pacbrew-dependencies.sh --environment
	@bash tools/prepare-opengl.sh

payload-stage:
	@bash tools/stage-payload.sh "$(ORBIT_PAYLOAD)" "$(PAYLOAD_STAGE)"

opengl:
	@bash tools/prepare-opengl.sh

libc:
	@bash tools/rebuild-libc.sh

$(RUNTIME): $(RUNTIME_INPUTS)
	@printf '%s\n' '==> [libc] Generating the clean-room loader shim'
	@bash tools/rebuild-libc.sh

# Pictures of every screen, rendered on this machine (build/preview/pictures).
preview:
	@bash tools/preview.sh build/preview/pictures all

test:
	@bash tools/build-tests.sh
	@ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
		build/tests/unit_tests --gtest_brief=1

fonts:
	@bash tools/bake-fonts.sh

clean:
	@rm -rf -- build dist
	@rm -f -- $(RUNTIME)

distclean: clean
	@rm -rf -- .deps

help:
	@printf '%s\n' \
	  'make            Build the PS5 app folder in dist/PPSA99177' \
	  'make ffpkg      Also build dist/PPSA99177.ffpkg (ShadowMountPlus image)' \
	  'make preview    Render every screen on this machine to PNG' \
	  'make test       Host unit tests (sanitizers on)' \
	  'make fonts      Rebake the Inter SDF fonts'
