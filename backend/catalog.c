/* A fixed metadata feed updates the catalogue independently of the application ELF. */
#include "catalog.h"
#include "catalog_crypto.h"
#include "ca.h"
#include "orbit.h"
#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Older clients keep v3, which excludes formats their parser cannot accept. */
#define CATALOG_URL "https://raw.githubusercontent.com/saawant12/orbit-store-ps5/main/catalogue-v4.enc"
#define CATALOG_LIMIT (CATALOG_SEAL_LIMIT + CATALOG_SEAL_OVERHEAD)
#define CHECK_INTERVAL (6 * 60 * 60)
static struct {
    pthread_cond_t changed;
    pthread_t thread;
    bool started, stop, pending, busy, cached;
    time_t checked, updated, check_after, next_check;
    char error[256];
} refresh = {.changed = PTHREAD_COND_INITIALIZER}; /* Protected by orbit.mutex. */

static bool text_field(const cJSON *o, const char *key, char *out, size_t cap, bool required) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!required && (!v || cJSON_IsNull(v))) {
        out[0] = 0;
        return true;
    }
    if (!cJSON_IsString(v) || strlen(v->valuestring) >= cap || (required && !*v->valuestring))
        return false;
    for (const unsigned char *p = (const void *)v->valuestring; *p; p++)
        if (*p < 32 || *p == 127)
            return false;
    copy_text(out, cap, v->valuestring);
    return true;
}
static bool identifier(const char *id) {
    if (!*id)
        return false;
    for (; *id; id++)
        if (!isalnum((unsigned char)*id) && *id != '-' && *id != '_' && *id != '.')
            return false;
    return true;
}
int release_parse(const cJSON *x, Release *r, bool diagnostic) {
    memset(r, 0, sizeof *r);
    if (!cJSON_IsObject(x))
        return -1;
#define FIELD(k, f, required) if (!text_field(x, k, r->f, sizeof r->f, required)) return -1
    FIELD("id", id, true);
    FIELD("title", title, true);
    FIELD("titleId", title_id, false);
    FIELD("gameId", game_id, false);
    FIELD("filename", filename, true);
    FIELD("url", url, true);
    FIELD("sha256", sha256, false);
    FIELD("sourceId", source, false);
    FIELD("format", format, false);
    FIELD("browserUrl", browser_url, false);
#undef FIELD
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(x, "sizeBytes");
    if (!identifier(r->id) || !cJSON_IsNumber(size) || !isfinite(size->valuedouble) ||
        size->valuedouble < 1 || size->valuedouble > 9007199254740991.0 ||
        floor(size->valuedouble) != size->valuedouble || strchr(r->filename, '/') ||
        strchr(r->filename, '\\') || strstr(r->filename, ".."))
        return -1;
    r->size = (int64_t)size->valuedouble;
    if (*r->sha256) {
        if (strlen(r->sha256) != 64)
            return -1;
        for (const char *p = r->sha256; *p; p++)
            if (!isxdigit((unsigned char)*p))
                return -1;
    }
    if (!strcmp(r->id, "orbit-network-check"))
        return diagnostic && r->size == 1084 &&
            !strcmp(r->filename, "Orbit-connection-test.txt") &&
            !strcmp(r->url, "https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.19/LICENSE") &&
            !strcmp(r->sha256, "a36dda207c36db5818729c54e7ad4e8b0c6fba847491ba64f372c1a2037b6d5c") ? 0 : -1;
    if (!identifier(r->game_id) || !*r->title_id ||
        (strcmp(r->format, "FFPFSC") && strcmp(r->format, "exFAT") && strcmp(r->format, "FPKG")))
        return -1;
#ifdef ORBIT_TEST
    if (provider_supported(r)) return 0;
#endif
    return (provider_url_supported(r) || provider_browser_supported(r)) &&
        (!r->browser_url[0] || provider_browser_supported(r)) ? 0 : -1;
}
cJSON *release_json(const Release *r) {
    cJSON *o = cJSON_CreateObject();
#define FIELD(k, f) cJSON_AddStringToObject(o, k, r->f)
    FIELD("id", id); FIELD("title", title); FIELD("titleId", title_id);
    FIELD("gameId", game_id); FIELD("filename", filename); FIELD("url", url);
    FIELD("sha256", sha256); FIELD("sourceId", source); FIELD("format", format);
    if (r->browser_url[0]) { FIELD("browserUrl", browser_url); }
#undef FIELD
    cJSON_AddNumberToObject(o, "sizeBytes", (double)r->size);
    return o;
}
static Release *parse_entries(cJSON *entries, size_t *count) {
    if (!cJSON_IsArray(entries) || cJSON_GetArraySize(entries) < 1 ||
        cJSON_GetArraySize(entries) > ORBIT_MAX_RELEASES)
        return NULL;
    size_t n = (size_t)cJSON_GetArraySize(entries);
    Release *releases = calloc(n + 1, sizeof *releases);
    if (!releases)
        return NULL;
    cJSON *entry;
    size_t i = 0;
    cJSON_ArrayForEach(entry, entries) {
        char cover[2048], hero[2048];
        if (release_parse(entry, &releases[i], false) ||
            !text_field(entry, "cover", cover, sizeof cover, true) || strncmp(cover, "https://", 8) ||
            !text_field(entry, "hero", hero, sizeof hero, true) || strncmp(hero, "https://", 8))
            goto invalid;
        const char *fallbacks[] = {"coverFallback", "heroFallback"};
        for (size_t k = 0; k < sizeof fallbacks / sizeof fallbacks[0]; k++) {
            char url[2048];
            if (!text_field(entry, fallbacks[k], url, sizeof url, false) ||
                (*url && strncmp(url, "https://", 8)))
                goto invalid;
        }
        const char *optional[] = {"genre", "tagline", "description", "publisher", "releaseDate", "version", "addedAt"};
        for (size_t k = 0; k < sizeof optional / sizeof optional[0]; k++) {
            const cJSON *value = cJSON_GetObjectItemCaseSensitive(entry, optional[k]);
            if (value && !cJSON_IsNull(value) &&
                (!cJSON_IsString(value) || strlen(value->valuestring) > 16384))
                goto invalid;
        }
        char provider[128], layout[16];
        if (!text_field(entry, "provider", provider, sizeof provider, true) ||
            !text_field(entry, "artworkLayout", layout, sizeof layout, true) ||
            (strcmp(layout, "wide") && strcmp(layout, "ambient")))
            goto invalid;
        for (size_t j = 0; j < i; j++)
            if (!strcmp(releases[j].id, releases[i].id))
                goto invalid;
        i++;
    }
    Release *test = &releases[n];
    copy_text(test->id, sizeof test->id, "orbit-network-check");
    copy_text(test->title, sizeof test->title, "Orbit connection test");
    copy_text(test->filename, sizeof test->filename, "Orbit-connection-test.txt");
    copy_text(test->url, sizeof test->url, "https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.19/LICENSE");
    copy_text(test->sha256, sizeof test->sha256, "a36dda207c36db5818729c54e7ad4e8b0c6fba847491ba64f372c1a2037b6d5c");
    test->size = 1084;
    *count = n + 1;
    return releases;
invalid:
    free(releases);
    return NULL;
}
int catalog_init(void) {
    unsigned char *plain = NULL;
    size_t length = 0;
    if (catalog_open(catalog_data, sizeof catalog_data, CATALOG_PURPOSE_FEED, &plain, &length)) return -1;
    const char *end = NULL;
    cJSON *feed = cJSON_ParseWithLengthOpts((char *)plain, length + 1, &end, true);
    bool valid = end == (char *)plain + length && json_int(feed, "schemaVersion") == 1 &&
                 json_int(feed, "revision") == catalog_revision;
    OPENSSL_cleanse(plain, length);
    free(plain);
    cJSON *entries = cJSON_DetachItemFromObjectCaseSensitive(feed, "releases");
    cJSON_Delete(feed);
    size_t count;
    Release *releases = valid ? parse_entries(entries, &count) : NULL;
    if (!releases) { cJSON_Delete(entries); return -1; }
    orbit.catalog = entries;
    orbit.catalog_revision = catalog_revision;
    orbit.release_count = count;
    memcpy(orbit.releases, releases, count * sizeof *releases);
    free(releases);
    return 0;
}
static int save_cache(const char *data, size_t length) {
    int fd = openat(orbit.state_fd, "catalogue.tmp", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0 && errno == EEXIST) {
        /* Remove only a leftover temporary regular file from our own interrupted write. */
        struct stat st;
        if (fstatat(orbit.state_fd, "catalogue.tmp", &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) ||
            unlinkat(orbit.state_fd, "catalogue.tmp", 0))
            return -1;
        fd = openat(orbit.state_fd, "catalogue.tmp", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    }
    if (fd < 0)
        return -1;
    size_t position = 0;
    while (position < length) {
        ssize_t n = write(fd, data + position, length - position);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        position += (size_t)n;
    }
    int rc = position == length ? fsync(fd) : -1;
    if (close(fd)) rc = -1;
    if (!rc) rc = renameat(orbit.state_fd, "catalogue.tmp", orbit.state_fd, "catalogue.json");
    if (rc) unlinkat(orbit.state_fd, "catalogue.tmp", 0);
    else fsync(orbit.state_fd);
    return rc;
}
static int apply(const char *data, size_t length, bool cache, char *error, size_t cap) {
    unsigned char *plain = NULL, *migrated = NULL;
    size_t plain_length = 0, migrated_length = 0;
    bool legacy = false;
    if (catalog_open(data, length, CATALOG_PURPOSE_FEED, &plain, &plain_length)) {
        /* One-way migration of a local old cache only. Network plaintext and
         * broken/unknown encrypted envelopes never get a downgrade fallback. */
        size_t first = 0;
        while (first < length && isspace((unsigned char)data[first])) first++;
        if (!cache || first == length || data[first] != '{' || length > CATALOG_SEAL_LIMIT) {
            copy_text(error, cap, "The catalogue update is invalid. Your current catalogue is unchanged.");
            return -1;
        }
        plain = malloc(length + 1);
        if (!plain) return -1;
        memcpy(plain, data, length);
        plain[length] = 0;
        plain_length = length;
        legacy = true;
    }
    const char *end = NULL;
    cJSON *feed = cJSON_ParseWithLengthOpts((char *)plain, plain_length + 1, &end, true);
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(feed, "schemaVersion"),
                *rev = cJSON_GetObjectItemCaseSensitive(feed, "revision");
    cJSON *entries = cJSON_GetObjectItemCaseSensitive(feed, "releases");
    size_t count = 0;
    Release *releases = NULL;
    if (!cJSON_IsObject(feed) || end != (char *)plain + plain_length || !cJSON_IsNumber(schema) || schema->valuedouble != 1 ||
        !cJSON_IsNumber(rev) || !isfinite(rev->valuedouble) || rev->valuedouble < 1 ||
        rev->valuedouble > 2147483647 || floor(rev->valuedouble) != rev->valuedouble ||
        !(releases = parse_entries(entries, &count))) {
        copy_text(error, cap, "The catalogue update is invalid. Your current catalogue is unchanged.");
        cJSON_Delete(feed);
        OPENSSL_cleanse(plain, plain_length);
        free(plain);
        return -1;
    }
    if (legacy && catalog_seal(plain, plain_length, CATALOG_PURPOSE_FEED, &migrated, &migrated_length)) {
        free(releases);
        cJSON_Delete(feed);
        OPENSSL_cleanse(plain, plain_length);
        free(plain);
        return -1;
    }
    OPENSSL_cleanse(plain, plain_length);
    free(plain);
    /* Encrypt even an older valid local cache: it may be kept for recovery,
     * but must not leave a plaintext catalogue beside the new app. */
    if (legacy && save_cache((char *)migrated, migrated_length)) {
        copy_text(error, cap, "Could not encrypt the saved catalogue.");
        free(migrated);
        free(releases);
        cJSON_Delete(feed);
        return -1;
    }
    unsigned revision = (unsigned)rev->valuedouble;
    int result = -1;
    pthread_mutex_lock(&orbit.mutex);
    if (revision < orbit.catalog_revision) {
        copy_text(error, cap, "An older catalogue was received. Keeping your newer catalogue.");
    } else if (revision == orbit.catalog_revision) {
        if (!cJSON_Compare(entries, orbit.catalog, true))
            copy_text(error, cap, "The catalogue changed without a new revision. Keeping the saved copy.");
        else result = 0;
    } else if (orbit.stop || refresh.stop) {
        copy_text(error, cap, "Orbit is stopping. The catalogue was not changed.");
    } else if (!cache && (state_save_locked() || save_cache(data, length))) {
        /* Persist each job's original release before a catalogue can remove/change it. */
        copy_text(error, cap, "Could not save the catalogue. Your current catalogue is unchanged.");
    } else {
        cJSON_DetachItemViaPointer(feed, entries);
        cJSON_Delete(orbit.catalog);
        orbit.catalog = entries;
        memcpy(orbit.releases, releases, count * sizeof *releases);
        orbit.release_count = count;
        orbit.catalog_revision = revision;
        refresh.cached = true;
        refresh.updated = time(NULL);
        result = 0;
    }
    pthread_mutex_unlock(&orbit.mutex);
    free(releases);
    free(migrated);
    cJSON_Delete(feed);
    return result;
}
void catalog_load_cache(void) {
    int fd = openat(orbit.state_fd, "catalogue.json", O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno != ENOENT) copy_text(refresh.error, sizeof refresh.error, "Saved catalogue could not be read. Using the bundled catalogue.");
        return;
    }
    struct stat st;
    char *data = NULL;
    size_t n = 0;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= CATALOG_LIMIT) {
        data = calloc((size_t)st.st_size + 1, 1);
        if (data) while (n < (size_t)st.st_size) {
            ssize_t got = read(fd, data + n, (size_t)st.st_size - n);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) break;
            n += (size_t)got;
        }
    }
    close(fd);
    if (!data || n != (size_t)st.st_size || apply(data, n, true, refresh.error, sizeof refresh.error))
        copy_text(refresh.error, sizeof refresh.error, "Saved catalogue could not be loaded. Using the bundled catalogue.");
    free(data);
}
static const char *feed_url(void) {
#ifdef ORBIT_TEST
    const char *url = getenv("ORBIT_TEST_CATALOG_URL");
    return url && !strncmp(url, "http://127.0.0.1:", 17) ? url : NULL;
#else
    return orbit.desktop ? NULL : CATALOG_URL;
#endif
}
static bool stopping(void) {
    pthread_mutex_lock(&orbit.mutex);
    bool stopped = refresh.stop || orbit.stop;
    pthread_mutex_unlock(&orbit.mutex);
    return stopped;
}
typedef struct { char *data; size_t size; time_t retry; } Buffer;
static size_t collect(char *data, size_t a, size_t b, void *context) {
    Buffer *buf = context;
    if (a && b > SIZE_MAX / a) return 0;
    size_t n = a * b;
    if (n > CATALOG_LIMIT - buf->size || stopping()) return 0;
    char *next = realloc(buf->data, buf->size + n + 1);
    if (!next) return 0;
    buf->data = next;
    memcpy(next + buf->size, data, n);
    buf->size += n;
    next[buf->size] = 0;
    return n;
}
static size_t headers(char *data, size_t a, size_t b, void *context) {
    Buffer *buf = context;
    size_t n = a * b;
    if (n > 12 && !strncasecmp(data, "Retry-After:", 12)) {
        char value[128];
        size_t len = n - 12 < sizeof value - 1 ? n - 12 : sizeof value - 1;
        memcpy(value, data + 12, len); value[len] = 0;
        char *end;
        long seconds = strtol(value, &end, 10);
        if (end == value) {
            time_t date = curl_getdate(value, NULL);
            seconds = date > time(NULL) ? date - time(NULL) : 0;
        }
        if (seconds > 7 * 86400) seconds = 7 * 86400;
        if (seconds > buf->retry) buf->retry = seconds;
    }
    return n;
}
static int progress(void *ctx, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)ctx; (void)a; (void)b; (void)c; (void)d;
    return stopping();
}
static void check(void) {
    char error[256] = {0};
    Buffer buf = {0};
    CURL *curl = curl_easy_init();
    long status = 0;
    CURLcode code = CURLE_FAILED_INIT;
    if (curl) {
        struct curl_blob ca = {(void *)orbit_ca, sizeof orbit_ca - 1, CURL_BLOB_COPY};
        curl_easy_setopt(curl, CURLOPT_URL, feed_url());
        curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
#ifdef ORBIT_TEST
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http");
#else
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#endif
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Orbit-Store/" ORBIT_VERSION);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headers);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &buf);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        code = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(curl);
    }
    if (code == CURLE_OK && status == 200 && buf.data)
        apply(buf.data, buf.size, false, error, sizeof error);
    else
        snprintf(error, sizeof error, "Catalogue refresh failed (HTTP %ld). Using your current catalogue.", status);
    free(buf.data);
    pthread_mutex_lock(&orbit.mutex);
    refresh.checked = time(NULL);
    if (buf.retry > 60) refresh.check_after = refresh.checked + buf.retry;
    refresh.next_check = refresh.checked + CHECK_INTERVAL;
    if (refresh.next_check < refresh.check_after) refresh.next_check = refresh.check_after;
    copy_text(refresh.error, sizeof refresh.error, error);
    refresh.busy = false;
    pthread_mutex_unlock(&orbit.mutex);
}
static void *worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&orbit.mutex);
    while (!refresh.stop) {
        time_t now = time(NULL);
        if (refresh.pending || now >= refresh.next_check) {
            refresh.pending = false;
            refresh.busy = true;
            refresh.check_after = now + 60;
            pthread_mutex_unlock(&orbit.mutex);
            check();
            pthread_mutex_lock(&orbit.mutex);
        } else {
            struct timespec deadline = {.tv_sec = refresh.next_check};
            pthread_cond_timedwait(&refresh.changed, &orbit.mutex, &deadline);
        }
    }
    pthread_mutex_unlock(&orbit.mutex);
    return NULL;
}
int catalog_start(void) {
    if (!feed_url()) return 0;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr)) return -1;
    int rc = pthread_attr_setstacksize(&attr, ORBIT_THREAD_STACK);
    pthread_mutex_lock(&orbit.mutex);
    if (!rc) rc = pthread_create(&refresh.thread, &attr, worker, NULL);
    refresh.started = rc == 0;
    pthread_mutex_unlock(&orbit.mutex);
    pthread_attr_destroy(&attr);
    return rc ? -1 : 0;
}
void catalog_stop(void) {
    pthread_mutex_lock(&orbit.mutex);
    bool started = refresh.started;
    refresh.stop = true;
    pthread_cond_signal(&refresh.changed);
    pthread_mutex_unlock(&orbit.mutex);
    if (started) pthread_join(refresh.thread, NULL);
}
cJSON *catalog_status(void) {
    cJSON *o = cJSON_CreateObject();
    pthread_mutex_lock(&orbit.mutex);
    size_t games = 0;
    for (size_t i = 0; i < orbit.release_count; i++) {
        if (!*orbit.releases[i].game_id) continue;
        bool seen = false;
        for (size_t j = 0; j < i; j++)
            if (!strcmp(orbit.releases[i].game_id, orbit.releases[j].game_id)) { seen = true; break; }
        if (!seen) games++;
    }
    cJSON_AddBoolToObject(o, "available", refresh.started);
    cJSON_AddBoolToObject(o, "busy", refresh.busy || refresh.pending);
    cJSON_AddBoolToObject(o, "cached", refresh.cached);
    cJSON_AddNumberToObject(o, "revision", orbit.catalog_revision);
    cJSON_AddNumberToObject(o, "gameCount", (double)games);
    cJSON_AddNumberToObject(o, "checkedAt", (double)refresh.checked);
    cJSON_AddNumberToObject(o, "updatedAt", (double)refresh.updated);
    cJSON_AddNumberToObject(o, "checkAfter", (double)refresh.check_after);
    cJSON_AddNumberToObject(o, "nextCheck", (double)refresh.next_check);
    cJSON_AddStringToObject(o, "error", refresh.error);
    pthread_mutex_unlock(&orbit.mutex);
    return o;
}
int catalog_refresh(char *error, size_t cap) {
    int code = 202;
    pthread_mutex_lock(&orbit.mutex);
    if (!refresh.started || refresh.stop || orbit.stop) {
        code = 409; copy_text(error, cap, "Catalogue refresh is available while Orbit is running on your PS5.");
    } else if (refresh.busy || refresh.pending) {
        code = 409; copy_text(error, cap, "The catalogue is already refreshing.");
    } else if (time(NULL) < refresh.check_after) {
        code = 429; copy_text(error, cap, "Please wait before refreshing the catalogue again.");
    } else {
        refresh.pending = true;
        refresh.error[0] = 0;
        pthread_cond_signal(&refresh.changed);
    }
    pthread_mutex_unlock(&orbit.mutex);
    return code;
}
