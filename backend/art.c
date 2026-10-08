/* Catalogue artwork for the native TV app.
 *
 * The browser storefront loads covers straight from the catalogue's HTTPS
 * URLs. The TV app has no TLS stack of its own, so it asks Orbit instead:
 * GET /api/v1/art/<gameId>/<cover|hero>. Orbit fetches the catalogue's own
 * URL for that game (never a URL supplied by a client) on two workers, off
 * the single HTTP thread, and keeps the bytes in a bounded memory cache. The
 * first request for an image answers 202; the app asks again shortly. */
#include "ca.h"
#include "orbit.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ART_ENTRIES 256
#define ART_WORKERS 2
#define ART_QUEUE 64
#define ART_MAX_BYTES (12 * 1024 * 1024)
#define ART_BUDGET (32 * 1024 * 1024)
#define ART_RETRY_FAILED 120 /* Seconds before a failed image is fetched again. */
#define ART_KEY 80

typedef enum { ART_EMPTY, ART_PENDING, ART_READY, ART_FAILED } ArtState;
typedef struct {
    char key[ART_KEY];
    ArtState state;
    unsigned char *data;
    size_t size;
    char mime[16];
    time_t used, failed_at;
} ArtEntry;
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t threads[ART_WORKERS];
    size_t started;
    bool stop;
    ArtEntry entries[ART_ENTRIES];
    char queue[ART_QUEUE][ART_KEY];
    size_t queued;
    size_t bytes;
} art = {.mutex = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER};

typedef struct {
    unsigned char *data;
    size_t size;
} Buffer;

static bool art_identifier(const char *id) {
    size_t n = strlen(id);
    if (!n || n > 63)
        return false;
    for (const char *p = id; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '-' || *p == '_' || *p == '.'))
            return false;
    return strcmp(id, ".") && strcmp(id, "..");
}

/* The image format from its first bytes; the server's Content-Type is not trusted. */
static const char *sniff(const unsigned char *d, size_t n) {
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4))
        return "image/webp";
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8))
        return "image/png";
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
        return "image/jpeg";
    return NULL;
}

/* Up to two catalogue URLs for a visible game, in preference order. Returns
 * the count; 0 when the game is unknown, hidden or has no such artwork. */
static size_t urls_for(const char *game_id, bool hero, char out[2][2048]) {
    size_t count = 0;
    pthread_mutex_lock(&orbit.mutex);
    const cJSON *entry;
    cJSON_ArrayForEach(entry, orbit.catalog) {
        if (strcmp(json_text(entry, "gameId"), game_id) ||
            !source_enabled_locked(release_find(json_text(entry, "id"))))
            continue;
        /* Wide layouts have a publisher banner as hero; ambient ones may have a
         * banner only as heroFallback (docs/api.md). */
        const char *keys[2] = {"cover", "coverFallback"};
        if (hero) {
            bool wide = !strcmp(json_text(entry, "artworkLayout"), "wide");
            keys[0] = wide ? "hero" : "heroFallback";
            keys[1] = wide ? "heroFallback" : NULL;
        }
        for (size_t k = 0; k < 2; k++) {
            const char *url = keys[k] ? json_text(entry, keys[k]) : "";
            if (!strncmp(url, "https://", 8) && strlen(url) < 2048)
                copy_text(out[count++], 2048, url);
        }
        break;
    }
    pthread_mutex_unlock(&orbit.mutex);
    return count;
}

static size_t collect(char *data, size_t a, size_t b, void *context) {
    Buffer *buf = context;
    if (a && b > SIZE_MAX / a)
        return 0;
    size_t n = a * b;
    if (n > ART_MAX_BYTES - buf->size)
        return 0;
    unsigned char *next = realloc(buf->data, buf->size + n);
    if (!next)
        return 0;
    buf->data = next;
    memcpy(next + buf->size, data, n);
    buf->size += n;
    return n;
}

static int stopping(void *ctx, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)ctx;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    pthread_mutex_lock(&art.mutex);
    bool stop = art.stop;
    pthread_mutex_unlock(&art.mutex);
    return stop;
}

/* Fetches one URL. In test builds ORBIT_TEST_ART_ORIGIN replaces the scheme
 * and host with a loopback fixture, keeping the path. */
static bool fetch(const char *source, Buffer *buf, const char **mime) {
    char url[2300];
    const char *protocols = "https";
    copy_text(url, sizeof url, source);
#ifdef ORBIT_TEST
    const char *origin = getenv("ORBIT_TEST_ART_ORIGIN");
    if (origin && *origin) {
        const char *path = strchr(source + 8, '/');
        snprintf(url, sizeof url, "%s%s", origin, path ? path : "/");
        protocols = "http";
    }
#endif
    CURL *curl = curl_easy_init();
    if (!curl)
        return false;
    struct curl_blob ca = {(void *)orbit_ca, sizeof orbit_ca - 1, CURL_BLOB_COPY};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, protocols);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, protocols);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)ART_MAX_BYTES);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Orbit-Store/" ORBIT_VERSION);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, stopping);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    *mime = code == CURLE_OK && status == 200 ? sniff(buf->data, buf->size) : NULL;
    return *mime != NULL;
}

static ArtEntry *find(const char *key) {
    for (size_t i = 0; i < ART_ENTRIES; i++)
        if (art.entries[i].state != ART_EMPTY && !strcmp(art.entries[i].key, key))
            return &art.entries[i];
    return NULL;
}

/* Drops the least recently served images until the cache fits its budget. */
static void trim_locked(const ArtEntry *keep) {
    while (art.bytes > ART_BUDGET) {
        ArtEntry *oldest = NULL;
        for (size_t i = 0; i < ART_ENTRIES; i++) {
            ArtEntry *e = &art.entries[i];
            if (e->state == ART_READY && e != keep && (!oldest || e->used < oldest->used))
                oldest = e;
        }
        if (!oldest)
            return;
        art.bytes -= oldest->size;
        free(oldest->data);
        memset(oldest, 0, sizeof *oldest);
    }
}

static void *worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&art.mutex);
    while (!art.stop) {
        if (!art.queued) {
            pthread_cond_wait(&art.changed, &art.mutex);
            continue;
        }
        char key[ART_KEY];
        copy_text(key, sizeof key, art.queue[0]);
        memmove(art.queue[0], art.queue[1], (art.queued - 1) * ART_KEY);
        art.queued--;
        pthread_mutex_unlock(&art.mutex);

        char game[ART_KEY], urls[2][2048];
        copy_text(game, sizeof game, key);
        char *hash = strchr(game, '#');
        bool hero = hash && !strcmp(hash + 1, "hero");
        if (hash)
            *hash = 0;
        size_t count = urls_for(game, hero, urls);
        Buffer buf = {0};
        const char *mime = NULL;
        for (size_t i = 0; i < count && !mime; i++) {
            free(buf.data);
            buf = (Buffer){0};
            fetch(urls[i], &buf, &mime);
        }

        pthread_mutex_lock(&art.mutex);
        ArtEntry *e = find(key);
        if (!e || e->state != ART_PENDING) {
            free(buf.data);
            continue;
        }
        e->used = time(NULL);
        if (mime) {
            e->state = ART_READY;
            e->data = buf.data;
            e->size = buf.size;
            copy_text(e->mime, sizeof e->mime, mime);
            art.bytes += buf.size;
            trim_locked(e);
        } else {
            free(buf.data);
            e->state = ART_FAILED;
            e->failed_at = time(NULL);
        }
    }
    pthread_mutex_unlock(&art.mutex);
    return NULL;
}

int art_get(const char *game_id, const char *kind, unsigned char **data, size_t *size, char *mime,
            size_t mime_cap) {
    *data = NULL;
    *size = 0;
    bool hero = !strcmp(kind, "hero");
    if (!art_identifier(game_id) || (!hero && strcmp(kind, "cover")))
        return 404;
    char urls[2][2048];
    if (!urls_for(game_id, hero, urls))
        return 404;
    char key[ART_KEY];
    snprintf(key, sizeof key, "%s#%s", game_id, kind);
    time_t now = time(NULL);
    int code = 202;
    pthread_mutex_lock(&art.mutex);
    ArtEntry *e = find(key);
    if (e && e->state == ART_READY) {
        *data = malloc(e->size);
        if (*data) {
            memcpy(*data, e->data, e->size);
            *size = e->size;
            copy_text(mime, mime_cap, e->mime);
            e->used = now;
            code = 200;
        } else {
            code = 503;
        }
    } else if (e && e->state == ART_PENDING) {
        code = 202;
    } else if (e && e->state == ART_FAILED && now - e->failed_at < ART_RETRY_FAILED) {
        code = 502;
    } else if (!art.started || art.queued == ART_QUEUE) {
        code = 503;
    } else {
        /* A free slot, else the least recently used finished one. */
        for (size_t i = 0; i < ART_ENTRIES && !e; i++)
            if (art.entries[i].state == ART_EMPTY)
                e = &art.entries[i];
        for (size_t i = 0; i < ART_ENTRIES && (!e || e->state != ART_EMPTY); i++) {
            ArtEntry *c = &art.entries[i];
            if (c->state != ART_PENDING && (!e || c->used < e->used))
                e = c;
        }
        if (!e) {
            code = 503;
        } else {
            if (e->state == ART_READY)
                art.bytes -= e->size;
            free(e->data);
            memset(e, 0, sizeof *e);
            copy_text(e->key, sizeof e->key, key);
            e->state = ART_PENDING;
            e->used = now;
            copy_text(art.queue[art.queued++], ART_KEY, key);
            pthread_cond_signal(&art.changed);
        }
    }
    pthread_mutex_unlock(&art.mutex);
    return code;
}

int art_start(void) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr))
        return -1;
    int rc = pthread_attr_setstacksize(&attr, ORBIT_THREAD_STACK);
    pthread_mutex_lock(&art.mutex);
    for (size_t i = 0; !rc && i < ART_WORKERS; i++) {
        rc = pthread_create(&art.threads[i], &attr, worker, NULL);
        if (!rc)
            art.started++;
    }
    pthread_mutex_unlock(&art.mutex);
    pthread_attr_destroy(&attr);
    return rc ? -1 : 0;
}

void art_stop(void) {
    pthread_mutex_lock(&art.mutex);
    size_t started = art.started;
    art.stop = true;
    pthread_cond_broadcast(&art.changed);
    pthread_mutex_unlock(&art.mutex);
    for (size_t i = 0; i < started; i++)
        pthread_join(art.threads[i], NULL);
    for (size_t i = 0; i < ART_ENTRIES; i++)
        free(art.entries[i].data);
    memset(art.entries, 0, sizeof art.entries);
    art.bytes = 0;
}
