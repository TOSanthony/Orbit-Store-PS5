.DEFAULT_GOAL := host
SOURCES := $(wildcard backend/*.c) .deps/cjson/cJSON.c
HEADERS := $(wildcard backend/*.h)
CPPFLAGS := -D_FILE_OFFSET_BITS=64 -Ibackend -Ilauncher -I.deps/cjson -Ibuild/generated
CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror -Wno-misleading-indentation
HOST_CC ?= clang-19
PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
PS5_CC := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
PS5_LIB := $(PS5_PAYLOAD_SDK)/target/user/homebrew
PS5_STRIP := $(PS5_PAYLOAD_SDK)/bin/prospero-strip
GENERATED := build/generated/assets.h build/generated/catalog.h build/generated/ca.h build/generated/network.h build/generated/launcher.h
PAYLOAD_SOURCES := $(SOURCES) launcher/install.c launcher/platform_ps5.c
PAYLOAD_CC = $(PS5_CC) $(CPPFLAGS) $(CFLAGS) -DORBIT_INSTALL_LAUNCHER -Wno-unreachable-code-generic-assoc -I$(PS5_LIB)/include
# The SDK opens DT_NEEDED libraries in order. AppInst uses Ipmi internally;
# retain and load Ipmi first even though Orbit does not call its API directly.
PS5_LAUNCHER_LIBS := -Wl,--push-state,--no-as-needed -lSceIpmi -Wl,--pop-state -lSceAppInstUtil
PAYLOAD_LIBS := -L$(PS5_LIB)/lib -lcurl -lmicrohttpd -lssl -lcrypto $(PS5_LAUNCHER_LIBS)
PS5_FS_WRAPS := -Wl,--wrap=openat -Wl,--wrap=mkdirat -Wl,--wrap=fstatat -Wl,--wrap=linkat -Wl,--wrap=renameat -Wl,--wrap=unlinkat
.PHONY: host payload payload-core test-backend test-launcher test-payload-imports test-updates
build/generated/network.h build/generated/launcher.h &: tools/embed-launcher.py config/network.json launcher/sce_sys/param.json launcher/sce_sys/icon0.png
	python3 tools/embed-launcher.py
host: build/orbit-host
build/orbit-host: $(SOURCES) $(HEADERS) $(GENERATED)
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -DORBIT_DESKTOP $(SOURCES) -o $@ -lcurl -lmicrohttpd -lssl -lcrypto -lpthread -lm
build/orbit-test: $(SOURCES) $(HEADERS) $(GENERATED)
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -DORBIT_DESKTOP -DORBIT_TEST $(SOURCES) -o $@ -lcurl -lmicrohttpd -lssl -lcrypto -lpthread -lm
payload: build/orbit_store.elf
# The runtime is the copy saved on the console for payload managers to start.
build/orbit_runtime.elf: $(PAYLOAD_SOURCES) $(HEADERS) launcher/install.h $(GENERATED) Makefile
	$(PAYLOAD_CC) $(PAYLOAD_SOURCES) -o $@ $(PAYLOAD_LIBS) $(PS5_FS_WRAPS)
	$(PS5_STRIP) --strip-debug $@
# The public payload: the same server and icon setup, carrying the runtime it saves.
build/orbit_store.elf: $(PAYLOAD_SOURCES) $(HEADERS) launcher/install.h launcher/runtime_image.c $(GENERATED) build/orbit_runtime.elf Makefile
	$(PAYLOAD_CC) -DORBIT_EMBED_RUNTIME $(PAYLOAD_SOURCES) launcher/runtime_image.c -o $@ $(PAYLOAD_LIBS) $(PS5_FS_WRAPS)
payload-core: build/orbit_core.elf
build/orbit_core.elf: $(SOURCES) $(HEADERS) $(GENERATED) Makefile
	$(PS5_CC) $(CPPFLAGS) $(CFLAGS) -Wno-unreachable-code-generic-assoc -I$(PS5_LIB)/include $(SOURCES) -o $@ -L$(PS5_LIB)/lib -lcurl -lmicrohttpd -lssl -lcrypto $(PS5_FS_WRAPS)
test-updates: build/orbit-host build/orbit-test
	python3 tests/update_integration.py

test-backend: build/orbit-host build/orbit-test
	python3 tests/backend_integration.py

.PHONY: test-storage
build/orbit-storage-test: backend/storage.c backend/orbit.h tests/storage_driver.c .deps/cjson/cJSON.c Makefile
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -DORBIT_DESKTOP -fsanitize=undefined -fsanitize-undefined-trap-on-error backend/storage.c tests/storage_driver.c .deps/cjson/cJSON.c -Wl,--wrap=lstat64 -Wl,--wrap=stat64 -Wl,--wrap=statvfs64 -Wl,--wrap=access -Wl,--wrap=open64 -Wl,--wrap=openat64 -Wl,--wrap=mkdirat -Wl,--wrap=fstat64 -Wl,--wrap=fstatat64 -Wl,--wrap=unlinkat -Wl,--wrap=write -Wl,--wrap=close -o $@ -lpthread -lm
test-storage: build/orbit-storage-test
	./build/orbit-storage-test

.PHONY: test-art
test-art: build/orbit-test
	python3 tests/art_integration.py

.PHONY: test-tv-app
test-tv-app: build/orbit-test
	python3 tests/tvapp_integration.py

.PHONY: test-diagnostics
test-diagnostics: build/orbit-test
	python3 tests/diagnostics_integration.py

.PHONY: test-api-recovery
build/orbit-api-recovery-test: $(SOURCES) $(HEADERS) $(GENERATED) tests/api_recovery_faults.c
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -DORBIT_DESKTOP -DORBIT_TEST $(SOURCES) tests/api_recovery_faults.c -o $@ -Wl,--wrap=MHD_run_wait -Wl,--wrap=MHD_start_daemon -Wl,--wrap=connect -lcurl -lmicrohttpd -lssl -lcrypto -lpthread -lm
test-api-recovery: build/orbit-api-recovery-test
	python3 tests/api_recovery_integration.py

.PHONY: test-process-identity
build/orbit-process-identity-test: backend/process_identity.c backend/process_identity.h tests/process_identity_driver.c tests/stubs/ps5/kernel.h Makefile
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -DORBIT_PROCESS_IDENTITY_EXPERIMENT -Itests/stubs backend/process_identity.c tests/process_identity_driver.c -o $@
build/orbit-process-identity-default-test: backend/process_identity.c backend/process_identity.h tests/process_identity_default.c
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) backend/process_identity.c tests/process_identity_default.c -o $@
test-process-identity: build/orbit-process-identity-test build/orbit-process-identity-default-test
	./build/orbit-process-identity-test
	./build/orbit-process-identity-default-test

.PHONY: test-ranges
.PHONY: test-debrid
build/orbit-debrid-network-test: backend/debrid_network.c backend/debrid.h backend/provider_network.h tests/debrid_network_driver.c Makefile
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -DORBIT_DESKTOP backend/debrid_network.c tests/debrid_network_driver.c -o $@ -lcurl
test-debrid: build/orbit-test build/orbit-debrid-network-test
	./build/orbit-debrid-network-test
	python3 tests/debrid_integration.py

test-ranges: build/orbit-test
	python3 tests/range_integration.py

.PHONY: test-transfer-writer
build/orbit-transfer-writer-test: backend/transfer_writer.c backend/transfer_writer.h backend/orbit.h tests/transfer_writer_driver.c
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -DORBIT_DESKTOP -fsanitize=undefined -fsanitize-trap=all backend/transfer_writer.c tests/transfer_writer_driver.c -Wl,--wrap=fsync -Wl,--wrap=pwrite64 -o $@ -lpthread
build/orbit-transfer-writer-schedule-test: backend/transfer_writer.c backend/transfer_writer.h backend/orbit.h tests/transfer_writer_schedule_driver.c
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -DORBIT_DESKTOP -DORBIT_BENCHMARK -fsanitize=undefined -fsanitize-trap=all backend/transfer_writer.c tests/transfer_writer_schedule_driver.c -Wl,--wrap=pwrite64 -Wl,--wrap=pthread_cond_timedwait -o $@ -lpthread
test-transfer-writer: build/orbit-transfer-writer-test build/orbit-transfer-writer-schedule-test
	./build/orbit-transfer-writer-test
	./build/orbit-transfer-writer-schedule-test

.PHONY: benchmark-transfer
benchmark-transfer: build/orbit-test
	python3 tests/transfer_benchmark.py

# Explicit developer diagnostic; never a release dependency. The random secret
# header is generated locally under ignored build/generated before compilation.
BENCH_SOURCES := $(filter-out backend/main.c backend/api.c,$(SOURCES)) experiments/transfer-benchmark/main.c
.PHONY: transfer-benchmark-ps5 test-transfer-benchmark
transfer-benchmark-ps5: build/orbit_transfer_benchmark.elf
build/orbit_transfer_benchmark.elf: $(BENCH_SOURCES) $(HEADERS) $(GENERATED) build/generated/benchmark-secret.h Makefile
	$(PS5_CC) $(CPPFLAGS) $(CFLAGS) -DORBIT_BENCHMARK -Wno-unreachable-code-generic-assoc -I$(PS5_LIB)/include $(BENCH_SOURCES) -o $@ -L$(PS5_LIB)/lib -lcurl -lmicrohttpd -lssl -lcrypto $(PS5_FS_WRAPS)
build/orbit-transfer-benchmark-test: $(BENCH_SOURCES) $(HEADERS) $(GENERATED) build/generated/benchmark-secret.h Makefile
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -D_DEFAULT_SOURCE -DORBIT_DESKTOP -DORBIT_TEST -DORBIT_BENCHMARK $(BENCH_SOURCES) -o $@ -lcurl -lmicrohttpd -lssl -lcrypto -lpthread -lm
test-transfer-benchmark: build/orbit-transfer-benchmark-test
	python3 tests/console_benchmark_integration.py

.PHONY: test-viking
build/orbit-providers-test: backend/providers.c backend/orbit.h tests/providers_driver.c
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -DORBIT_DESKTOP backend/providers.c tests/providers_driver.c -o $@
test-viking: build/orbit-test build/orbit-providers-test
	./build/orbit-providers-test
	python3 tests/viking_integration.py
build/orbit-launcher-test: launcher/install.c launcher/install.h tests/launcher_driver.c build/generated/launcher.h
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) launcher/install.c tests/launcher_driver.c -o $@
build/orbit-launcher-platform-test: launcher/platform_ps5.c launcher/install.h tests/launcher_platform_driver.c tests/stubs/ps5/kernel.h
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -Itests/stubs launcher/platform_ps5.c tests/launcher_platform_driver.c -Wl,--wrap=dlsym -o $@
test-launcher: build/orbit-launcher-test build/orbit-launcher-platform-test
	python3 tests/launcher_integration.py
	python3 tests/launcher_platform.py
test-payload-imports: payload payload-core
	python3 tests/payload_imports.py

.PHONY: test-catalog-refresh
test-catalog-refresh: build/orbit-test
	python3 tests/catalog_refresh.py

.PHONY: test-library
test-library: build/orbit-host build/orbit-test
	python3 tests/library_integration.py
	python3 tests/library_actions.py

# Isolated opt-in experiment. Never part of host, payload or release packaging.
.PHONY: browser-probe test-browser-probe
BROWSER_PROBE := experiments/browser-capture
build/orbit-browser-probe-test: backend/browser_scan.c backend/browser_scan.h tests/browser_probe_driver.c
	$(HOST_CC) $(CFLAGS) -Ibackend -fsanitize=undefined -fsanitize-trap=all backend/browser_scan.c tests/browser_probe_driver.c -o $@
build/orbit-browser-probe-fixture: $(BROWSER_PROBE)/main.c backend/browser_scan.c backend/browser_scan.h backend/browser_platform.h tests/browser_probe_fixture.c
	$(HOST_CC) $(CFLAGS) -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -DORBIT_PROBE_FIXTURE_BUILD -Ibackend $(BROWSER_PROBE)/main.c backend/browser_scan.c tests/browser_probe_fixture.c -o $@ -lmicrohttpd -lpthread
test-browser-probe: build/orbit-browser-probe-test build/orbit-browser-probe-fixture
	./build/orbit-browser-probe-test
	python3 tests/browser_probe_integration.py
browser-probe: build/orbit_browser_probe.elf
build/orbit_browser_probe.elf: $(BROWSER_PROBE)/main.c backend/browser_platform.c backend/browser_platform.h backend/browser_services.c backend/browser_scan.c backend/browser_scan.h Makefile
	$(PS5_CC) $(CFLAGS) -DORBIT_BROWSER_PROBE -Ibackend -I$(PS5_LIB)/include $(BROWSER_PROBE)/main.c backend/browser_platform.c backend/browser_services.c backend/browser_scan.c -o $@ -L$(PS5_LIB)/lib -lmicrohttpd

.PHONY: test-browser-capture
build/orbit-browser-services-test: backend/browser_services.c backend/browser_platform.h tests/browser_services_driver.c
	$(HOST_CC) $(CFLAGS) -DORBIT_BROWSER_PROBE -Ibackend backend/browser_services.c tests/browser_services_driver.c -Wl,--wrap=dlopen -Wl,--wrap=dlsym -o $@
.PHONY: test-browser-services
build/orbit-browser-worker-test: backend/browser.c $(HEADERS) tests/browser_worker_driver.c
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -DORBIT_DESKTOP -DORBIT_TEST backend/browser.c .deps/cjson/cJSON.c tests/browser_worker_driver.c -Wl,--wrap=pthread_attr_setstacksize -Wl,--wrap=pthread_create -Wl,--wrap=pthread_join -o $@ -lpthread -lm
test-browser-services: build/orbit-browser-services-test build/orbit-browser-worker-test
	python3 -c 'import subprocess; [subprocess.run(["./build/orbit-browser-services-test", str(i)], check=True) for i in range(9)]'
	./build/orbit-browser-worker-test idle
	./build/orbit-browser-worker-test request
build/orbit-browser-capture-test: backend/browser_match.c backend/browser_match.h backend/browser_scan.c backend/providers.c backend/provider_network.c tests/browser_capture_driver.c
	$(HOST_CC) $(CPPFLAGS) $(CFLAGS) -DORBIT_DESKTOP -fsanitize=undefined -fsanitize-trap=all backend/browser_match.c backend/browser_scan.c backend/providers.c backend/provider_network.c tests/browser_capture_driver.c -o $@ -lcurl
test-browser-capture: build/orbit-test build/orbit-browser-capture-test
	./build/orbit-browser-capture-test
	python3 tests/browser_capture_integration.py
