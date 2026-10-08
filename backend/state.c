#include "network.h"
#include "orbit.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
Orbit orbit = {.mutex = PTHREAD_MUTEX_INITIALIZER,
               .changed = PTHREAD_COND_INITIALIZER,
               .port = ORBIT_HTTP_PORT,
               .state_fd = -1};
void copy_text(char *to, size_t cap, const char *s) {
    if (cap)
        snprintf(to, cap, "%s", s ? s : "");
}
const char *json_text(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}
int64_t json_int(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (int64_t)v->valuedouble : 0;
}
static uint64_t json_identity(const cJSON *o, const char *key) {
    const char *s = json_text(o, key);
    return *s ? strtoumax(s, NULL, 10) : (uint64_t)json_int(o, key);
}
void random_hex(char *out, size_t n) {
    unsigned char b[32];
    if (n > 32 || RAND_bytes(b, (int)n) != 1) {
        fputs("Secure randomness unavailable\n", stderr);
        exit(1);
    }
    for (size_t i = 0; i < n; i++)
        snprintf(out + 2 * i, 3, "%02x", b[i]);
}
Release *release_find(const char *id) {
    for (size_t i = 0; i < orbit.release_count; i++)
        if (!strcmp(orbit.releases[i].id, id))
            return &orbit.releases[i];
    return NULL;
}
Job *job_find(const char *id) {
    for (size_t i = 0; i < orbit.job_count; i++)
        if (!strcmp(orbit.jobs[i].id, id))
            return &orbit.jobs[i];
    return NULL;
}
static cJSON *job_json(const Job *j, bool private_fields) {
    cJSON *o = cJSON_CreateObject();
#define S(k, f) cJSON_AddStringToObject(o, k, j->f)
    S("id", id);
    S("releaseId", release_id);
    S("storageId", storage_id);
    S("filename", filename);
    S("status", status);
    S("error", error);
    S("verification", verification);
    cJSON_AddStringToObject(o, "delivery", j->delivery[0] ? j->delivery : "direct");
    S("deliveryPhase", delivery_phase);
    int64_t received = j->received;
    if (private_fields && j->segmented) {
        received = 0;
        for (unsigned i = 0; i < j->range_count; i++) received += j->range_received[i];
    }
    cJSON_AddNumberToObject(o, "received", (double)received);
    cJSON_AddNumberToObject(o, "total", (double)j->total);
    cJSON_AddNumberToObject(o, "speed", j->speed);
    cJSON_AddNumberToObject(o, "retryAt", (double)j->retry_at);
    cJSON_AddNumberToObject(o, "order", j->order);
    cJSON_AddStringToObject(o, "title", j->release.title);
    cJSON_AddStringToObject(o, "titleId", j->release.title_id);
    cJSON_AddStringToObject(o, "format", j->release.format);
    cJSON_AddStringToObject(o, "gameId", j->release.game_id);
    char path[700];
    snprintf(path, sizeof path, "%s/homebrew/%s", j->root, j->filename);
    cJSON_AddStringToObject(o, "path", path);
    if (private_fields) {
        if (!strcmp(j->delivery, "torbox")) {
            S("debridAccount", debrid_account);
            cJSON_AddNumberToObject(o, "debridId", (double)j->debrid_id);
            cJSON_AddNumberToObject(o, "debridFile", (double)j->debrid_file);
            cJSON_AddBoolToObject(o, "debridSubmitted", j->debrid_submitted);
        }
        if (j->segmented) {
            cJSON *ranges = cJSON_AddArrayToObject(o, "rangeReceived");
            for (unsigned i = 0; i < j->range_count; i++)
                cJSON_AddItemToArray(ranges, cJSON_CreateNumber((double)j->range_received[i]));
        }
        cJSON_AddItemToObject(o, "release", release_json(&j->release));
        S("root", root);
        S("etag", etag);
        S("modified", modified);
        char device[32], inode[32];
        snprintf(device, sizeof device, "%ju", (uintmax_t)j->device);
        snprintf(inode, sizeof inode, "%ju", (uintmax_t)j->inode);
        cJSON_AddStringToObject(o, "device", device);
        cJSON_AddStringToObject(o, "inode", inode);
        cJSON_AddNumberToObject(o, "attempts", j->attempts);
    }
#undef S
    return o;
}
cJSON *jobs_json_locked(void) {
    cJSON *a = cJSON_CreateArray();
    /* Sort references only. The transfer worker retains a pointer to its fixed slot. */
    const Job *ordered[ORBIT_MAX_JOBS];
    size_t count = 0;
    for (size_t i = 0; i < orbit.job_count; i++) {
        const Job *j = &orbit.jobs[i];
        if (!j->id[0])
            continue;
        size_t p = count++;
        while (p && ordered[p - 1]->order > j->order) {
            ordered[p] = ordered[p - 1];
            p--;
        }
        ordered[p] = j;
    }
    for (size_t i = 0; i < count; i++)
        cJSON_AddItemToArray(a, job_json(ordered[i], false));
    return a;
}
int state_save_locked(void) {
    cJSON *o = cJSON_CreateObject(), *a = cJSON_AddArrayToObject(o, "jobs"),
          *t = cJSON_AddArrayToObject(o, "tokens");
    /* Direct queues retain the 0.7.0 durable layout. TorBox queues must
     * reject older builds, which cannot retain the delivery/account identity. */
    unsigned schema = 4;
    for (size_t i = 0; i < orbit.job_count; i++)
        if (orbit.jobs[i].id[0] && !strcmp(orbit.jobs[i].delivery, "torbox")) schema = 5;
    cJSON_AddNumberToObject(o, "schema", schema);
    cJSON_AddStringToObject(o, "preferredStorage", orbit.preferred_storage);
    cJSON_AddNumberToObject(o, "debridNextCreate", (double)orbit.debrid_next_create);
    cJSON_AddItemToObject(o, "favorites", favorites_json_locked());
    cJSON *sources = cJSON_AddObjectToObject(o, "sources");
    cJSON_AddNumberToObject(sources, "enabled", orbit.sources.enabled);
    cJSON_AddNumberToObject(sources, "noticeVersion", orbit.sources.notice_version);
    cJSON_AddNumberToObject(sources, "acknowledgedAt", (double)orbit.sources.acknowledged_at);
    for (size_t i = 0; i < orbit.job_count; i++)
        if (orbit.jobs[i].id[0])
            cJSON_AddItemToArray(a, job_json(&orbit.jobs[i], true));
    for (size_t i = 0; i < orbit.token_count; i++)
        cJSON_AddItemToArray(t, cJSON_CreateString(orbit.tokens[i]));
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s)
        return -1;
    int fd = openat(orbit.state_fd, "state.tmp", O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600),
        rc = -1;
    if (fd >= 0) {
        size_t n = strlen(s), p = 0;
        while (p < n) {
            ssize_t w = write(fd, s + p, n - p);
            if (w <= 0)
                break;
            p += (size_t)w;
        }
        if (p == n && fsync(fd) == 0)
            rc = 0;
        if (close(fd) != 0)
            rc = -1;
    }
    free(s);
    if (rc == 0)
        rc = renameat(orbit.state_fd, "state.tmp", orbit.state_fd, "state.json");
    if (rc == 0)
        rc = fsync(orbit.state_fd);
    orbit.state_failed = rc != 0;
    return rc;
}
static int backup_legacy_state(const char *data, size_t length) {
    char nonce[17], name[64];
    random_hex(nonce, 8);
    snprintf(name, sizeof name, "state-before-buffered-%s.json", nonce);
    int fd = openat(orbit.state_fd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    size_t written = 0;
    while (written < length) {
        ssize_t n = write(fd, data + written, length - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        written += (size_t)n;
    }
    int rc = written == length ? fsync(fd) : -1;
    if (close(fd)) rc = -1;
    if (!rc) rc = fsync(orbit.state_fd);
    if (rc) unlinkat(orbit.state_fd, name, 0);
    else orbit.queue_backup_created = true;
    return rc;
}
int state_load(void) {
    int fd = openat(orbit.state_fd, "state.json", O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return 0;
    struct stat st;
    if (fstat(fd, &st) || st.st_size < 1 || st.st_size > 1024 * 1024) {
        close(fd);
        return -1;
    }
    size_t n = (size_t)st.st_size;
    char *s = calloc(n + 1, 1);
    if (!s) {
        close(fd);
        return -1;
    }
    size_t p = 0;
    while (p < n) {
        ssize_t got = read(fd, s + p, n - p);
        if (got <= 0)
            break;
        p += (size_t)got;
    }
    close(fd);
    cJSON *o = p == n ? cJSON_Parse(s) : NULL;
    const cJSON *schema_value = cJSON_GetObjectItemCaseSensitive(o, "schema");
    if (!cJSON_IsNumber(schema_value) ||
        (schema_value->valuedouble != 1 && schema_value->valuedouble != 2 && schema_value->valuedouble != 3 &&
         schema_value->valuedouble != 4 && schema_value->valuedouble != 5)) {
        free(s);
        cJSON_Delete(o);
        return -1;
    }
    int schema = (int)schema_value->valuedouble;
    /* Save the exact old queue before any buffered-schema write. No overwrite and no
     * migration when backup fails; filenames/credentials stay on the console. */
    if (schema < 4 && backup_legacy_state(s, n)) {
        free(s);
        cJSON_Delete(o);
        return -1;
    }
    free(s);
    const cJSON *x;
    const cJSON *next_create = cJSON_GetObjectItemCaseSensitive(o, "debridNextCreate");
    if (next_create) {
        if (!cJSON_IsNumber(next_create) || !(next_create->valuedouble >= 0 && next_create->valuedouble <= 4102444800.0) ||
            next_create->valuedouble != (double)(time_t)next_create->valuedouble) { cJSON_Delete(o); return -1; }
        orbit.debrid_next_create = (time_t)next_create->valuedouble;
    }
    copy_text(orbit.preferred_storage, sizeof orbit.preferred_storage,
              json_text(o, "preferredStorage"));
    const cJSON *sources = cJSON_GetObjectItemCaseSensitive(o, "sources");
    if (json_int(sources, "noticeVersion") == ORBIT_SOURCE_NOTICE_VERSION &&
        json_int(sources, "acknowledgedAt") > 0 && json_int(sources, "enabled") >= 0 &&
        json_int(sources, "enabled") <= (ORBIT_SOURCE_ARCHIVE | ORBIT_SOURCE_VIKINGFILE)) {
        orbit.sources.enabled = (unsigned)json_int(sources, "enabled");
        orbit.sources.notice_version = ORBIT_SOURCE_NOTICE_VERSION;
        orbit.sources.acknowledged_at = (time_t)json_int(sources, "acknowledgedAt");
    }
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(o, "tokens")) {
        if (cJSON_IsString(x) && strlen(x->valuestring) == 64 && orbit.token_count < 16)
            copy_text(orbit.tokens[orbit.token_count++], 65, x->valuestring);
    }
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(o, "favorites")) {
        if (!cJSON_IsString(x) || !*x->valuestring || strlen(x->valuestring) >= 64 ||
            orbit.favorite_count >= ORBIT_MAX_RELEASES)
            continue;
        bool duplicate = false;
        for (size_t i = 0; i < orbit.favorite_count; i++)
            if (!strcmp(orbit.favorites[i], x->valuestring))
                duplicate = true;
        if (!duplicate)
            copy_text(orbit.favorites[orbit.favorite_count++], 64, x->valuestring);
    }
    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(o, "jobs")) {
        Release snapshot, *r = release_find(json_text(x, "releaseId"));
        const cJSON *saved = cJSON_GetObjectItemCaseSensitive(x, "release");
        if (saved) {
            /* A bad saved identity must never silently switch to the current file. */
            if (release_parse(saved, &snapshot, true) ||
                strcmp(snapshot.id, json_text(x, "releaseId"))) {
                cJSON_Delete(o);
                return -1;
            }
            r = &snapshot;
        }
        if (!r || orbit.job_count >= ORBIT_MAX_JOBS)
            continue;
        Job *j = &orbit.jobs[orbit.job_count++];
        j->release = *r;
#define R(k, f) copy_text(j->f, sizeof j->f, json_text(x, k))
        R("id", id);
        R("releaseId", release_id);
        R("storageId", storage_id);
        R("root", root);
        R("status", status);
        R("etag", etag);
        R("modified", modified);
        R("error", error);
        R("verification", verification);
#undef R
        const char *delivery = json_text(x, "delivery");
        if (*delivery && strcmp(delivery, "direct") && strcmp(delivery, "torbox")) { cJSON_Delete(o); return -1; }
        if (!strcmp(delivery, "torbox")) {
            const cJSON *web = cJSON_GetObjectItemCaseSensitive(x, "debridId"),
                        *file = cJSON_GetObjectItemCaseSensitive(x, "debridFile");
            const char *account = json_text(x, "debridAccount");
            if ((schema != 3 && schema != 5) || strlen(account) != 64 || strspn(account, "0123456789abcdef") != 64 ||
                !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(x, "debridSubmitted")) ||
                !cJSON_IsNumber(web) || !cJSON_IsNumber(file) ||
                !(web->valuedouble >= -1 && web->valuedouble <= 9007199254740991.0) ||
                !(file->valuedouble >= -1 && file->valuedouble <= 9007199254740991.0) ||
                web->valuedouble != (double)(int64_t)web->valuedouble ||
                file->valuedouble != (double)(int64_t)file->valuedouble) { cJSON_Delete(o); return -1; }
            copy_text(j->delivery, sizeof j->delivery, "torbox");
            copy_text(j->debrid_account, sizeof j->debrid_account, json_text(x, "debridAccount"));
            j->debrid_id = (int64_t)web->valuedouble; j->debrid_file = (int64_t)file->valuedouble;
            j->debrid_submitted = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(x, "debridSubmitted"));
        }
        copy_text(j->filename, sizeof j->filename, r->filename);
        j->device = (dev_t)json_identity(x, "device");
        j->inode = (ino_t)json_identity(x, "inode");
        j->received = json_int(x, "received");
        const cJSON *ranges = cJSON_GetObjectItemCaseSensitive(x, "rangeReceived");
        if (ranges) {
            int count = cJSON_GetArraySize(ranges);
            if (!cJSON_IsArray(ranges) ||
                !(((schema == 2 || schema == 3) && count == 2) || ((schema == 4 || schema == 5) && (count == 1 || count == 2 || count == 4))) ||
                (count > 1 && (!j->etag[0] || !strncmp(j->etag, "W/", 2)))) {
                cJSON_Delete(o);
                return -1;
            }
            j->range_count = (unsigned)count;
            j->received = 0;
            for (unsigned i = 0; i < j->range_count; i++) {
                const cJSON *v = cJSON_GetArrayItem(ranges, i);
                int64_t length = range_boundary(r->size, j->range_count, i + 1) -
                                 range_boundary(r->size, j->range_count, i);
                if (!cJSON_IsNumber(v) || !(v->valuedouble >= 0 && v->valuedouble <= length) ||
                    v->valuedouble != (double)(int64_t)v->valuedouble) {
                    cJSON_Delete(o);
                    return -1;
                }
                j->range_received[i] = (int64_t)v->valuedouble;
                j->received += j->range_received[i];
            }
            j->segmented = true;
        }
        j->total = r->size;
        j->attempts = (unsigned)json_int(x, "attempts");
        int64_t order = json_int(x, "order");
        j->order = order > 0 && order < 2147483647 ? (unsigned)order : (unsigned)orbit.job_count;
        j->retry_at = (time_t)json_int(x, "retryAt");
        if (!strcmp(j->status, "downloading") || !strcmp(j->status, "verifying")) {
            copy_text(j->status, sizeof j->status, "paused");
            copy_text(j->error, sizeof j->error, "Interrupted. Resume to continue.");
        }
        if (!source_enabled_locked(r) &&
            (!strcmp(j->status, "queued") || !strcmp(j->status, "retrying") ||
             !strcmp(j->status, "paused"))) {
            copy_text(j->status, sizeof j->status, "paused");
            copy_text(j->error, sizeof j->error,
                      "Source disabled. Enable it in Sources before resuming.");
        }
    }
    cJSON_Delete(o);
    return 0;
}
