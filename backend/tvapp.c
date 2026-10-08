/* The TV app from Orbit's public releases. Orbit reads a fixed feed, downloads PPSA99177.ffpkg
 * only from the release host, checks its size, SHA-256 and UFS2 image, and moves it into
 * /data/homebrew, where ShadowMountPlus registers it. Installing is always the owner's choice.
 * A copy they placed themselves is replaced only when they confirm, and Orbit never installs
 * beside a folder copy of the same title. Paths are relative to orbit.system_root, as in
 * autoboot.c. */
#include "ca.h"
#include "orbit.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define FEED "https://raw.githubusercontent.com/saawant12/orbit-store-ps5/main/tv-app.json"
#define RELEASE_BASE "https://github.com/saawant12/orbit-store-ps5/releases/download/"
#define ASSET_HOST "https://release-assets.githubusercontent.com/"
#define NAME "PPSA99177.ffpkg"
#define HOMEBREW "/data/homebrew"
#define INSTALLED HOMEBREW "/" NAME
#define FOLDER_APP HOMEBREW "/PPSA99177/eboot.bin"
#define ORBIT_DIR "/data/orbit-store"
#define PART ORBIT_DIR "/tv-app.part"
#define RECORD ORBIT_DIR "/tv-app.json"
#define MAX_IMAGE (256LL * 1024 * 1024)
#define SPARE (64LL * 1024 * 1024) /* Left free on /data after the image. */
#define UFS2_MAGIC_OFFSET (65536 + 1372)
#define COOLDOWN 30
#define AUTO_CHECK_INTERVAL (6 * 60 * 60)
typedef char Path[1100];

typedef enum { NOT_INSTALLED, ORBIT_COPY, OWN_COPY, FOLDER_COPY, BLOCKED } Installed;
static const char *const installed_names[] = {"none", "orbit", "manual", "folder", "blocked"};

static struct {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    pthread_t thread;
    bool started, stop, busy, replace;
    char action[16], phase[24], error[256];
    char version[40], url[512], checksum[65];
    int64_t size, received;
    time_t checked, retry_at, check_after, automatic_check_after;
} tv = {.mutex = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER};

typedef struct {
    int fd; /* -1 collects into data */
    unsigned char *data;
    int64_t size, limit;
    EVP_MD_CTX *digest;
    const char *failure;
} Sink;

static void at(char *out, const char *path) {
    snprintf(out, sizeof(Path), "%s%s", orbit.system_root, path);
}
static bool available(void) {
#ifdef ORBIT_TEST
    return orbit.autoboot;
#else
    return orbit.autoboot && !orbit.desktop;
#endif
}
static bool interrupted(void) {
    pthread_mutex_lock(&tv.mutex);
    bool stop = tv.stop;
    pthread_mutex_unlock(&tv.mutex);
    return stop;
}
static const char *test_base(void) {
#ifdef ORBIT_TEST
    const char *base = getenv("ORBIT_TEST_UPDATE_BASE");
    if (base && !strncmp(base, "http://127.0.0.1:", 17))
        return base;
#endif
    return NULL;
}
static bool test_url(const char *url) {
    const char *base = test_base();
    return base && !strncmp(url, base, strlen(base)) && url[strlen(base)] == '/';
}
static bool permitted(const char *url, bool asset) {
    return test_url(url) || (!asset ? !strcmp(url, FEED)
                                    : !strncmp(url, RELEASE_BASE, strlen(RELEASE_BASE)) ||
                                          !strncmp(url, ASSET_HOST, strlen(ASSET_HOST)));
}
static int ensure_dir(const char *path) {
    struct stat st;
    if (mkdir(path, 0755) && errno != EEXIST)
        return -1;
    return !lstat(path, &st) && S_ISDIR(st.st_mode) ? 0 : -1;
}

/* What is in /data/homebrew now. The record ties an image to Orbit's install by size, time and
 * inode, so a copy the owner swapped in later reads as their own. */
static Installed installed(char *version, size_t cap) {
    Path p;
    struct stat st;
    if (version)
        *version = 0;
    /* Check the folder independently: an image may also exist, including one
     * copied while an install was downloading. Never create/update a duplicate. */
    at(p, FOLDER_APP);
    if (!lstat(p, &st))
        return FOLDER_COPY;
    if (errno != ENOENT && errno != ENOTDIR)
        return BLOCKED;
    at(p, INSTALLED);
    if (lstat(p, &st)) {
        if (errno != ENOENT)
            return BLOCKED;
        return NOT_INSTALLED;
    }
    if (!S_ISREG(st.st_mode))
        return BLOCKED;
    Installed state = OWN_COPY;
    unsigned char *data;
    size_t n;
    at(p, RECORD);
    if (!read_regular_file(p, 4096, &data, &n)) {
        cJSON *record = cJSON_Parse((char *)data);
        free(data);
        if (json_int(record, "size") == (int64_t)st.st_size &&
            json_int(record, "modified") == (int64_t)st.st_mtime &&
            json_int(record, "inode") == (int64_t)st.st_ino) {
            state = ORBIT_COPY;
            if (version)
                copy_text(version, cap, json_text(record, "version"));
        }
        cJSON_Delete(record);
    }
    return state;
}

static size_t sink_write(char *data, size_t a, size_t b, void *context) {
    Sink *sink = context;
    size_t n = a * b;
    if ((int64_t)n > sink->limit - sink->size) {
        sink->failure =
            sink->fd < 0 ? "The TV app release information is too large. Nothing was downloaded."
                         : "The download was larger than the release says. Nothing was installed.";
        return 0;
    }
    if (interrupted())
        return 0;
    if (sink->fd < 0) {
        unsigned char *next = realloc(sink->data, (size_t)sink->size + n + 1);
        if (!next)
            return 0;
        sink->data = next;
        memcpy(next + sink->size, data, n);
        next[sink->size + (int64_t)n] = 0;
    } else {
        for (size_t p = 0; p < n;) {
            ssize_t w = write(sink->fd, data + p, n - p);
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0) {
                sink->failure = "Could not save the TV app on the console's internal storage.";
                return 0;
            }
            p += (size_t)w;
        }
        if (!EVP_DigestUpdate(sink->digest, data, n))
            return 0;
        pthread_mutex_lock(&tv.mutex);
        tv.received = sink->size + (int64_t)n;
        pthread_mutex_unlock(&tv.mutex);
    }
    sink->size += (int64_t)n;
    return n;
}
static int sink_reset(Sink *sink) {
    sink->size = 0;
    sink->failure = NULL;
    if (sink->fd < 0)
        return 0;
    if (ftruncate(sink->fd, 0) || lseek(sink->fd, 0, SEEK_SET) < 0)
        return -1;
    return EVP_DigestInit_ex(sink->digest, EVP_sha256(), NULL) ? 0 : -1;
}
static int progress(void *ctx, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)ctx;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    return interrupted();
}
/* 0 once a 200 reply is complete. Redirects are followed only within the allowed hosts. */
static int fetch(const char *url, Sink *sink, bool asset, long *status, char *error, size_t cap) {
    char current[4096];
    copy_text(current, sizeof current, url);
    *status = 0;
    for (unsigned redirects = 0; redirects <= 4; redirects++) {
        if (!permitted(current, asset)) {
            copy_text(error, cap,
                      "The TV app release redirected outside Orbit's allowed download hosts.");
            return -1;
        }
        CURL *curl = sink_reset(sink) ? NULL : curl_easy_init();
        if (!curl) {
            copy_text(error, cap, "Could not start the TV app download.");
            return -1;
        }
        struct curl_blob ca = {(void *)orbit_ca, sizeof orbit_ca - 1, CURL_BLOB_COPY};
        curl_easy_setopt(curl, CURLOPT_URL, current);
        curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, test_url(current) ? "http" : "https");
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, asset ? 1800L : 30L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Orbit-Store/" ORBIT_VERSION);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sink_write);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, sink);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        CURLcode result = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
        char *location = NULL;
        curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &location);
        bool redirect = result == CURLE_OK && *status >= 300 && *status < 400 && location &&
                        strlen(location) < sizeof current;
        if (redirect)
            copy_text(current, sizeof current, location);
        curl_easy_cleanup(curl);
        if (*status == 403 || *status == 429 || *status == 503) {
            pthread_mutex_lock(&tv.mutex);
            tv.retry_at = time(NULL) + 60;
            pthread_mutex_unlock(&tv.mutex);
        }
        if (redirect)
            continue;
        if (result == CURLE_OK && *status == 200)
            return 0;
        if (interrupted())
            copy_text(error, cap, "The TV app download stopped because Orbit is closing.");
        else if (sink->failure)
            copy_text(error, cap, sink->failure);
        else
            snprintf(error, cap, "TV app download failed (HTTP %ld, %s). Try again later.", *status,
                     curl_easy_strerror(result));
        return -1;
    }
    copy_text(error, cap, "Too many release redirects.");
    return -1;
}

static bool plain_version(const char *v) {
    size_t n = strlen(v);
    return n && n < 32 && orbit_version_compare(v, v) == 0;
}
static int check(char *error, size_t cap) {
    char url[600];
    const char *base = test_base();
    if (base)
        snprintf(url, sizeof url, "%s/tv-app", base);
    else
        copy_text(url, sizeof url, FEED);
    Sink sink = {.fd = -1, .limit = 16384};
    long status;
    int rc = fetch(url, &sink, false, &status, error, cap);
    cJSON *feed = rc ? NULL : cJSON_Parse((char *)sink.data);
    free(sink.data);
    if (rc) {
        if (status == 404)
            copy_text(error, cap,
                      "The TV app hasn't been published yet. Check again after the next release.");
        return -1;
    }
    const char *version = json_text(feed, "version"), *asset = json_text(feed, "url"),
               *hash = json_text(feed, "checksum");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(feed, "size");
    double bytes = cJSON_IsNumber(size) ? size->valuedouble : 0;
    size_t length = strlen(asset), tail = strlen("/" NAME);
    if (*version == 'v')
        version++;
    bool url_ok = length < sizeof tv.url &&
                  (test_url(asset) || (!strncmp(asset, RELEASE_BASE, strlen(RELEASE_BASE)) &&
                                       length > tail && !strcmp(asset + length - tail, "/" NAME) &&
                                       !strpbrk(asset, "?#\\ ") && !strstr(asset, "..")));
    if (!cJSON_IsObject(feed) || strcmp(json_text(feed, "filename"), NAME) ||
        !plain_version(version) || !url_ok || strlen(hash) != 64 ||
        strspn(hash, "0123456789abcdef") != 64 || bytes < UFS2_MAGIC_OFFSET + 4 ||
        bytes > (double)MAX_IMAGE || bytes != (double)(int64_t)bytes) {
        cJSON_Delete(feed);
        copy_text(error, cap, "The TV app release information is invalid. Nothing was downloaded.");
        return -1;
    }
    pthread_mutex_lock(&tv.mutex);
    copy_text(tv.version, sizeof tv.version, version);
    copy_text(tv.url, sizeof tv.url, asset);
    copy_text(tv.checksum, sizeof tv.checksum, hash);
    tv.size = (int64_t)bytes;
    tv.checked = time(NULL);
    pthread_mutex_unlock(&tv.mutex);
    cJSON_Delete(feed);
    return 0;
}

static void set_phase(const char *phase) {
    pthread_mutex_lock(&tv.mutex);
    copy_text(tv.phase, sizeof tv.phase, phase);
    pthread_mutex_unlock(&tv.mutex);
}
static int save_record(const char *path, const char *text) {
    Path temp;
    snprintf(temp, sizeof temp, "%s.orbit-tmp", path);
    unlink(temp);
    int fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0)
        return -1;
    size_t n = strlen(text);
    int rc = write(fd, text, n) == (ssize_t)n && !fsync(fd) ? 0 : -1;
    if (close(fd))
        rc = -1;
    if (!rc)
        rc = rename(temp, path);
    if (rc)
        unlink(temp);
    return rc;
}
/* Checks the finished download: its length, SHA-256 and the UFS2 superblock magic. */
static int verify(Sink *sink, int64_t size, const char *checksum, char *error, size_t cap) {
    unsigned char digest[EVP_MAX_MD_SIZE], magic[4];
    unsigned length = 0;
    char hex[65];
    if (!EVP_DigestFinal_ex(sink->digest, digest, &length) || length != 32)
        return -1;
    for (unsigned i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (sink->size != size || strcmp(hex, checksum)) {
        copy_text(error, cap, "The TV app failed its SHA-256 check. Nothing was installed.");
        return -1;
    }
    if (pread(sink->fd, magic, sizeof magic, UFS2_MAGIC_OFFSET) != (ssize_t)sizeof magic ||
        magic[0] != 0x19 || magic[1] != 0x01 || magic[2] != 0x54 || magic[3] != 0x19) {
        copy_text(error, cap, "The download isn't a TV app image. Nothing was installed.");
        return -1;
    }
    return 0;
}
static int install(char *error, size_t cap) {
    char url[512], checksum[65], version[40];
    pthread_mutex_lock(&tv.mutex);
    copy_text(url, sizeof url, tv.url);
    copy_text(checksum, sizeof checksum, tv.checksum);
    copy_text(version, sizeof version, tv.version);
    int64_t size = tv.size;
    bool replace = tv.replace;
    pthread_mutex_unlock(&tv.mutex);
    Path dir, part, target, record;
    at(dir, ORBIT_DIR);
    int orbit_dir = ensure_dir(dir);
    at(dir, HOMEBREW);
    if (orbit_dir || ensure_dir(dir)) {
        snprintf(error, cap, "Could not prepare %s on the console (%s).", HOMEBREW,
                 strerror(errno));
        return -1;
    }
    at(part, PART);
    at(target, INSTALLED);
    at(record, RECORD);
    unlink(part);
    Sink sink = {.fd = open(part, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0644),
                 .limit = size,
                 .digest = EVP_MD_CTX_new()};
    long status;
    int rc = -1;
    if (sink.fd < 0 || !sink.digest)
        copy_text(error, cap, "Could not save the TV app on the console's internal storage.");
    else
        rc = fetch(url, &sink, true, &status, error, cap);
    if (!rc) {
        set_phase("verifying");
        rc = verify(&sink, size, checksum, error, cap);
    }
    if (!rc && fsync(sink.fd)) {
        rc = -1;
        copy_text(error, cap, "Could not save the TV app on the console's internal storage.");
    }
    if (sink.fd >= 0 && close(sink.fd) && !rc) {
        rc = -1;
        copy_text(error, cap, "Could not save the TV app on the console's internal storage.");
    }
    EVP_MD_CTX_free(sink.digest);
    if (!rc && interrupted()) {
        rc = -1;
        copy_text(error, cap, "The TV app install stopped because Orbit is closing.");
    }
    if (!rc) {
        set_phase("installing");
        /* The owner may have copied something in while the download ran. */
        char current[40] = {0};
        Installed now = installed(current, sizeof current);
        if (now == FOLDER_COPY || now == BLOCKED || (now == OWN_COPY && !replace)) {
            rc = -1;
            copy_text(error, cap,
                      "Another copy of the TV app appeared while downloading. Nothing was "
                      "replaced; check again to see what's installed.");
        } else if (now == ORBIT_COPY && orbit_version_compare(version, current) == -1) {
            rc = -1;
            copy_text(error, cap, "A newer TV app is already installed. Nothing was replaced.");
        } else if (rename(part, target)) {
            rc = -1;
            snprintf(error, cap, "Could not move the TV app into %s (%s).", HOMEBREW,
                     strerror(errno));
        }
    }
    if (rc) {
        unlink(part);
        return rc;
    }
    struct stat st;
    char text[512];
    bool recorded = !lstat(target, &st);
    if (recorded) {
        snprintf(text, sizeof text,
                 "{\"version\":\"%s\",\"checksum\":\"%s\",\"size\":%lld,\"modified\":%lld,"
                 "\"inode\":%lld}\n",
                 version, checksum, (long long)st.st_size, (long long)st.st_mtime,
                 (long long)st.st_ino);
        recorded = !save_record(record, text);
    }
    if (!recorded) {
        copy_text(error, cap,
                  "The TV app was installed, but Orbit could not record its version. It will "
                  "show as a copy you placed yourself.");
        return -1;
    }
    return 0;
}

static void *worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&tv.mutex);
    while (!tv.stop) {
        while (!tv.action[0] && !tv.stop)
            pthread_cond_wait(&tv.changed, &tv.mutex);
        if (tv.stop)
            break;
        bool checking = !strcmp(tv.action, "check");
        tv.action[0] = 0;
        pthread_mutex_unlock(&tv.mutex);
        char error[256] = {0};
        int rc = checking ? check(error, sizeof error) : install(error, sizeof error);
        pthread_mutex_lock(&tv.mutex);
        copy_text(tv.error, sizeof tv.error,
                  rc ? (*error ? error : "The TV app could not be installed. Try again.") : "");
        copy_text(tv.phase, sizeof tv.phase, rc ? "error" : checking ? "checked" : "installed");
        tv.busy = false;
    }
    pthread_mutex_unlock(&tv.mutex);
    return NULL;
}
int tvapp_start(void) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr))
        return -1;
    int rc = pthread_attr_setstacksize(&attr, ORBIT_THREAD_STACK);
    if (!rc)
        rc = pthread_create(&tv.thread, &attr, worker, NULL);
    pthread_attr_destroy(&attr);
    pthread_mutex_lock(&tv.mutex);
    tv.started = !rc;
    pthread_mutex_unlock(&tv.mutex);
    return rc ? -1 : 0;
}
void tvapp_stop(void) {
    pthread_mutex_lock(&tv.mutex);
    tv.stop = true;
    pthread_cond_signal(&tv.changed);
    bool started = tv.started;
    pthread_mutex_unlock(&tv.mutex);
    if (started)
        pthread_join(tv.thread, NULL);
}
cJSON *tvapp_status(void) {
    char current[40] = {0};
    Installed state = available() ? installed(current, sizeof current) : NOT_INSTALLED;
    cJSON *o = cJSON_CreateObject();
    pthread_mutex_lock(&tv.mutex);
    cJSON_AddBoolToObject(o, "available", available() && tv.started);
    cJSON_AddStringToObject(o, "installed", installed_names[state]);
    cJSON_AddStringToObject(o, "installedVersion", current);
    cJSON_AddStringToObject(o, "latestVersion", tv.version);
    cJSON_AddStringToObject(o, "checksum", tv.checksum);
    cJSON_AddNumberToObject(o, "size", (double)tv.size);
    cJSON_AddBoolToObject(o, "updateAvailable",
                          state == ORBIT_COPY && orbit_version_compare(tv.version, current) > 0);
    cJSON_AddStringToObject(o, "phase", *tv.phase ? tv.phase : "idle");
    cJSON_AddStringToObject(o, "error", tv.error);
    cJSON_AddBoolToObject(o, "busy", tv.busy);
    cJSON_AddNumberToObject(o, "received", (double)tv.received);
    cJSON_AddNumberToObject(o, "retryAt", (double)tv.retry_at);
    cJSON_AddNumberToObject(o, "checkedAt", (double)tv.checked);
    cJSON_AddNumberToObject(o, "checkAfter", (double)tv.check_after);
    cJSON_AddNumberToObject(o, "automaticCheckAfter", (double)tv.automatic_check_after);
    pthread_mutex_unlock(&tv.mutex);
    return o;
}
int tvapp_action(const cJSON *input, char *error, size_t cap) {
    const char *action = json_text(input, "action");
    bool checking = !strcmp(action, "check"), installing = !strcmp(action, "install");
    const cJSON *replace = cJSON_GetObjectItemCaseSensitive(input, "replace");
    const cJSON *automatic_input = cJSON_GetObjectItemCaseSensitive(input, "automatic");
    bool automatic = checking && cJSON_IsTrue(automatic_input);
    time_t now = time(NULL);
    int code = 202;
    pthread_mutex_lock(&tv.mutex);
    if (!checking && !installing) {
        code = 400;
        copy_text(error, cap, "Unknown TV app action.");
    } else if (replace && (!installing || !cJSON_IsBool(replace))) {
        code = 400;
        copy_text(error, cap, "Replace is only valid, as true or false, when installing.");
    } else if (automatic_input && (!checking || !cJSON_IsBool(automatic_input))) {
        code = 400;
        copy_text(error, cap, "Automatic mode is only valid for TV app checks.");
    } else if (!available() || !tv.started) {
        code = 409;
        copy_text(error, cap, "The TV app is installed from Orbit running on your PS5.");
    } else if (automatic && !tv.stop &&
               (tv.busy || tv.automatic_check_after > now || tv.retry_at > now ||
                tv.check_after > now)) {
        /* Opening the TV app again, or another client, shares the console's cooldown. */
        code = 0;
    } else if (tv.busy || tv.stop) {
        code = 409;
        copy_text(error, cap, "The TV app is already being checked or installed. Wait for it.");
    } else if (tv.retry_at > now || (checking && tv.check_after > now)) {
        code = 429;
        copy_text(error, cap, "Please wait before checking or downloading again.");
    } else if (installing && (!tv.checked || now - tv.checked > 900 ||
                              strcmp(json_text(input, "version"), tv.version) ||
                              strcmp(json_text(input, "checksum"), tv.checksum))) {
        code = 409;
        copy_text(error, cap, "Check for the TV app again before installing it.");
    } else if (installing) {
        char current[40] = {0};
        Installed state = installed(current, sizeof current);
        struct statvfs vfs;
        Path data;
        at(data, "/data");
        if (state == FOLDER_COPY) {
            code = 409;
            copy_text(error, cap,
                      "The TV app is already installed as the folder /data/homebrew/PPSA99177. "
                      "Remove that folder to install the release, or keep updating it yourself.");
        } else if (state == BLOCKED) {
            code = 409;
            copy_text(error, cap,
                      "Something other than the TV app is at /data/homebrew/" NAME
                      ". Move it away first.");
        } else if (state == ORBIT_COPY && orbit_version_compare(tv.version, current) == -1) {
            code = 409;
            copy_text(error, cap, "A newer TV app is already installed. Nothing was replaced.");
        } else if (state == OWN_COPY && !cJSON_IsTrue(replace)) {
            code = 409;
            copy_text(error, cap,
                      "A copy you placed yourself is already in /data/homebrew. Confirm "
                      "replacing it with this release.");
        } else if (!statvfs(data, &vfs) &&
                   (double)vfs.f_bavail * (double)(vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize) <
                       (double)(tv.size + SPARE)) {
            code = 507;
            snprintf(error, cap,
                     "Not enough free space on the console's internal storage. The TV app needs "
                     "about %lld MB.",
                     (long long)((tv.size + SPARE) / (1024 * 1024)));
        }
    }
    if (code == 202) {
        tv.busy = true;
        tv.error[0] = 0;
        tv.received = 0;
        tv.replace = installing && cJSON_IsTrue(replace);
        if (checking) {
            tv.checked = 0;
            tv.version[0] = 0;
            tv.checksum[0] = 0;
            tv.size = 0;
            tv.check_after = now + COOLDOWN;
            tv.automatic_check_after = now + AUTO_CHECK_INTERVAL;
        } else
            tv.retry_at = now + COOLDOWN;
        copy_text(tv.action, sizeof tv.action, action);
        copy_text(tv.phase, sizeof tv.phase, checking ? "checking" : "downloading");
        pthread_cond_signal(&tv.changed);
    }
    pthread_mutex_unlock(&tv.mutex);
    return code ? code : 202;
}
