#include "orbit.h"
#include "debrid.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef ORBIT_DESKTOP
#include <ps5/kernel.h>
extern int sceKernelSendNotificationRequest(int, const void *, size_t, int);
#endif
static volatile sig_atomic_t stopping;
static atomic_bool stop_requested;
#ifdef ORBIT_INSTALL_LAUNCHER
#include "install.h"
#endif
#ifdef ORBIT_EMBED_RUNTIME
/* The runtime this payload saves on the console; see launcher/runtime_image.c. */
extern const unsigned char orbit_runtime_image[], orbit_runtime_image_end[];
#endif
static int notify_console(const char *text) {
#ifndef ORBIT_DESKTOP
    unsigned char notification[3120] = {0};
    snprintf((char *)notification + 45, sizeof notification - 45, "%s", text);
    return sceKernelSendNotificationRequest(0, notification, sizeof notification, 0);
#else
    (void)text;
    return 0;
#endif
}
int pairing_notify(void) {
#if defined(ORBIT_DESKTOP) && !defined(ORBIT_TEST)
    return -1;
#else
    char message[160];
    snprintf(message, sizeof message,
             "Orbit Store pairing code: %s\nOpen Pair devices in Orbit to keep the code on screen.",
             orbit.pair_code);
    return notify_console(message);
#endif
}
static void stop_signal(int n) {
    (void)n;
    stopping = 1;
}
void orbit_request_stop(void) {
    atomic_store(&stop_requested, true);
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    copy_text(orbit.state_dir, sizeof orbit.state_dir, "/data/orbit-store");
    const unsigned char *runtime = NULL;
    size_t runtime_size = 0;
#ifdef ORBIT_EMBED_RUNTIME
    runtime = orbit_runtime_image;
    runtime_size = (size_t)(orbit_runtime_image_end - orbit_runtime_image);
#endif
#ifdef ORBIT_DESKTOP
    orbit.desktop = true;
    copy_text(orbit.state_dir, sizeof orbit.state_dir, ".state");
    for (int i = 1; i < argc; i++) {
        if (i + 1 < argc && !strcmp(argv[i], "--state"))
            copy_text(orbit.state_dir, sizeof orbit.state_dir, argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "--storage"))
            copy_text(orbit.desktop_storage, sizeof orbit.desktop_storage, argv[++i]);
        else if (i + 1 < argc && !strcmp(argv[i], "--port"))
            orbit.port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--listen-all"))
            orbit.listen_all = true;
        else if (i + 1 < argc && !strcmp(argv[i], "--system-root")) {
            copy_text(orbit.system_root, sizeof orbit.system_root, argv[++i]);
            orbit.autoboot = true;
        } else if (i + 1 < argc && !strcmp(argv[i], "--runtime-image")) {
            unsigned char *image;
            if (read_regular_file(argv[++i], 64 * 1024 * 1024, &image, &runtime_size)) {
                perror("runtime image");
                return 1;
            }
            runtime = image;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 1;
        }
    }
#else
    (void)argc;
    (void)argv;
    orbit.autoboot = true;
#endif
    if (orbit.port < 1024 || orbit.port > 65535 || catalog_init()) {
        fputs("Invalid configuration/catalogue.\n", stderr);
        return 1;
    }
#ifdef ORBIT_TEST
    const char *url = getenv("ORBIT_TEST_URL"), *size = getenv("ORBIT_TEST_SIZE"),
               *sha = getenv("ORBIT_TEST_SHA256");
    if (!url || strncmp(url, "http://127.0.0.1:", 17) || !size) {
        fputs("Tests require a loopback fixture URL and size.\n", stderr);
        return 1;
    }
    copy_text(orbit.releases[0].url, sizeof orbit.releases[0].url, url);
    orbit.releases[0].size = strtoll(size, NULL, 10);
    copy_text(orbit.releases[0].sha256, sizeof orbit.releases[0].sha256, sha ? sha : "");
    const char *test_source = getenv("ORBIT_TEST_SOURCE");
    if (test_source && !source_flag(test_source)) return 1;
    if (test_source)
        copy_text(orbit.releases[0].source, sizeof orbit.releases[0].source, test_source);
    const char *browser_url = getenv("ORBIT_TEST_BROWSER_URL");
    if (browser_url) copy_text(orbit.releases[0].browser_url, sizeof orbit.releases[0].browser_url, browser_url);
    cJSON *fixture = cJSON_GetArrayItem(orbit.catalog, 0);
    if (test_source)
        cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(fixture, "sourceId"), test_source);
    copy_text(orbit.releases[0].title, sizeof orbit.releases[0].title, "Orbit local test file");
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(fixture, "title"),
                         "Orbit local test file");
    cJSON_SetValuestring(
        cJSON_GetObjectItemCaseSensitive(fixture, "description"),
        "A disposable local HTTP fixture for testing the download controls. No PS5 is connected.");
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(fixture, "sizeBytes"),
                         orbit.releases[0].size);
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(fixture, "provider"),
                         "Local test server");
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(fixture, "cover"), "/orbit.svg");
#endif
    if (mkdir(orbit.state_dir, 0700) != 0 && access(orbit.state_dir, F_OK) != 0) {
        perror("state directory");
        return 1;
    }
    orbit.state_fd = open(orbit.state_dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (orbit.state_fd < 0) {
        perror("state directory");
        return 1;
    }
    int lock = openat(orbit.state_fd, "instance.lock", O_WRONLY | O_CREAT | O_NOFOLLOW, 0600);
    if (lock < 0) {
        perror("Orbit state lock");
        return 1;
    }
    if (flock(lock, LOCK_EX | LOCK_NB)) {
        int error = errno;
        close(lock);
        close(orbit.state_fd);
        if (error == EWOULDBLOCK || error == EAGAIN) {
            /* A newer payload still replaces the saved copy for the next start. */
            int synced = autoboot_sync(runtime, runtime_size);
            if (runtime && synced >= 0) {
                char message[256];
                snprintf(message, sizeof message,
                         "Orbit %s saved. The previous session is still running. Stop Orbit, then "
                         "run the saved payload to apply it.",
                         ORBIT_VERSION);
                puts(message);
                notify_console(message);
            } else if (runtime && synced < 0) {
                notify_console("Orbit is running, but the replacement could not be fully saved. "
                               "Retry the update; the current session is unchanged.");
            } else {
                puts("Orbit is already running. Open the Orbit Store icon; the current queue is "
                     "unchanged.");
                notify_console("Orbit Store is already running. Open its home-screen icon.");
            }
            return 0;
        }
        fputs("Orbit state cannot be locked.\n", stderr);
        return 1;
    }
    diagnostics_begin();
    diagnostics_stage("startup", "started", 0);
    diagnostics_stage("catalogue-cache", "started", 0);
    catalog_load_cache();
    diagnostics_stage("catalogue-cache", "ok", 0);
    diagnostics_stage("https", "started", 0);
    int startup_result = curl_global_init(CURL_GLOBAL_DEFAULT);
    diagnostics_stage("https", startup_result ? "error" : "ok", startup_result);
    if (!startup_result) {
        diagnostics_stage("saved-state", "started", 0);
        startup_result = state_load();
        diagnostics_stage("saved-state", startup_result ? "error" : "ok", startup_result);
    }
    if (startup_result) {
        fputs("Failed to initialize HTTPS or read saved state. Preserving state for recovery.\n",
              stderr);
        return 1;
    }
    diagnostics_stage("process-identity", "started", 0);
    orbit.process_identity = orbit_process_identity();
    diagnostics_stage("process-identity",
                      orbit.process_identity.state == ORBIT_PROCESS_UNCHECKED ? "skipped" : "ok", 0);
    diagnostics_stage("pairing", "started", 0);
    char random[9];
    random_hex(random, 4);
    unsigned long code = strtoul(random, NULL, 16) % 1000000;
    snprintf(orbit.pair_code, sizeof orbit.pair_code, "%06lu", code);
    printf("Orbit Store %s | %s | port %d\n", ORBIT_VERSION,
           orbit.desktop ? "DESKTOP PREVIEW (game transfers disabled)" : "PS5", orbit.port);
    printf("Pairing code: %s\n", orbit.pair_code);
    diagnostics_stage("pairing", "ok", 0);
#ifndef ORBIT_DESKTOP
    diagnostics_stage("firmware", "started", 0);
    orbit.firmware = kernel_get_fw_version();
    printf("Firmware raw: 0x%08x\n", orbit.firmware);
    diagnostics_stage("firmware", "ok", 0);
#endif
#ifdef ORBIT_INSTALL_LAUNCHER
    copy_text(orbit.launcher_status, sizeof orbit.launcher_status, "checking");
#else
    copy_text(orbit.launcher_status, sizeof orbit.launcher_status, "not-included");
#endif
    signal(SIGTERM, stop_signal);
    signal(SIGINT, stop_signal);
    signal(SIGPIPE, SIG_IGN);
    diagnostics_stage("listener", "started", 0);
    debrid_start();
    startup_result = api_start();
    diagnostics_stage("listener", startup_result ? "error" : "ok", startup_result);
    if (startup_result) {
        char message[200];
        snprintf(message, sizeof message,
                 "Orbit Store cannot listen on TCP port %d. Check for another service using this "
                 "port. No launcher changes were made.",
                 orbit.port);
        fprintf(stderr, "%s\n", message);
        notify_console(message);
        return 1;
    }
    /* Console threads may default to small stacks; curl and OpenSSL need headroom. */
    diagnostics_stage("download-worker", "started", 0);
    pthread_attr_t worker_attr;
    bool failed = pthread_attr_init(&worker_attr) != 0;
    if (!failed) {
        failed = pthread_attr_setstacksize(&worker_attr, ORBIT_THREAD_STACK) ||
                 pthread_create(&orbit.worker, &worker_attr, download_worker, NULL);
        pthread_attr_destroy(&worker_attr);
    }
    diagnostics_stage("download-worker", failed ? "error" : "ok", failed ? -1 : 0);
    if (failed) {
        api_stop();
        return 1;
    }
#ifdef ORBIT_INSTALL_LAUNCHER
    diagnostics_stage("launcher", "started", 0);
    int launcher = orbit_launcher_ensure(orbit.state_fd);
    pthread_mutex_lock(&orbit.mutex);
    copy_text(orbit.launcher_status, sizeof orbit.launcher_status,
              launcher >= 0 ? "ready" : "error");
    copy_text(orbit.launcher_error, sizeof orbit.launcher_error, orbit_launcher_last_error());
    copy_text(orbit.launcher_registration_method, sizeof orbit.launcher_registration_method,
              orbit_launcher_registration_method());
    pthread_mutex_unlock(&orbit.mutex);
    diagnostics_stage("launcher", launcher < 0 ? "error" : "ok", launcher);
#endif
#if defined(ORBIT_INSTALL_LAUNCHER) || defined(ORBIT_DESKTOP)
    diagnostics_stage("integrations", "started", 0);
    startup_result = autoboot_sync(runtime, runtime_size);
    diagnostics_stage("integrations", startup_result < 0 ? "error" : "ok", startup_result);
#endif
    diagnostics_stage("catalogue", "started", 0);
    startup_result = catalog_start();
    diagnostics_stage("catalogue", startup_result ? "error" : "ok", startup_result);
    if (startup_result)
        puts("Catalogue refresh could not start; the saved catalogue remains available.");
    diagnostics_stage("updates", "started", 0);
    startup_result = updates_start();
    diagnostics_stage("updates", startup_result ? "error" : "ok", startup_result);
    if (startup_result)
        puts("Orbit update worker could not start; the server remains available.");
    diagnostics_stage("browser", "started", 0);
    startup_result = browser_start();
    diagnostics_stage("browser", startup_result ? "error" : "ok", startup_result);
    diagnostics_stage("library", "started", 0);
    startup_result = library_start();
    diagnostics_stage("library", startup_result ? "error" : "ok", startup_result);
    if (startup_result)
        puts("Library worker could not start; the server remains available.");
    diagnostics_stage("tv-app", "started", 0);
    startup_result = tvapp_start();
    diagnostics_stage("tv-app", startup_result ? "error" : "ok", startup_result);
    if (startup_result)
        puts("TV app installer could not start; the server remains available.");
    diagnostics_stage("artwork", "started", 0);
    startup_result = art_start();
    diagnostics_stage("artwork", startup_result ? "error" : "ok", startup_result);
    if (startup_result)
        puts("Artwork workers could not start; the TV app shows covers as placeholders.");
    char ready_message[200];
    snprintf(ready_message, sizeof ready_message, "Orbit Store :%d | Pair: %s | %s", orbit.port,
             orbit.pair_code,
             !strcmp(orbit.launcher_status, "ready")   ? "Open the Orbit Store icon"
             : !strcmp(orbit.launcher_status, "error") ? "Icon setup failed; server is running"
                                                       : "Server ready");
    diagnostics_stage("notification", "started", 0);
    startup_result = notify_console(ready_message);
    diagnostics_stage("notification", startup_result ? "error" : "ok", startup_result);
    diagnostics_stage("ready", "ok", 0);
    while (!stopping) {
        sleep(1);
        if (atomic_load(&stop_requested)) {
            sleep(1); /* Let the stop acknowledgement reach the browser. */
            stopping = 1;
        }
    }
    diagnostics_stage("shutdown", "started", 0);
    api_stop();
    pthread_mutex_lock(&orbit.mutex);
    orbit.stop = true;
    pthread_cond_broadcast(&orbit.changed);
    pthread_mutex_unlock(&orbit.mutex);
    art_stop();
    debrid_stop();
    tvapp_stop();
    browser_stop();
    library_stop();
    updates_stop();
    catalog_stop();
    pthread_join(orbit.worker, NULL);
    curl_global_cleanup();
    cJSON_Delete(orbit.catalog);
    diagnostics_stage("shutdown", "ok", 0);
    close(lock);
    close(orbit.state_fd);
    return 0;
}
