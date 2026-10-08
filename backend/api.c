#include "assets.h"
#include "orbit.h"
#include "debrid.h"
#include <openssl/crypto.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <microhttpd.h>
#include <netinet/in.h>
#include <poll.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#ifdef ORBIT_TEST
#define ORBIT_PLATFORM "fixture"
#else
#define ORBIT_PLATFORM (orbit.desktop ? "desktop" : "ps5")
#endif
static struct MHD_Daemon *daemon_handle;
static pthread_t api_thread;
static atomic_bool api_stopping;
static bool api_thread_started;
typedef struct {
    char body[4097];
    size_t length;
    bool too_large;
} Request;
static enum MHD_Result respond(struct MHD_Connection *c, unsigned code, const char *body, size_t n,
                               const char *mime) {
    struct MHD_Response *r =
        MHD_create_response_from_buffer(n, (void *)body, MHD_RESPMEM_MUST_COPY);
    if (!r)
        return MHD_NO;
    MHD_add_response_header(r, "Content-Type", mime);
    MHD_add_response_header(r, "X-Content-Type-Options", "nosniff");
    MHD_add_response_header(r, "Referrer-Policy", "no-referrer");
    MHD_add_response_header(r, "Cache-Control", "no-store");
    MHD_add_response_header(r, "Content-Security-Policy",
                            "default-src 'self'; img-src 'self' https:; style-src 'self' "
                            "'unsafe-inline'; script-src 'self'; "
                            "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'");
    enum MHD_Result rc = MHD_queue_response(c, code, r);
    MHD_destroy_response(r);
    return rc;
}
static enum MHD_Result json_response(struct MHD_Connection *c, unsigned code, cJSON *o) {
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s)
        return MHD_NO;
    enum MHD_Result rc = respond(c, code, s, strlen(s), "application/json");
    free(s);
    return rc;
}
static enum MHD_Result error(struct MHD_Connection *c, unsigned code, const char *message) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", message);
    return json_response(c, code, o);
}
static bool numeric_host(const char *host) {
    if (!host || strlen(host) > 100)
        return false;
    char h[128];
    copy_text(h, sizeof h, host);
    char *p = strchr(h, ':');
    if (p)
        *p = 0;
    if (!strcmp(h, "localhost"))
        return true;
    struct in_addr a;
    return inet_pton(AF_INET, h, &a) == 1;
}
static bool local_peer(struct MHD_Connection *c) {
    const union MHD_ConnectionInfo *i =
        MHD_get_connection_info(c, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
    if (!i || !i->client_addr || i->client_addr->sa_family != AF_INET)
        return false;
    const struct sockaddr_in *a = (const struct sockaddr_in *)i->client_addr;
    return (ntohl(a->sin_addr.s_addr) >> 24) == 127;
}
static bool authorized(struct MHD_Connection *c) {
    const char *h = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Authorization");
    if (!h || strncmp(h, "Bearer ", 7) || strlen(h + 7) != 64)
        return false;
    bool yes = false;
    pthread_mutex_lock(&orbit.mutex);
    for (size_t i = 0; i < orbit.token_count; i++)
        if (CRYPTO_memcmp(h + 7, orbit.tokens[i], 64) == 0)
            yes = true;
    pthread_mutex_unlock(&orbit.mutex);
    return yes;
}
/* The console's LAN address, which the TV app shows so a phone knows where to open Orbit: the
 * source address the default route would use. Connecting a UDP socket sends nothing, and
 * 192.0.2.1 (TEST-NET-1) is never contacted. Empty without a route. */
static void lan_address(char *out, size_t cap) {
    out[0] = 0;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return;
    struct sockaddr_in peer = {0}, self = {0};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(53);
    inet_pton(AF_INET, "192.0.2.1", &peer.sin_addr);
    socklen_t length = sizeof self;
    if (!connect(fd, (const struct sockaddr *)&peer, sizeof peer) &&
        !getsockname(fd, (struct sockaddr *)&self, &length) && self.sin_family == AF_INET &&
        self.sin_addr.s_addr != 0 && (ntohl(self.sin_addr.s_addr) >> 24) != 127 &&
        !inet_ntop(AF_INET, &self.sin_addr, out, (socklen_t)cap))
        out[0] = 0;
    close(fd);
}
static cJSON *storage_json(void) {
    Storage s[ORBIT_MAX_STORAGE];
    size_t n = storage_list(s);
    cJSON *a = cJSON_CreateArray();
    pthread_mutex_lock(&orbit.mutex);
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", s[i].id);
        cJSON_AddStringToObject(o, "label", s[i].label);
        char path[540];
        snprintf(path, sizeof path, "%s/homebrew", s[i].root);
        cJSON_AddStringToObject(o, "path", path);
        cJSON_AddNumberToObject(o, "freeBytes", (double)s[i].free_bytes);
        cJSON_AddNumberToObject(o, "totalBytes", (double)s[i].total_bytes);
        uint64_t pending = storage_pending_locked(&s[i], NULL);
        cJSON_AddNumberToObject(o, "pendingBytes", (double)pending);
        cJSON_AddNumberToObject(o, "projectedFreeBytes", (double)s[i].free_bytes - (double)pending);
        cJSON_AddBoolToObject(o, "external", s[i].external);
        cJSON_AddItemToArray(a, o);
    }
    pthread_mutex_unlock(&orbit.mutex);
    return a;
}
static enum MHD_Result route(struct MHD_Connection *c, const char *url, const char *method,
                             Request *req) {
    const char *host = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Host");
    if (!numeric_host(host))
        return error(c, 403, "Use the console IP address to access Orbit.");
    bool get = !strcmp(method, "GET"), post = !strcmp(method, "POST");
    if (!get && !post)
        return error(c, 405, "Method not supported.");
    if (post) {
        const char *origin = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Origin");
        char expected[150];
        snprintf(expected, sizeof expected, "http://%s", host);
        if (origin && strcmp(origin, expected))
            return error(c, 403, "Origin does not match this console.");
        const char *ct = MHD_lookup_connection_value(c, MHD_HEADER_KIND, "Content-Type");
        if (!ct || strncasecmp(ct, "application/json", 16))
            return error(c, 415, "Use application/json.");
    }
    if (get && !strcmp(url, "/api/v1/system")) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", "Orbit Store");
        cJSON_AddStringToObject(o, "version", ORBIT_VERSION);
        cJSON_AddStringToObject(o, "platform", ORBIT_PLATFORM);
        cJSON_AddBoolToObject(o, "paired", authorized(c));
        cJSON_AddBoolToObject(o, "localSessionAvailable", local_peer(c));
#if defined(ORBIT_DESKTOP) && !defined(ORBIT_TEST)
        cJSON_AddBoolToObject(o, "pairNotificationAvailable", false);
#else
        cJSON_AddBoolToObject(o, "pairNotificationAvailable", true);
#endif
        cJSON_AddBoolToObject(o, "consoleValidated", false);
        cJSON_AddStringToObject(o, "targetFirmware", "12.60 / Relapse");
        pthread_mutex_lock(&orbit.mutex);
        cJSON_AddBoolToObject(o, "stateHealthy", !orbit.state_failed);
        cJSON_AddNumberToObject(o, "catalogueRevision", orbit.catalog_revision);
        cJSON_AddStringToObject(o, "preferredStorage", orbit.preferred_storage);
        cJSON_AddNumberToObject(o, "httpPort", orbit.port);
        cJSON_AddStringToObject(o, "launcherStatus", orbit.launcher_status);
        cJSON_AddStringToObject(o, "launcherError", orbit.launcher_error);
        cJSON_AddStringToObject(o, "launcherRegistrationMethod",
                                orbit.launcher_registration_method);
        pthread_mutex_unlock(&orbit.mutex);
        return json_response(c, 200, o);
    }
    if (get && !strcmp(url, "/api/v1/catalog/updates"))
        return json_response(c, 200, catalog_status());
    if (get && !strcmp(url, "/api/v1/catalog")) {
        cJSON *a = cJSON_CreateArray(), *x;
        pthread_mutex_lock(&orbit.mutex);
        cJSON_ArrayForEach(x, orbit.catalog) {
            if (!source_enabled_locked(release_find(json_text(x, "id"))))
                continue;
            cJSON *entry = cJSON_Duplicate(x, true);
            cJSON_DeleteItemFromObjectCaseSensitive(entry, "url");
            cJSON_DeleteItemFromObjectCaseSensitive(entry, "browserUrl");
            cJSON_AddBoolToObject(entry, "browserAvailable",
                provider_browser_supported(release_find(json_text(x, "id"))));
            cJSON_AddBoolToObject(entry, "directAvailable",
                provider_url_supported(release_find(json_text(x, "id"))));
            cJSON_AddItemToArray(a, entry);
        }
        pthread_mutex_unlock(&orbit.mutex);
        return json_response(c, 200, a);
    }
    if (get && !strcmp(url, "/api/v1/sources")) {
        pthread_mutex_lock(&orbit.mutex);
        cJSON *o = sources_json_locked();
        pthread_mutex_unlock(&orbit.mutex);
        return json_response(c, 200, o);
    }
    if (get && !strcmp(url, "/api/v1/session")) {
        if (!local_peer(c))
            return error(c, 403, "Pair this device using the code shown on your PS5.");
        char address[INET_ADDRSTRLEN];
        lan_address(address, sizeof address);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "address", address);
        pthread_mutex_lock(&orbit.mutex);
        if (!orbit.token_count) {
            random_hex(orbit.tokens[0], 32);
            orbit.token_count = 1;
        }
        int rc = state_save_locked();
        cJSON_AddStringToObject(o, "token", orbit.tokens[0]);
        cJSON_AddStringToObject(o, "pairCode", orbit.pair_code);
        pthread_mutex_unlock(&orbit.mutex);
        if (rc) {
            cJSON_Delete(o);
            return error(c, 503, "Could not save pairing.");
        }
        return json_response(c, 200, o);
    }
    cJSON *input = NULL;
    if (post) {
        input = cJSON_Parse(req->body);
        if (!cJSON_IsObject(input)) {
            cJSON_Delete(input);
            return error(c, 400, "Expected a JSON object.");
        }
    }
    if (post && !strcmp(url, "/api/v1/pair/show")) {
        cJSON_Delete(input);
        struct timespec clock;
        if (clock_gettime(CLOCK_MONOTONIC, &clock))
            return error(c, 503, "Could not show the pairing code. Try again.");
        pthread_mutex_lock(&orbit.mutex);
        time_t remaining = orbit.pair_notify_after - clock.tv_sec;
        int rc = 0;
        if (remaining <= 0) {
            rc = pairing_notify();
            /* Failed notifications are throttled too, so repeated requests cannot flood the API. */
            orbit.pair_notify_after = clock.tv_sec + 30;
        }
        pthread_mutex_unlock(&orbit.mutex);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "retryAfter", remaining > 0 ? (double)remaining : 30);
        if (remaining > 0)
            cJSON_AddStringToObject(o, "error", "Wait a moment before showing the code again.");
        else if (rc)
            cJSON_AddStringToObject(
                o, "error",
                "Could not show a notification. Open Pair devices in Orbit on your PS5.");
        else
            cJSON_AddBoolToObject(o, "shown", true);
        /* Only the console displays the code. This response never grants a session or token. */
        return json_response(c, remaining > 0 ? 429 : rc ? 503 : 200, o);
    }
    if (post && !strcmp(url, "/api/v1/pair")) {
        pthread_mutex_lock(&orbit.mutex);
        time_t now = time(NULL);
        int code = 200;
        char token[65] = {0};
        if (now < orbit.pair_locked_until)
            code = 429;
        else if (strlen(json_text(input, "code")) != 6 ||
                 CRYPTO_memcmp(json_text(input, "code"), orbit.pair_code, 6) != 0) {
            code = 403;
            if (++orbit.pair_failures >= 5) {
                orbit.pair_locked_until = now + 60;
                orbit.pair_failures = 0;
            }
        } else if (orbit.token_count == 16)
            code = 409;
        else {
            random_hex(token, 32);
            copy_text(orbit.tokens[orbit.token_count++], 65, token);
            orbit.pair_failures = 0;
            if (state_save_locked()) {
                orbit.token_count--;
                code = 503;
            }
        }
        pthread_mutex_unlock(&orbit.mutex);
        cJSON_Delete(input);
        if (code != 200)
            return error(c, (unsigned)code,
                         code == 429   ? "Too many attempts. Wait one minute."
                         : code == 409 ? "Pairing slots are full."
                                       : "Pairing failed. Check the console code.");
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "token", token);
        return json_response(c, 200, o);
    }
    if (!strncmp(url, "/api/", 5)) {
        if (!authorized(c)) {
            cJSON_Delete(input);
            return error(c, 401, "Pair this device with your PS5 to continue.");
        }
        if (get && !strcmp(url, "/api/v1/storage"))
            return json_response(c, 200, storage_json());
        if (post && !strcmp(url, "/api/v1/storage/preferred")) {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(input, "storageId");
            char e[256] = {0}, chosen[24] = {0};
            int code = 400;
            if (cJSON_IsString(id) && strlen(id->valuestring) < sizeof chosen) {
                copy_text(chosen, sizeof chosen, id->valuestring);
                code = storage_prefer(chosen, e, sizeof e);
            }
            cJSON_Delete(input);
            if (code != 200)
                return error(c, (unsigned)code, code == 400 ? "Expected storageId." : e);
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "preferredStorage", chosen);
            return json_response(c, 200, o);
        }
        if (get && !strncmp(url, "/api/v1/art/", 12)) {
            /* Catalogue artwork for the native TV app (backend/art.c). */
            char game[64] = {0}, kind[8] = {0}, trailing;
            if (sscanf(url + 12, "%63[^/]/%7[^/]%c", game, kind, &trailing) != 2)
                return error(c, 404, "Unknown artwork.");
            unsigned char *data = NULL;
            size_t size = 0;
            char mime[16] = {0};
            int code = art_get(game, kind, &data, &size, mime, sizeof mime);
            if (code == 200) {
                enum MHD_Result rc = respond(c, 200, (const char *)data, size, mime);
                free(data);
                return rc;
            }
            if (code == 202) {
                cJSON *o = cJSON_CreateObject();
                cJSON_AddBoolToObject(o, "pending", true);
                return json_response(c, 202, o);
            }
            return error(c, (unsigned)code,
                         code == 404   ? "Unknown artwork."
                         : code == 503 ? "Artwork is busy. Try again shortly."
                                       : "This artwork could not be loaded.");
        }
        if (get && !strcmp(url, "/api/v1/debrid"))
            return json_response(c, 200, debrid_status());
        if (post && !strcmp(url, "/api/v1/debrid")) {
            char e[256] = {0};
            int code = debrid_action(input, e, sizeof e);
            cJSON *key = cJSON_GetObjectItemCaseSensitive(input, "apiKey");
            if (cJSON_IsString(key)) OPENSSL_cleanse(key->valuestring, strlen(key->valuestring));
            cJSON_Delete(input);
            return code == 202 ? json_response(c, 202, debrid_status()) : error(c, (unsigned)code, e);
        }
        if (get && !strcmp(url, "/api/v1/browser"))
            return json_response(c, 200, browser_status());
        if (post && !strcmp(url, "/api/v1/browser")) {
            char e[256] = {0};
            int code = browser_action(input, e, sizeof e);
            cJSON_Delete(input);
            return code == 202 ? json_response(c, 202, browser_status()) : error(c, (unsigned)code, e);
        }
        if (get && !strcmp(url, "/api/v1/diagnostics"))
            return json_response(c, 200, diagnostics_snapshot());
        if ((get || post) && !strcmp(url, "/api/v1/library")) {
            cJSON_Delete(input);
            return json_response(c, 200, library_snapshot(post));
        }
        if ((get || post) && !strcmp(url, "/api/v1/library/storage")) {
            cJSON_Delete(input);
            return json_response(c, 200, library_storage_snapshot(post));
        }
        if (post && !strcmp(url, "/api/v1/library/actions")) {
            char e[256] = {0};
            int code = library_action(input, e, sizeof e);
            cJSON_Delete(input);
            return code == 202 ? json_response(c, 202, library_snapshot(false))
                               : error(c, (unsigned)code, e);
        }
        if ((get || post) && !strcmp(url, "/api/v1/favorites")) {
            char e[256] = {0};
            pthread_mutex_lock(&orbit.mutex);
            int code = post ? favorite_set_locked(input, e, sizeof e) : 200;
            cJSON *o = code == 200 ? favorites_json_locked() : NULL;
            pthread_mutex_unlock(&orbit.mutex);
            cJSON_Delete(input);
            return o ? json_response(c, 200, o) : error(c, (unsigned)code, e);
        }
        if (post && !strcmp(url, "/api/v1/downloads/clear-history")) {
            char e[256] = {0};
            pthread_mutex_lock(&orbit.mutex);
            int code = history_clear_locked(input, e, sizeof e);
            cJSON *o = code == 200 ? jobs_json_locked() : NULL;
            pthread_mutex_unlock(&orbit.mutex);
            cJSON_Delete(input);
            return o ? json_response(c, 200, o) : error(c, (unsigned)code, e);
        }
        if (post && !strcmp(url, "/api/v1/sources")) {
            char e[256] = {0};
            pthread_mutex_lock(&orbit.mutex);
            int code = sources_set_locked(input, e, sizeof e);
            cJSON *o = code == 200 ? sources_json_locked() : NULL;
            pthread_mutex_unlock(&orbit.mutex);
            cJSON_Delete(input);
            return o ? json_response(c, 200, o) : error(c, (unsigned)code, e);
        }
        if (get && !strcmp(url, "/api/v1/downloads")) {
            pthread_mutex_lock(&orbit.mutex);
            cJSON *o = jobs_json_locked();
            pthread_mutex_unlock(&orbit.mutex);
            return json_response(c, 200, o);
        }
        if (post && !strcmp(url, "/api/v1/downloads")) {
            const cJSON *delivery = cJSON_GetObjectItemCaseSensitive(input, "delivery");
            if (delivery && !cJSON_IsString(delivery)) {
                cJSON_Delete(input);
                return error(c, 400, "Choose direct or torbox delivery.");
            }
            char e[256] = {0};
            Job *j = NULL;
            pthread_mutex_lock(&orbit.mutex);
            int code = job_create_delivery_locked(json_text(input, "releaseId"),
                                         json_text(input, "storageId"), json_text(input, "delivery"), e, sizeof e, &j);
            cJSON *o = NULL;
            if (code == 201) {
                o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "id", j->id);
            }
            pthread_mutex_unlock(&orbit.mutex);
            cJSON_Delete(input);
            return o ? json_response(c, 201, o) : error(c, (unsigned)code, e);
        }
        if (post && !strcmp(url, "/api/v1/catalog/updates")) {
            char e[256] = {0};
            int code =
                !strcmp(json_text(input, "action"), "refresh") ? catalog_refresh(e, sizeof e) : 400;
            cJSON_Delete(input);
            return code == 202 ? json_response(c, 202, catalog_status())
                               : error(c, (unsigned)code, *e ? e : "Expected refresh action.");
        }
        if (get && !strcmp(url, "/api/v1/updates"))
            return json_response(c, 200, updates_status());
        if (post && !strcmp(url, "/api/v1/updates")) {
            char e[256] = {0};
            int code = updates_action(input, e, sizeof e);
            cJSON_Delete(input);
            return code == 202 ? json_response(c, 202, updates_status())
                               : error(c, (unsigned)code, e);
        }
        if (get && !strcmp(url, "/api/v1/tv-app"))
            return json_response(c, 200, tvapp_status());
        if (post && !strcmp(url, "/api/v1/tv-app")) {
            char e[256] = {0};
            int code = tvapp_action(input, e, sizeof e);
            cJSON_Delete(input);
            return code == 202 ? json_response(c, 202, tvapp_status())
                               : error(c, (unsigned)code, e);
        }
        if (get && !strcmp(url, "/api/v1/autoboot"))
            return json_response(c, 200, autoboot_status());
        if (post && (!strcmp(url, "/api/v1/autoboot") || !strcmp(url, "/api/v1/integrations"))) {
            char e[256] = {0};
            const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(input, "enabled");
            int code = !cJSON_IsBool(enabled) ? 400 :
                !strcmp(url, "/api/v1/integrations")
                    ? integration_set(json_text(input, "manager"), cJSON_IsTrue(enabled), e, sizeof e)
                    : autoboot_set(json_text(input, "manager"), cJSON_IsTrue(enabled), e, sizeof e);
            cJSON_Delete(input);
            if (code == 200)
                return json_response(c, 200, autoboot_status());
            return error(c, (unsigned)code, code == 400 ? "Expected manager and enabled." : e);
        }
        if (post && !strncmp(url, "/api/v1/downloads/", 18)) {
            char id[24] = {0}, action[24] = {0}, trailing;
            int parsed = sscanf(url + 18, "%23[^/]/%23[^/]%c", id, action, &trailing);
            if (parsed != 2) {
                cJSON_Delete(input);
                return error(c, 404, "Unknown download action.");
            }
            char e[256] = {0};
            pthread_mutex_lock(&orbit.mutex);
            Job *j = job_find(id);
            int code =
                j ? job_action_locked(
                        j, action,
                        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(input, "deletePartial")), e,
                        sizeof e)
                  : 404;
            cJSON *o = code == 200 ? jobs_json_locked() : NULL;
            pthread_mutex_unlock(&orbit.mutex);
            cJSON_Delete(input);
            return o ? json_response(c, 200, o)
                     : error(c, (unsigned)code, j ? e : "Unknown download.");
        }
        cJSON_Delete(input);
        return error(c, 404, "Unknown endpoint.");
    }
    cJSON_Delete(input);
    if (!get)
        return error(c, 405, "Method not supported.");
    const char *path = !strcmp(url, "/") ? "/index.html" : url;
    for (size_t i = 0; i < sizeof assets / sizeof assets[0]; i++)
        if (!strcmp(path, assets[i].path))
            return respond(c, 200, (const char *)assets[i].data, assets[i].size, assets[i].mime);
    return error(c, 404, "Not found.");
}
static enum MHD_Result handle(void *cls, struct MHD_Connection *c, const char *url,
                              const char *method, const char *version, const char *data,
                              size_t *size, void **con_cls) {
    (void)cls;
    (void)version;
    if (!*con_cls) {
        *con_cls = calloc(1, sizeof(Request));
        return *con_cls ? MHD_YES : MHD_NO;
    }
    Request *r = *con_cls;
    if (*size) {
        if (r->length + *size > 4096)
            r->too_large = true;
        else {
            memcpy(r->body + r->length, data, *size);
            r->length += *size;
            r->body[r->length] = 0;
        }
        *size = 0;
        return MHD_YES;
    }
    if (r->too_large)
        return error(c, 413, "Request body is too large.");
    return route(c, url, method, r);
}
static void completed(void *cls, struct MHD_Connection *c, void **con_cls,
                      enum MHD_RequestTerminationCode reason) {
    (void)cls;
    (void)c;
    (void)reason;
    if (*con_cls) OPENSSL_cleanse(*con_cls, sizeof(Request));
    free(*con_cls);
    *con_cls = NULL;
}
/* One owner drives and rebuilds the HTTP daemon. An internal MHD thread can
 * survive a lost listener without reopening it; stopping that thread also
 * depends on its old wake socket. External polling gives us a bounded wait
 * and lets the same owner tear down stale connections before rebinding. */
static struct MHD_Daemon *listen_start(void) {
    struct sockaddr_in bind_address = {0};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = htons((uint16_t)orbit.port);
    bind_address.sin_addr.s_addr =
        htonl(orbit.desktop && !orbit.listen_all ? INADDR_LOOPBACK : INADDR_ANY);
    return MHD_start_daemon(
        MHD_NO_FLAG, (uint16_t)orbit.port, NULL, NULL, handle, NULL,
        MHD_OPTION_SOCK_ADDR, &bind_address, MHD_OPTION_CONNECTION_LIMIT, 32U,
        MHD_OPTION_CONNECTION_TIMEOUT, 15U, MHD_OPTION_CONNECTION_MEMORY_LIMIT, (size_t)32768,
        MHD_OPTION_NOTIFY_COMPLETED, completed, NULL, MHD_OPTION_END);
}
static bool listener_address(int fd) {
    struct sockaddr_in address = {0};
    socklen_t size = sizeof address;
    return getsockname(fd, (struct sockaddr *)&address, &size) == 0 &&
           address.sin_family == AF_INET && address.sin_port == htons((uint16_t)orbit.port) &&
           address.sin_addr.s_addr ==
               htonl(orbit.desktop && !orbit.listen_all ? INADDR_LOOPBACK : INADDR_ANY);
}
static bool listener_valid(void) {
    const union MHD_DaemonInfo *info =
        MHD_get_daemon_info(daemon_handle, MHD_DAEMON_INFO_LISTEN_FD);
    if (!info || info->listen_fd == MHD_INVALID_SOCKET)
        return false;
    int listening = 0;
    socklen_t size = sizeof listening;
    return getsockopt(info->listen_fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &size) == 0 &&
           listening && listener_address(info->listen_fd);
}
/* A stale socket may still report SO_ACCEPTCONN after the network stack resets.
 * Test the kernel's loopback handshake, not an HTTP response: a busy route or
 * slow disk must not be mistaken for listener loss. Resource exhaustion and a
 * full backlog are inconclusive; only a refused connection counts as dead. */
static bool listener_refused(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return false;
    }
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)orbit.port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int result = connect(fd, (const struct sockaddr *)&address, sizeof address);
    int error = result == 0 ? 0 : errno;
    if (error == EINPROGRESS || error == EWOULDBLOCK) {
        struct pollfd event = {.fd = fd, .events = POLLOUT};
        socklen_t size = sizeof error;
        error = 0;
        if (poll(&event, 1, 50) > 0 &&
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0)
            error = 0;
    }
    close(fd);
    return error == ECONNREFUSED;
}
static void listen_stop(void) {
    if (!daemon_handle) return;
    /* Detach the listener first: MHD aborts if it closes an already-invalid
     * listen FD. Never close a descriptor that now refers to another file. */
    int fd = MHD_quiesce_daemon(daemon_handle);
    MHD_stop_daemon(daemon_handle);
    daemon_handle = NULL;
    if (fd != MHD_INVALID_SOCKET && listener_address(fd)) close(fd);
}
static void retry_wait(unsigned milliseconds) {
    while (milliseconds && !atomic_load(&api_stopping)) {
        unsigned step = milliseconds < 100 ? milliseconds : 100;
        struct timespec delay = {.tv_nsec = (long)step * 1000000};
        nanosleep(&delay, NULL);
        milliseconds -= step;
    }
}
static void *api_worker(void *unused) {
    (void)unused;
    time_t next_check = 0;
    unsigned refused = 0, retry_ms = 500;
    bool recovering = false;
    while (!atomic_load(&api_stopping)) {
        if (!daemon_handle) {
            retry_wait(retry_ms);
            if (atomic_load(&api_stopping)) break;
            daemon_handle = listen_start();
            if (!daemon_handle) {
                retry_ms = retry_ms < 8000 ? retry_ms * 2 : 8000;
                continue;
            }
            retry_ms = 500;
            refused = 0;
            next_check = 0;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        bool healthy = true;
        if (now.tv_sec >= next_check) {
            next_check = now.tv_sec + 2;
            healthy = listener_valid();
            if (healthy) {
                refused = listener_refused() ? refused + 1 : 0;
                healthy = refused < 3;
            }
        }
        if (healthy && MHD_run_wait(daemon_handle, 250) == MHD_YES) {
            if (recovering) {
                diagnostics_stage("listener-recovery", "ok", 0);
                recovering = false;
            }
            continue;
        }
        if (!recovering) diagnostics_stage("listener-recovery", "started", 0);
        recovering = true;
        listen_stop();
    }
    listen_stop();
    return NULL;
}
int api_start(void) {
    if (api_thread_started) return -1;
    daemon_handle = listen_start();
    if (!daemon_handle) return -1;
    atomic_store(&api_stopping, false);
    pthread_attr_t attr;
    if (pthread_attr_init(&attr)) {
        listen_stop();
        return -1;
    }
    int result = pthread_attr_setstacksize(&attr, ORBIT_THREAD_STACK);
    if (!result) result = pthread_create(&api_thread, &attr, api_worker, NULL);
    pthread_attr_destroy(&attr);
    if (result) {
        listen_stop();
        return -1;
    }
    api_thread_started = true;
    return 0;
}
void api_stop(void) {
    if (!api_thread_started) return;
    atomic_store(&api_stopping, true);
    pthread_join(api_thread, NULL);
    api_thread_started = false;
}
