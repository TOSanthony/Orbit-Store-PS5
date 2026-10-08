#include "orbit.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define EVENT_LIMIT 64
typedef struct {
    time_t at;
    const char *phase, *provider, *status, *reason;
    unsigned download, attempts;
    int connections, curl_code, result;
    long http_status;
    int64_t received, total;
} Event;
/* All strings below are constants. Never export logs, URLs, paths, titles or raw errors. */
static Event events[EVENT_LIMIT];
static size_t event_count, event_next;
static time_t started;
/* Two tiny last-stage records survive a process/console failure. No queue data,
 * tokens, URLs, arbitrary messages or paths are written to these records. */
static int trace_fd = -1;
static bool trace_path_safe(const char *name) {
    struct stat st;
    if (fstatat(trace_fd, name, &st, AT_SYMLINK_NOFOLLOW)) return errno == ENOENT;
    return S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_size <= 1024;
}
void diagnostics_begin(void) {
    /* Called once, after the instance lock and before workers/API start. */
    trace_fd = orbit.state_fd;
    if (trace_fd < 0) return;
    if (!trace_path_safe("startup-status.json") || !trace_path_safe("startup-previous.json")) {
        trace_fd = -1;
        return;
    }
    if (renameat(trace_fd, "startup-status.json", trace_fd, "startup-previous.json") &&
        errno != ENOENT) {
        trace_fd = -1;
        return;
    }
    (void)fsync(trace_fd);
}
static void trace_write(const Event *e) {
    if (trace_fd < 0 || !trace_path_safe("startup-status.json")) return;
    const char *temp = ".startup-status.tmp";
    /* Recover only our bounded regular temporary file, never follow a link. */
    if (!trace_path_safe(temp)) return;
    if (unlinkat(trace_fd, temp, 0) && errno != ENOENT) return;
    int fd = openat(trace_fd, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    char text[512];
    int length = snprintf(text, sizeof text,
        "{\"schema\":1,\"version\":\"%s\",\"at\":%lld,\"phase\":\"%s\",\"status\":\"%s\",\"result\":%d}\n",
        ORBIT_VERSION, (long long)e->at, e->phase, e->status, e->result);
    bool ok = length > 0 && (size_t)length < sizeof text;
    size_t sent = 0;
    while (ok && sent < (size_t)length) {
        ssize_t count = write(fd, text + sent, (size_t)length - sent);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) ok = false;
        else sent += (size_t)count;
    }
    if (ok && fsync(fd)) ok = false;
    if (close(fd)) ok = false;
    if (ok && !renameat(trace_fd, temp, trace_fd, "startup-status.json"))
        (void)fsync(trace_fd);
    else (void)unlinkat(trace_fd, temp, 0);
    /* Logging is best effort: never make the download state unhealthy. */
}
static const char *allowed(const char *value, const char *const *options, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (!strcmp(value, options[i])) return options[i];
    return "unknown";
}
#define ALLOWED(value, ...) allowed(value, (const char *const[]){__VA_ARGS__}, sizeof((const char *const[]){__VA_ARGS__}) / sizeof(char *))
static const char *reason(const char *error) {
    if (!*error) return "none";
    if (strstr(error, "page instead of")) return "provider-returned-page";
    if (strstr(error, "Source check failed")) return "source-check-failed";
    if (strstr(error, "Source size changed")) return "source-size-changed";
    if (strstr(error, "Source identity") || strstr(error, "Source changed")) return "source-identity-changed";
    if (strstr(error, "Source disabled")) return "source-disabled";
    if (strstr(error, "space")) return "insufficient-space";
    if (strstr(error, "missing") || strstr(error, "Reconnect")) return "destination-unavailable";
    if (strstr(error, "already exists")) return "destination-exists";
    if (strstr(error, "SHA") || strstr(error, "checksum")) return "checksum-mismatch";
    if (strstr(error, "partial")) return "partial-file-error";
    if (strstr(error, "write") || strstr(error, "persist") || strstr(error, "save")) return "storage-write-failed";
    if (strstr(error, "range") || strstr(error, "Range")) return "range-response-invalid";
    if (strstr(error, "Transfer failed")) return "transfer-failed";
    return "other-error";
}
static Event job_event(const Job *j) {
    Event e = {.at = time(NULL), .provider = "none", .status = "none", .reason = "none"};
    if (!j) return e;
    for (size_t i = 0; i < orbit.job_count; i++)
        if (j == &orbit.jobs[i]) e.download = (unsigned)i + 1;
    e.provider = ALLOWED(j->release.source, "archive", "vikingfile");
    e.status = ALLOWED(j->status, "queued", "downloading", "verifying", "retrying", "paused", "cancelled", "complete", "error");
    e.reason = reason(j->error);
    e.attempts = j->attempts;
    e.connections = j->segmented ? (int)j->range_count : 1;
    e.received = j->received;
    e.total = j->total;
    return e;
}
void diagnostics_event_locked(const char *phase, const Job *j, long http_status, int curl_code) {
    Event e = job_event(j);
    if (!started) started = e.at;
    e.phase = ALLOWED(phase, "startup", "catalogue-cache", "https", "saved-state", "process-identity", "pairing", "firmware", "listener", "listener-recovery", "download-worker", "launcher", "integrations", "catalogue", "updates", "library", "browser", "tv-app", "artwork", "notification", "ready", "shutdown", "browser-modules", "browser-user-service", "browser-launch", "browser-user-cleanup", "queued", "pause", "resume", "retry", "cancel", "transfer-start", "transfer-end");
    e.http_status = http_status >= 100 && http_status <= 599 ? http_status : 0;
    e.curl_code = curl_code >= 0 && curl_code <= 999 ? curl_code : 0;
    events[event_next] = e;
    event_next = (event_next + 1) % EVENT_LIMIT;
    if (event_count < EVENT_LIMIT) event_count++;
}
void diagnostics_stage(const char *phase, const char *status, int result) {
    pthread_mutex_lock(&orbit.mutex);
    diagnostics_event_locked(phase, NULL, 0, 0);
    Event *e = &events[(event_next + EVENT_LIMIT - 1) % EVENT_LIMIT];
    e->status = ALLOWED(status, "started", "ok", "error", "skipped");
    e->result = result;
    trace_write(e);
    pthread_mutex_unlock(&orbit.mutex);
}
static cJSON *event_json(const Event *e) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "at", (double)e->at);
    if (e->phase) cJSON_AddStringToObject(o, "phase", e->phase);
    cJSON_AddStringToObject(o, "provider", e->provider);
    cJSON_AddStringToObject(o, "status", e->status);
    cJSON_AddStringToObject(o, "reason", e->reason);
    cJSON_AddNumberToObject(o, "download", e->download);
    cJSON_AddNumberToObject(o, "httpStatus", (double)e->http_status);
    cJSON_AddNumberToObject(o, "curlCode", e->curl_code);
    if (!e->download) cJSON_AddNumberToObject(o, "result", e->result);
    cJSON_AddNumberToObject(o, "attempts", e->attempts);
    cJSON_AddNumberToObject(o, "connections", e->connections);
    cJSON_AddNumberToObject(o, "received", (double)e->received);
    cJSON_AddNumberToObject(o, "total", (double)e->total);
    return o;
}
cJSON *diagnostics_snapshot(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "schema", 1);
    cJSON_AddStringToObject(o, "version", ORBIT_VERSION);
    cJSON_AddNumberToObject(o, "generatedAt", (double)time(NULL));
    pthread_mutex_lock(&orbit.mutex);
    cJSON_AddNumberToObject(o, "startedAt", (double)started);
    cJSON_AddStringToObject(o, "platform", orbit.desktop ? "desktop" : "ps5");
    char firmware[16] = "unknown";
    if (orbit.firmware) snprintf(firmware, sizeof firmware, "0x%08x", orbit.firmware);
    cJSON_AddStringToObject(o, "firmware", firmware);
    cJSON_AddStringToObject(o, "loaderVersion", "unknown");
    cJSON *process = cJSON_AddObjectToObject(o, "processIdentity");
    cJSON_AddStringToObject(process, "status", orbit_process_state(orbit.process_identity.state));
    cJSON_AddNumberToObject(process, "originalNameLength", orbit.process_identity.original_length);
    cJSON_AddNumberToObject(process, "currentNameLength", orbit.process_identity.current_length);
    cJSON_AddNumberToObject(process, "nativeResult", orbit.process_identity.native_result);
    cJSON_AddBoolToObject(o, "stateHealthy", !orbit.state_failed);
    cJSON_AddBoolToObject(o, "queueBackupCreated", orbit.queue_backup_created);
    cJSON_AddNumberToObject(o, "catalogueRevision", orbit.catalog_revision);
    cJSON *launcher = cJSON_AddObjectToObject(o, "launcher");
    cJSON_AddStringToObject(launcher, "status", ALLOWED(orbit.launcher_status, "ready", "error", "checking", "not-included"));
    cJSON_AddStringToObject(launcher, "method", ALLOWED(orbit.launcher_registration_method, "title-directory", "app-scan", "not-needed"));
    cJSON_AddBoolToObject(launcher, "hasError", !!*orbit.launcher_error);
    cJSON *downloads = cJSON_AddArrayToObject(o, "downloads");
    for (size_t i = 0; i < orbit.job_count; i++) {
        if (!*orbit.jobs[i].id) continue;
        Event e = job_event(&orbit.jobs[i]);
        cJSON *entry = event_json(&e);
        const Job *j = &orbit.jobs[i];
        cJSON *writer = cJSON_AddObjectToObject(entry, "writer");
        cJSON_AddNumberToObject(writer, "peakBufferedBytes", (double)j->transfer_stats.peak_buffered);
        cJSON_AddNumberToObject(writer, "writeCalls", (double)j->transfer_stats.write_calls);
        cJSON_AddNumberToObject(writer, "syncCalls", (double)j->transfer_stats.sync_calls);
        cJSON_AddNumberToObject(writer, "bufferWaitSeconds", j->transfer_stats.buffer_wait_seconds);
        cJSON_AddNumberToObject(writer, "writeSeconds", j->transfer_stats.write_seconds);
        cJSON_AddNumberToObject(writer, "syncSeconds", j->transfer_stats.sync_seconds);
        cJSON_AddItemToArray(downloads, entry);
    }
    cJSON *recent = cJSON_AddArrayToObject(o, "events");
    for (size_t i = 0; i < event_count; i++)
        cJSON_AddItemToArray(recent, event_json(&events[(event_next + EVENT_LIMIT - event_count + i) % EVENT_LIMIT]));
    pthread_mutex_unlock(&orbit.mutex);
    cJSON_AddNumberToObject(o, "eventLimit", EVENT_LIMIT);
    cJSON_AddItemToObject(o, "storage", storage_diagnostics());
    return o;
}
