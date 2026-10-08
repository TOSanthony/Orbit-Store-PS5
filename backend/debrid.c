/* TorBox web downloads. Credentials and signed CDN URLs stay in the backend. */
#include "debrid.h"
#include "ca.h"
#include "provider_network.h"
#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#define KEY_CAP 513
#define RESPONSE_MAX (1024 * 1024)
#define HOST_TTL 600
static struct {
    pthread_t thread;
    bool started, stop, busy, connected, check;
    char key[KEY_CAP], pending_key[KEY_CAP], account[65], error[256];
    time_t checked, retry_at, next_check;
    unsigned revision;
    struct { bool available; int64_t limit; } hosts[2];
} debrid;
typedef struct {
    char *body;
    size_t length;
    long status;
    time_t retry_at;
    Job *job;
} Reply;
static const char *api_base(void) {
#ifdef ORBIT_TEST
    const char *base = getenv("ORBIT_TEST_TORBOX_URL");
    return base && !strncmp(base, "http://127.0.0.1:", 17) ? base : NULL;
#else
    return "https://api.torbox.app/v1/api";
#endif
}
static bool halted(Job *job) {
    pthread_mutex_lock(&orbit.mutex);
    bool stop = orbit.stop || debrid.stop || (job && (job->pause || job->cancel ||
                !source_enabled_locked(&job->release)));
    pthread_mutex_unlock(&orbit.mutex);
    return stop;
}
static int progress(void *ctx, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)a; (void)b; (void)c; (void)d;
    return halted(((Reply *)ctx)->job);
}
static size_t collect(char *data, size_t a, size_t b, void *ctx) {
    Reply *r = ctx;
    if (a && b > SIZE_MAX / a) return 0;
    size_t n = a * b;
    if (n > RESPONSE_MAX - r->length) return 0;
    char *next = realloc(r->body, r->length + n + 1);
    if (!next) return 0;
    r->body = next;
    memcpy(next + r->length, data, n); r->length += n; next[r->length] = 0;
    return n;
}
static size_t headers(char *data, size_t a, size_t b, void *ctx) {
    Reply *r = ctx;
    if (a && b > SIZE_MAX / a) return 0;
    size_t n = a * b;
    if (n > 12 && !strncasecmp(data, "Retry-After:", 12)) {
        char value[100]; size_t len = n - 12;
        if (len < sizeof value) {
            memcpy(value, data + 12, len); value[len] = 0;
            char *end = NULL; long wait = strtol(value, &end, 10);
            while (end && isspace((unsigned char)*end)) end++;
            time_t at = end && end != value && !*end && wait >= 0 && wait <= 86400
                      ? time(NULL) + wait : curl_getdate(value, NULL);
            if (at > r->retry_at) r->retry_at = at;
        }
    }
    return n;
}
static void message(const cJSON *json, long status, char *err, size_t cap) {
    const char *code = json_text(json, "error");
    const char *text = status == 401 || status == 403 || !strcmp(code, "BAD_TOKEN") || !strcmp(code, "NO_AUTH")
        ? "TorBox rejected the API key. Reconnect your account."
        : status == 429 ? "TorBox rate limit reached. Wait before retrying."
        : !strcmp(code, "PLAN_RESTRICTED_FEATURE") ? "Your TorBox plan does not include this download feature."
        : !strcmp(code, "DOWNLOAD_TOO_LARGE") ? "This file exceeds your TorBox download limit."
        : !strcmp(code, "ITEM_NOT_FOUND") ? "This download is no longer in your TorBox account."
        : !strcmp(code, "DUPLICATE_ITEM") ? "This link is already in your TorBox account. Retry to check it."
        : "TorBox could not prepare this file. Check your account and try again.";
    /* Never relay upstream text: it may contain credentials, a URL or account data. */
    copy_text(err, cap, text);
}
static cJSON *call(const char *key, const char *path, const char *form, bool bearer,
                   Job *job, time_t *retry, char *err, size_t cap, bool *rejected) {
    if (rejected) *rejected = false;
    const char *base = api_base();
    if (!base || halted(job)) { if (rejected) *rejected = true; copy_text(err, cap, "TorBox request stopped."); return NULL; }
    char url[4096], authorization[KEY_CAP + 32];
    if (snprintf(url, sizeof url, "%s%s", base, path) >= (int)sizeof url) return NULL;
    Reply reply = {.job = job};
    CURL *curl = curl_easy_init(); struct curl_slist *h = NULL;
    if (!curl) { copy_text(err, cap, "Could not initialize TorBox HTTPS."); return NULL; }
    if (bearer) {
        snprintf(authorization, sizeof authorization, "Authorization: Bearer %s", key);
        h = curl_slist_append(h, authorization);
        OPENSSL_cleanse(authorization, sizeof authorization);
    }
    h = curl_slist_append(h, "Accept: application/json");
    if (form) h = curl_slist_append(h, "Content-Type: application/x-www-form-urlencoded");
    struct curl_blob ca = {(void *)orbit_ca, sizeof orbit_ca - 1, CURL_BLOB_COPY};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
#ifdef ORBIT_TEST
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#endif
    /* Authentication is sent to the fixed API only; never follow its redirects. */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, provider_open_socket);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    if (form) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, form);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "OrbitStore/" ORBIT_VERSION);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &reply);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headers);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &reply);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &reply);
    CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &reply.status);
    curl_easy_cleanup(curl);
    for (struct curl_slist *p = h; p; p = p->next) OPENSSL_cleanse(p->data, strlen(p->data));
    curl_slist_free_all(h); OPENSSL_cleanse(url, sizeof url);
    cJSON *json = rc == CURLE_OK && reply.body ? cJSON_Parse(reply.body) : NULL;
    if (reply.body) { OPENSSL_cleanse(reply.body, reply.length); free(reply.body); }
    if (reply.status == 429 && reply.retry_at < time(NULL) + 60) reply.retry_at = time(NULL) + 60;
    if (reply.retry_at > *retry) *retry = reply.retry_at;
    if (rc != CURLE_OK || reply.status != 200 || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "success"))) {
        /* A complete rejection can be retried. A lost response or server failure
           may have created a remote item, so the queue must reconcile first. */
        if (rejected && rc == CURLE_OK &&
            (reply.status == 400 || reply.status == 401 || reply.status == 403 || reply.status == 429 ||
             (reply.status == 200 && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json, "success")) &&
              strcmp(json_text(json, "error"), "DUPLICATE_ITEM")))) *rejected = true;
        if (rc != CURLE_OK) copy_text(err, cap, halted(job) ? "TorBox request stopped." : "Could not reach TorBox. Check your connection and retry.");
        else message(json, reply.status, err, cap);
        cJSON_Delete(json); return NULL;
    }
    return json;
}
static bool identifier(const cJSON *o, const char *key, int64_t *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 0 ||
        v->valuedouble > 9007199254740991.0 || floor(v->valuedouble) != v->valuedouble) return false;
    *out = (int64_t)v->valuedouble; return true;
}
static bool account_hash(const cJSON *data, char out[65]) {
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(data, "id");
    if ((!cJSON_IsString(id) || !*id->valuestring || strlen(id->valuestring) > 128) && !cJSON_IsNumber(id)) return false;
    char *identity = cJSON_PrintUnformatted(id); unsigned char hash[32]; unsigned n = 0;
    bool ok = identity && EVP_Digest(identity, strlen(identity), hash, &n, EVP_sha256(), NULL) && n == 32;
    free(identity);
    if (ok) for (size_t i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", hash[i]);
    return ok;
}
static int persist(const char *key, const char *account) {
    if (!key[0]) {
        if (unlinkat(orbit.state_fd, "debrid.json", 0) && errno != ENOENT) return -1;
        return fsync(orbit.state_fd);
    }
    cJSON *o = cJSON_CreateObject(); cJSON_AddNumberToObject(o, "schema", 1);
    cJSON_AddStringToObject(o, "key", key); cJSON_AddStringToObject(o, "account", account);
    char *json = cJSON_PrintUnformatted(o);
    cJSON *secret = cJSON_GetObjectItemCaseSensitive(o, "key");
    if (secret && secret->valuestring) OPENSSL_cleanse(secret->valuestring, strlen(secret->valuestring));
    cJSON_Delete(o); if (!json) return -1;
    char name[64], random[17]; random_hex(random, 8); snprintf(name, sizeof name, "debrid-%s.tmp", random);
    int fd = openat(orbit.state_fd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600), rc = -1;
    if (fd >= 0) {
        size_t size = strlen(json), done = 0;
        while (done < size) { ssize_t n = write(fd, json + done, size - done); if (n <= 0) break; done += (size_t)n; }
        if (done == size && !fsync(fd)) rc = 0;
        if (close(fd)) rc = -1;
        if (!rc) rc = renameat(orbit.state_fd, name, orbit.state_fd, "debrid.json");
        if (!rc) rc = fsync(orbit.state_fd);
        unlinkat(orbit.state_fd, name, 0);
    }
    OPENSSL_cleanse(json, strlen(json)); free(json); return rc;
}
static bool valid_key(const char *key) {
    size_t n = strlen(key); if (n < 16 || n >= KEY_CAP) return false;
    for (const unsigned char *p = (const void *)key; *p; p++)
        if (!(isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '=')) return false;
    return true;
}
static void pause_jobs_locked(void) {
    for (size_t i = 0; i < orbit.job_count; i++) {
        Job *j = &orbit.jobs[i];
        if (strcmp(j->delivery, "torbox") || !j->id[0] || !strcmp(j->status, "complete") || !strcmp(j->status, "cancelled")) continue;
        j->pause = true;
        if (strcmp(j->status, "downloading") && strcmp(j->status, "verifying")) copy_text(j->status, sizeof j->status, "paused");
    }
}
static void check_account(const char *key) {
    char err[256] = {0}, account[65] = {0}; time_t retry = 0;
    bool available[2] = {false, false}; int64_t limits[2] = {0, 0};
    cJSON *user = call(key, "/user/me?settings=false", NULL, true, NULL, &retry, err, sizeof err, NULL);
    bool ok = user && account_hash(cJSON_GetObjectItemCaseSensitive(user, "data"), account);
    cJSON_Delete(user);
    if (!ok && !err[0]) copy_text(err, sizeof err, "TorBox returned an invalid account response.");
    cJSON *hosters = ok ? call(key, "/webdl/hosters", NULL, true, NULL, &retry, err, sizeof err, NULL) : NULL;
    const cJSON *hosts = cJSON_GetObjectItemCaseSensitive(hosters, "data"), *host;
    ok = ok && cJSON_IsArray(hosts);
    cJSON_ArrayForEach(host, hosts) {
        const cJSON *domain;
        cJSON_ArrayForEach(domain, cJSON_GetObjectItemCaseSensitive(host, "domains")) {
            if (!cJSON_IsString(domain)) continue;
            int index = !strcmp(domain->valuestring, "archive.org") ? 0 :
                (!strcmp(domain->valuestring, "vikingfile.com") || !strcmp(domain->valuestring, "vik1ngfile.site")) ? 1 : -1;
            if (index < 0) continue;
            int64_t limit = 0, daily = 0, used = 0, links = 0, link_used = 0;
            bool valid = identifier(host, "per_link_size_limit", &limit);
            bool bandwidth = !identifier(host, "daily_bandwidth_limit", &daily) || !daily ||
                             (identifier(host, "daily_bandwidth_used", &used) && used < daily);
            bool count = !identifier(host, "daily_link_limit", &links) || !links ||
                         (identifier(host, "daily_link_used", &link_used) && link_used < links);
            available[index] = valid && bandwidth && count && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(host, "status"));
            limits[index] = daily > used && (!limit || daily - used < limit) ? daily - used : limit;
        }
    }
    cJSON_Delete(hosters);
    if (!ok && !err[0]) copy_text(err, sizeof err, "TorBox returned invalid host availability.");
    pthread_mutex_lock(&orbit.mutex);
    if (ok && !debrid.stop) {
        if (*debrid.account && strcmp(debrid.account, account)) {
            pause_jobs_locked();
            if (state_save_locked()) { ok = false; copy_text(err, sizeof err, "Could not pause downloads before changing TorBox accounts."); }
        }
    }
    if (ok && !debrid.stop) {
        if (persist(key, account)) { ok = false; copy_text(err, sizeof err, "Could not save the TorBox connection on this console."); }
        else {
            OPENSSL_cleanse(debrid.key, sizeof debrid.key);
            copy_text(debrid.key, sizeof debrid.key, key); copy_text(debrid.account, sizeof debrid.account, account);
            debrid.connected = true; debrid.checked = time(NULL);
            for (int i = 0; i < 2; i++) { debrid.hosts[i].available = available[i]; debrid.hosts[i].limit = limits[i]; }
            debrid.revision++;
        }
    }
    if (!ok) {
        /* A failed refresh must not leave the last successful host response
           looking usable. Keep the saved connection so it can be retried. */
        debrid.checked = 0;
        for (int i = 0; i < 2; i++) debrid.hosts[i].available = false;
        debrid.revision++;
    }
    copy_text(debrid.error, sizeof debrid.error, err);
    if (retry > debrid.retry_at) debrid.retry_at = retry;
    debrid.next_check = time(NULL) + (ok ? HOST_TTL - 60 : 60);
    if (debrid.next_check < debrid.retry_at) debrid.next_check = debrid.retry_at;
    debrid.busy = false;
    pthread_mutex_unlock(&orbit.mutex);
}
static void *worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&orbit.mutex);
    while (!debrid.stop && !orbit.stop) {
        if (!debrid.busy && debrid.connected && debrid.key[0] && time(NULL) >= debrid.next_check) {
            copy_text(debrid.pending_key, sizeof debrid.pending_key, debrid.key);
            debrid.busy = debrid.check = true;
        }
        if (debrid.check) {
            char key[KEY_CAP]; copy_text(key, sizeof key, debrid.pending_key);
            OPENSSL_cleanse(debrid.pending_key, sizeof debrid.pending_key); debrid.check = false;
            pthread_mutex_unlock(&orbit.mutex); check_account(key); OPENSSL_cleanse(key, sizeof key);
            pthread_mutex_lock(&orbit.mutex);
        } else {
            struct timespec until = {.tv_sec = time(NULL) + 1};
            pthread_cond_timedwait(&orbit.changed, &orbit.mutex, &until);
        }
    }
    pthread_mutex_unlock(&orbit.mutex); return NULL;
}
int debrid_start(void) {
    int fd = openat(orbit.state_fd, "debrid.json", O_RDONLY | O_NOFOLLOW);
    if (fd >= 0) {
        struct stat st; char buf[2048] = {0};
        if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_size > 0 && st.st_size < (off_t)sizeof buf &&
            read(fd, buf, (size_t)st.st_size) == st.st_size) {
            cJSON *o = cJSON_Parse(buf); const char *key = json_text(o, "key"), *account = json_text(o, "account");
            if (json_int(o, "schema") == 1 && valid_key(key) && strlen(account) == 64) {
                copy_text(debrid.key, sizeof debrid.key, key); copy_text(debrid.account, sizeof debrid.account, account);
                copy_text(debrid.pending_key, sizeof debrid.pending_key, key); debrid.busy = debrid.check = true;
            }
            cJSON *secret = cJSON_GetObjectItemCaseSensitive(o, "key");
            if (secret && secret->valuestring) OPENSSL_cleanse(secret->valuestring, strlen(secret->valuestring));
            cJSON_Delete(o);
        }
        OPENSSL_cleanse(buf, sizeof buf); close(fd);
    }
    pthread_attr_t attr;
    if (pthread_attr_init(&attr)) return -1;
    int rc = pthread_attr_setstacksize(&attr, ORBIT_THREAD_STACK);
    if (!rc) rc = pthread_create(&debrid.thread, &attr, worker, NULL);
    pthread_attr_destroy(&attr); debrid.started = !rc;
    if (rc) debrid.busy = debrid.check = false;
    return rc ? -1 : 0;
}
void debrid_stop(void) {
    pthread_mutex_lock(&orbit.mutex); debrid.stop = true; pthread_cond_broadcast(&orbit.changed); pthread_mutex_unlock(&orbit.mutex);
    if (debrid.started) pthread_join(debrid.thread, NULL);
    pthread_mutex_lock(&orbit.mutex);
    debrid.connected = false;
    OPENSSL_cleanse(debrid.key, sizeof debrid.key); OPENSSL_cleanse(debrid.pending_key, sizeof debrid.pending_key);
    pthread_mutex_unlock(&orbit.mutex);
}
cJSON *debrid_status(void) {
    pthread_mutex_lock(&orbit.mutex);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "provider", "torbox");
    cJSON_AddBoolToObject(o, "connected", debrid.connected);
    cJSON_AddBoolToObject(o, "busy", debrid.busy);
    cJSON_AddBoolToObject(o, "available", debrid.started);
    cJSON_AddStringToObject(o, "error", debrid.error);
    cJSON_AddNumberToObject(o, "checkedAt", (double)debrid.checked);
    cJSON_AddNumberToObject(o, "retryAt", (double)debrid.retry_at);
    cJSON_AddNumberToObject(o, "revision", debrid.revision);
    cJSON *hosts = cJSON_AddArrayToObject(o, "hosts");
    for (int i = 0; i < 2; i++) {
        cJSON *h = cJSON_CreateObject(); cJSON_AddStringToObject(h, "sourceId", i ? "vikingfile" : "archive");
        cJSON_AddBoolToObject(h, "available", debrid.connected && debrid.hosts[i].available && time(NULL) < debrid.checked + HOST_TTL);
        cJSON_AddNumberToObject(h, "maxFileSize", (double)debrid.hosts[i].limit); cJSON_AddItemToArray(hosts, h);
    }
    pthread_mutex_unlock(&orbit.mutex); return o;
}
int debrid_action(const cJSON *input, char *err, size_t cap) {
    const char *action = json_text(input, "action"), *key = json_text(input, "apiKey");
    if (strcmp(action, "connect") && strcmp(action, "refresh") && strcmp(action, "disconnect")) { copy_text(err, cap, "Choose connect, refresh or disconnect."); return 400; }
    if (!strcmp(action, "connect") && !valid_key(key)) { copy_text(err, cap, "Enter a valid TorBox API key."); return 400; }
    pthread_mutex_lock(&orbit.mutex); int code = 202;
    if (!debrid.started || debrid.stop || orbit.stop) { code = 503; copy_text(err, cap, "TorBox integration is unavailable."); }
    else if (debrid.busy) { code = 409; copy_text(err, cap, "A TorBox account check is already running."); }
    else if (!strcmp(action, "disconnect")) {
        if (persist("", "")) { code = 503; copy_text(err, cap, "Could not remove the saved TorBox connection."); }
        else {
            pause_jobs_locked();
            OPENSSL_cleanse(debrid.key, sizeof debrid.key); debrid.account[0] = debrid.error[0] = 0;
            debrid.connected = false; debrid.checked = 0; debrid.revision++;
            if (state_save_locked()) { code = 503; copy_text(err, cap, "TorBox was disconnected, but download state could not be saved."); }
        }
    } else if (debrid.retry_at > time(NULL)) { code = 429; copy_text(err, cap, "Wait before checking TorBox again."); }
    else if (!strcmp(action, "refresh") && !debrid.key[0]) { code = 409; copy_text(err, cap, "Connect TorBox first."); }
    else {
        copy_text(debrid.pending_key, sizeof debrid.pending_key, !strcmp(action, "connect") ? key : debrid.key);
        debrid.busy = debrid.check = true; debrid.error[0] = 0;
        debrid.retry_at = time(NULL) + 10;
        pthread_cond_broadcast(&orbit.changed);
    }
    pthread_mutex_unlock(&orbit.mutex); return code;
}
int debrid_prepare_locked(const Release *r, Job *job, char *err, size_t cap) {
    int host = r && !strcmp(r->source, "archive") ? 0 : r && !strcmp(r->source, "vikingfile") ? 1 : -1;
    if (!debrid.connected || !debrid.key[0]) { copy_text(err, cap, "Connect TorBox in App settings first."); return 409; }
    if (debrid.busy || time(NULL) >= debrid.checked + HOST_TTL) { copy_text(err, cap, "Refresh TorBox in App settings before starting this download."); return 409; }
    if (!r || host < 0 || !strcmp(r->id, "orbit-network-check") ||
        (!provider_supported(r) && !provider_browser_supported(r))) { copy_text(err, cap, "This option is not supported by TorBox."); return 409; }
    if (!debrid.hosts[host].available || (debrid.hosts[host].limit && r->size > debrid.hosts[host].limit)) {
        copy_text(err, cap, "TorBox currently cannot download this host or file size. Choose the original source."); return 409;
    }
    copy_text(job->delivery, sizeof job->delivery, "torbox");
    copy_text(job->debrid_account, sizeof job->debrid_account, debrid.account);
    job->debrid_id = job->debrid_file = -1; return 200;
}
static cJSON *find_item(cJSON *reply, Job *job) {
    cJSON *data = cJSON_GetObjectItemCaseSensitive(reply, "data");
    if (!cJSON_IsArray(data)) return cJSON_IsObject(data) ? data : NULL;
    cJSON *item;
    cJSON_ArrayForEach(item, data) {
        int64_t id;
        if (!identifier(item, "id", &id)) continue;
        if (job->debrid_id >= 0 ? id == job->debrid_id : !strcmp(json_text(item, "original_url"), job->release.url)) return item;
    }
    return NULL;
}
int debrid_resolve(Job *job, char *url, size_t url_cap, time_t *retry_at, char *err, size_t cap) {
    char key[KEY_CAP], path[2048]; int result = -1;
    pthread_mutex_lock(&orbit.mutex);
    bool account = debrid.connected && debrid.key[0] && !strcmp(job->debrid_account, debrid.account);
    bool checking = debrid.busy && debrid.key[0] && !strcmp(job->debrid_account, debrid.account);
    copy_text(key, sizeof key, account ? debrid.key : "");
    pthread_mutex_unlock(&orbit.mutex);
    if (!account && checking) { *retry_at = time(NULL) + 5; result = 0; goto end; }
    if (!account) { copy_text(err, cap, "Reconnect the original TorBox account before resuming this download."); goto end; }
    if (halted(job)) goto end;
    snprintf(path, sizeof path, job->debrid_id >= 0 ? "/webdl/mylist?id=%lld&bypass_cache=true" : "/webdl/mylist?limit=1000&bypass_cache=true", (long long)job->debrid_id);
    cJSON *list = call(key, path, NULL, true, job, retry_at, err, cap, NULL);
    if (!list) { if (*retry_at > time(NULL)) result = 0; goto end; }
    cJSON *item = find_item(list, job);
    if (job->debrid_id < 0 && item) {
        int64_t id;
        if (identifier(item, "id", &id)) {
            pthread_mutex_lock(&orbit.mutex); job->debrid_id = id; job->debrid_submitted = true;
            int saved = state_save_locked(); pthread_mutex_unlock(&orbit.mutex);
            if (saved) { cJSON_Delete(list); copy_text(err, cap, "Could not save the TorBox download identity."); goto end; }
        }
    }
    if (job->debrid_id < 0) {
        cJSON_Delete(list);
        pthread_mutex_lock(&orbit.mutex);
        bool uncertain = job->debrid_submitted;
        time_t next = orbit.debrid_next_create;
        bool saved = true;
        if (!uncertain && time(NULL) >= next) {
            job->debrid_submitted = true;
            orbit.debrid_next_create = time(NULL) + 61;
            saved = !state_save_locked();
            if (!saved) job->debrid_submitted = false;
        }
        pthread_mutex_unlock(&orbit.mutex);
        if (uncertain) { copy_text(err, cap, "The earlier TorBox submission could not be confirmed. Check your TorBox downloads before adding it again."); goto end; }
        if (!saved) { copy_text(err, cap, "Could not save the TorBox submission state."); goto end; }
        if (time(NULL) < next) { *retry_at = next; result = 0; goto end; }
        char *escaped = curl_easy_escape(NULL, job->release.url, 0);
        char form[8192];
        if (!escaped) {
            pthread_mutex_lock(&orbit.mutex); job->debrid_submitted = false; state_save_locked(); pthread_mutex_unlock(&orbit.mutex);
            copy_text(err, cap, "Could not prepare the TorBox request."); goto end;
        }
        snprintf(form, sizeof form, "link=%s&as_queued=false", escaped); curl_free(escaped);
        bool rejected = false;
        cJSON *created = call(key, "/webdl/createwebdownload", form, true, job, retry_at, err, cap, &rejected);
        int64_t id = -1;
        bool valid = created && identifier(cJSON_GetObjectItemCaseSensitive(created, "data"), "webdownload_id", &id);
        cJSON_Delete(created);
        if (!valid) {
            if (rejected) {
                pthread_mutex_lock(&orbit.mutex);
                job->debrid_submitted = false;
                if (*retry_at > orbit.debrid_next_create) orbit.debrid_next_create = *retry_at;
                int saved_rejection = state_save_locked();
                pthread_mutex_unlock(&orbit.mutex);
                if (saved_rejection) { copy_text(err, cap, "Could not save the TorBox retry state."); goto end; }
                if (*retry_at > time(NULL)) result = 0;
            }
            if (!err[0]) copy_text(err, cap, "TorBox did not return a download identity. Retry to check the account.");
            goto end;
        }
        pthread_mutex_lock(&orbit.mutex); job->debrid_id = id; int rc = state_save_locked(); pthread_mutex_unlock(&orbit.mutex);
        if (rc) { copy_text(err, cap, "Could not save the TorBox download identity."); goto end; }
        *retry_at = time(NULL) + 5; result = 0; goto end;
    }
    int64_t item_id = -1;
    if (!item || !identifier(item, "id", &item_id) || item_id != job->debrid_id ||
        strcmp(json_text(item, "original_url"), job->release.url)) {
        cJSON_Delete(list); copy_text(err, cap, "TorBox returned a different download identity."); goto end;
    }
    const char *state = json_text(item, "download_state");
    if (!strcmp(state, "error") || !strcmp(state, "failed") || *json_text(item, "error")) {
        cJSON_Delete(list); copy_text(err, cap, "TorBox could not prepare this file. Check its status in your TorBox account."); goto end;
    }
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "download_present")) ||
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "download_finished"))) {
        cJSON_Delete(list); *retry_at = time(NULL) + 5; result = 0; goto end;
    }
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(item, "files");
    const cJSON *file = cJSON_GetArrayItem(files, 0); int64_t file_id = -1, size = 0;
    const char *name = json_text(file, "short_name");
    if (!*name) name = json_text(file, "name");
    bool match = cJSON_IsArray(files) && cJSON_GetArraySize(files) == 1 &&
        identifier(file, "id", &file_id) && identifier(file, "size", &size) && size == job->release.size &&
        !strcmp(name, job->filename) && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(file, "infected")) &&
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(file, "zipped")) &&
        (job->debrid_file < 0 || job->debrid_file == file_id);
    cJSON_Delete(list);
    if (!match) { copy_text(err, cap, "TorBox's file does not match the selected single-file download. Nothing was transferred."); goto end; }
    pthread_mutex_lock(&orbit.mutex); job->debrid_file = file_id; int saved = state_save_locked(); pthread_mutex_unlock(&orbit.mutex);
    if (saved) { copy_text(err, cap, "Could not save the TorBox file identity."); goto end; }
    char *escaped = curl_easy_escape(NULL, key, 0);
    if (!escaped) goto end;
    /* Appending the filename can produce literal spaces in TorBox's URL.
     * File identity and the local filename were already checked above; request
     * the canonical address without this optional query value. Keep URL and
     * redirect validation strict, including rejecting unescaped whitespace. */
    snprintf(path, sizeof path, "/webdl/requestdl?token=%s&web_id=%lld&file_id=%lld&redirect=false&zip_link=false&append_name=false", escaped, (long long)job->debrid_id, (long long)file_id); curl_free(escaped);
    cJSON *download = call(key, path, NULL, false, job, retry_at, err, cap, NULL);
    OPENSSL_cleanse(path, sizeof path);
    const char *address = json_text(download, "data");
    if (download && strlen(address) < url_cap && debrid_transfer_url_allowed(address)) {
        copy_text(url, url_cap, address); result = 1;
    } else if (download) copy_text(err, cap, "TorBox returned an unsupported download address.");
    else if (*retry_at > time(NULL)) result = 0;
    cJSON *secret = cJSON_GetObjectItemCaseSensitive(download, "data");
    if (cJSON_IsString(secret)) OPENSSL_cleanse(secret->valuestring, strlen(secret->valuestring));
    cJSON_Delete(download);
end:
    OPENSSL_cleanse(key, sizeof key); OPENSSL_cleanse(path, sizeof path);
    if (result == 0 && *retry_at < time(NULL) + 5) *retry_at = time(NULL) + 5;
    return result;
}
