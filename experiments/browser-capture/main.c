/* Isolated PS5 experiment. Not linked into Orbit, installed or deployed by make. */
#include "browser_platform.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <microhttpd.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static atomic_bool cancelled, selected, observed, file_requested;
static volatile sig_atomic_t interrupted;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char state[64] = "waiting-for-user";
static char origin[80], page_path[80], continue_path[100], status_path[100], cancel_path[100];
static char file_path[160], marker[PROBE_MARKER_MAX], page[4096];
static uint64_t attempted, readable;
static unsigned failures;

static void signal_stop(int value) { (void)value; interrupted = 1; }
static time_t seconds(void) {
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) ? -1 : now.tv_sec;
}
static void set_state(const char *value, const ProbeReader *reader) {
    pthread_mutex_lock(&lock);
    snprintf(state, sizeof state, "%s", value);
    if (reader) {
        attempted = reader->attempted;
        readable = reader->readable;
        failures = reader->failures;
    }
    pthread_mutex_unlock(&lock);
    /* Never print browser bytes, PIDs, URLs or random capabilities. */
    printf("Orbit browser probe: %s\n", value);
}
static enum MHD_Result reply(struct MHD_Connection *connection, unsigned code,
                             const char *type, const char *body, const char *location) {
    struct MHD_Response *response = MHD_create_response_from_buffer(strlen(body), (void *)body,
                                                                  MHD_RESPMEM_MUST_COPY);
    if (!response) return MHD_NO;
    MHD_add_response_header(response, "Content-Type", type);
    MHD_add_response_header(response, "Cache-Control", "no-store");
    MHD_add_response_header(response, "Referrer-Policy", "no-referrer");
    MHD_add_response_header(response, "X-Content-Type-Options", "nosniff");
    MHD_add_response_header(response, "Content-Security-Policy",
        "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
        "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
    if (location) MHD_add_response_header(response, "Location", location);
    if (!strcmp(type, "application/octet-stream"))
        MHD_add_response_header(response, "Content-Disposition", "attachment; filename=orbit-browser-test.bin");
    enum MHD_Result result = MHD_queue_response(connection, code, response);
    MHD_destroy_response(response);
    return result;
}
static enum MHD_Result handle(void *cls, struct MHD_Connection *connection,
                              const char *url, const char *method, const char *version,
                              const char *data, size_t *size, void **context) {
    (void)cls; (void)version; (void)data;
    if (!*context) { *context = (void *)1; return MHD_YES; }
    if (*size) { *size = 0; return MHD_YES; }
    const char *host = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Host");
    if (!host || strcmp(host, origin + 7))
        return reply(connection, 403, "text/plain", "Invalid host.", NULL);
    if (!strcmp(method, "POST") && !strcmp(url, cancel_path)) {
        const char *from = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Origin");
        if (!from || strcmp(from, origin))
            return reply(connection, 403, "text/plain", "Invalid origin.", NULL);
        atomic_store(&cancelled, true);
        return reply(connection, 200, "text/plain", "Cancelled. You can return to Orbit.", NULL);
    }
    if (strcmp(method, "GET")) return reply(connection, 405, "text/plain", "GET only.", NULL);
    if (!strcmp(url, page_path)) return reply(connection, 200, "text/html; charset=utf-8", page, NULL);
    if (!strcmp(url, status_path)) {
        char body[320];
        pthread_mutex_lock(&lock);
        snprintf(body, sizeof body,
            "{\"state\":\"%s\",\"markerObserved\":%s,\"fileRequested\":%s,"
            "\"attemptedBytes\":%llu,\"readableBytes\":%llu,\"readFailures\":%u}",
            state, atomic_load(&observed) ? "true" : "false",
            atomic_load(&file_requested) ? "true" : "false",
            (unsigned long long)attempted, (unsigned long long)readable, failures);
        pthread_mutex_unlock(&lock);
        return reply(connection, 200, "application/json", body, NULL);
    }
    if (!strcmp(url, continue_path)) {
        if (atomic_load(&cancelled)) return reply(connection, 410, "text/plain", "Test ended.", NULL);
        atomic_store(&selected, true);
        return reply(connection, 302, "text/plain", "One-byte test file.", marker);
    }
    if (!strcmp(url, file_path)) {
        if (!atomic_load(&selected) || atomic_load(&cancelled))
            return reply(connection, 410, "text/plain", "Test ended.", NULL);
        atomic_store(&file_requested, true);
        return reply(connection, 200, "application/octet-stream", "O", NULL);
    }
    return reply(connection, 404, "text/plain", "Not found.", NULL);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, signal_stop);
    signal(SIGTERM, signal_stop);
    if (seconds() <= 0) { set_state("monotonic-clock-unavailable", NULL); return 1; }
    unsigned char random[32];
    arc4random_buf(random, sizeof random);
    char session[33] = {0}, file[33] = {0};
    for (size_t i = 0; i < 16; i++) {
        snprintf(session + i * 2, 3, "%02x", random[i]);
        snprintf(file + i * 2, 3, "%02x", random[i + 16]);
    }
    snprintf(page_path, sizeof page_path, "/%s/", session);
    snprintf(continue_path, sizeof continue_path, "/%s/continue", session);
    snprintf(status_path, sizeof status_path, "/%s/status", session);
    snprintf(cancel_path, sizeof cancel_path, "/%s/cancel", session);
    snprintf(file_path, sizeof file_path, "/%s/file/%s/orbit-browser-test.bin", session, file);
    struct sockaddr_in address = {.sin_family = AF_INET};
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#ifdef ORBIT_PROBE_FIXTURE_BUILD
    /* Host simulator only: allow a loopback-published Docker port for UI QA. */
    const char *port_text = getenv("ORBIT_PROBE_FIXTURE_PORT");
    if (port_text) {
        char *end = NULL;
        long port = strtol(port_text, &end, 10);
        if (!end || *end || port < 1024 || port > 65535) return 1;
        address.sin_port = htons((uint16_t)port);
        address.sin_addr.s_addr = htonl(INADDR_ANY);
    }
#endif
    struct MHD_Daemon *server = MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD, 0,
        NULL, NULL, handle, NULL, MHD_OPTION_SOCK_ADDR, &address,
        MHD_OPTION_CONNECTION_LIMIT, 4U, MHD_OPTION_CONNECTION_TIMEOUT, 5U,
        MHD_OPTION_CONNECTION_MEMORY_LIMIT, (size_t)16384,
        MHD_OPTION_THREAD_STACK_SIZE, (size_t)(1024 * 1024), MHD_OPTION_END);
    if (!server) { set_state("local-server-failed", NULL); return 1; }
    const union MHD_DaemonInfo *info = MHD_get_daemon_info(server, MHD_DAEMON_INFO_BIND_PORT);
    if (!info || !info->port) { MHD_stop_daemon(server); return 1; }
    snprintf(origin, sizeof origin, "http://127.0.0.1:%u", info->port);
    snprintf(marker, sizeof marker, "%s%s", origin, file_path);
    snprintf(page, sizeof page,
        "<!doctype html><html lang=en><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Orbit browser test</title><style>"
        "body{font:24px system-ui;background:#10121b;color:#fff;max-width:850px;margin:7vh auto;padding:24px}"
        "a,button{display:inline-block;border:0;border-radius:16px;background:#adbbff;color:#121827;"
        "padding:18px 24px;margin:12px 12px 12px 0;font:inherit;text-decoration:none}"
        "a:focus,button:focus{outline:4px solid white;outline-offset:4px}pre{white-space:pre-wrap;font-size:18px}"
        "</style><h1>Orbit browser test</h1>"
        "<p>This is a local capability test. No provider or game download is involved.</p>"
        "<p>Start sends a one-byte test file. The probe checks for its unique URL in the browser network process. "
        "It never saves browser memory and ends within 90 seconds.</p>"
        "<button id=start>Start one-byte test</button><button id=cancel>Cancel test</button>"
        "<a href='http://127.0.0.1:34177/'>Return to Orbit</a><pre id=status>Waiting for you to start.</pre>"
        "<p id=notice role=status>The result will stay visible after this test ends.</p>"
        "<script>var s=document.getElementById('status'),n=document.getElementById('notice'),"
        "b=document.getElementById('start'),c=document.getElementById('cancel'),stopped=false;"
        "function poll(){if(stopped)return;fetch('%s').then(function(r){return r.json()}).then(function(x){"
        "if(stopped)return;s.textContent=JSON.stringify(x,null,2);"
        "if(x.state!=='waiting-for-user'&&x.state!=='observing-test-marker'&&"
        "(x.state!=='marker-observed'||x.fileRequested)){"
        "stopped=true;b.disabled=c.disabled=true;n.textContent='Test finished. Result kept above.'}"
        "else setTimeout(poll,500)}).catch(function(){stopped=true;b.disabled=c.disabled=true;"
        "n.textContent='Test connection ended. Last observed result kept above; completion may be unconfirmed.'})}"
        "b.onclick=function(){b.disabled=true;fetch('%s').then(function(r){return r.arrayBuffer()})"
        ".catch(function(){n.textContent='Test file request failed. Check the result above.'})};"
        "c.onclick=function(){stopped=true;b.disabled=c.disabled=true;fetch('%s',{method:'POST'})"
        ".then(function(){n.textContent='Test cancelled. Last observed result kept above.'})"
        ".catch(function(){n.textContent='Test connection ended. Last observed result kept above.'})};"
        "poll()</script></html>", status_path, continue_path, cancel_path);
    char start_url[160];
    snprintf(start_url, sizeof start_url, "%s%s", origin, page_path);
    if (probe_browser_open(start_url)) {
        set_state("browser-launch-failed", NULL);
        MHD_stop_daemon(server);
        return 1;
    }
    time_t now = seconds();
    if (now <= 0) { MHD_stop_daemon(server); return 1; }
    time_t deadline = now + 90;
    /* Leave five seconds to deliver an expired scan result before closing HTTP. */
    ProbeTarget target = {.cancel = &cancelled, .interrupted = &interrupted, .deadline = deadline - 5};
    ProbeReader reader = {.context = &target, .active = probe_target_active,
                          .read = probe_target_read, .remaining = 128U * 1024U * 1024U};
    bool finished = false;
    while (!interrupted && !atomic_load(&cancelled) &&
           (now = seconds()) > 0 && now < deadline) {
        if (!finished && now >= target.deadline) {
            set_state("expired", &reader);
            finished = true;
        }
        if (!atomic_load(&selected) || finished) {
            struct timespec delay = {.tv_nsec = 100000000};
            nanosleep(&delay, NULL);
            continue;
        }
        int found = probe_target_find(&target);
        if (found != 1) {
            set_state(found == 0 ? "network-process-not-found" : found == -2 ?
                      "ambiguous-network-process" : "unsupported-process-metadata", &reader);
            finished = true;
            continue;
        }
        set_state("observing-test-marker", &reader);
        ProbeResult result = probe_target_scan(&target, &reader, marker);
        if (result == PROBE_FOUND) {
            atomic_store(&observed, true);
            set_state("marker-observed", &reader);
        } else {
            set_state(target.identity_changed ? "process-changed" :
                atomic_load(&cancelled) ? "cancelled" :
                seconds() >= target.deadline ? "expired" : result == PROBE_BUDGET ? "scan-budget-reached" :
                result == PROBE_INVALID ? "unsupported-memory-map" :
                reader.readable ? "marker-not-observed" : "browser-memory-unreadable", &reader);
        }
        finished = true; /* One bounded pass, no automatic retries. */
    }
    atomic_store(&cancelled, true);
    if (!finished) set_state(interrupted || seconds() < deadline ? "cancelled" : "expired", &reader);
    MHD_stop_daemon(server);
    return atomic_load(&observed) ? 0 : 1;
}
