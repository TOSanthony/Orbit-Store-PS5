#include "ca.h"
#include "orbit.h"
#include "debrid.h"
#include <openssl/crypto.h>
#include "provider_network.h"
#include "transfer_writer.h"
#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#ifdef ORBIT_BENCHMARK
#include "transfer_benchmark.h"
#endif
_Static_assert(sizeof(off_t) >= 8, "Orbit requires 64-bit file offsets");
#define PARALLEL_MIN_REMAINING (32LL * 1024 * 1024)
typedef struct Transfer {
    Job *job;
    Release *release;
    int fd;
    long status;
    int64_t length, start, end, total, offset, written, limit;
    int64_t live[ORBIT_MAX_RANGES];
    TransferWriter *writer;
    struct Transfer *owner;
    int segment;
    char etag[256], modified[128], type[128], error[256];
    time_t retry_at;
    bool accepted, probe, range_probe, unsupported_range;
    bool capture_probe;
    atomic_bool *cancel_signal;
    CURL *handle;
    char disposition[512];
    double started;
#ifdef ORBIT_BENCHMARK
    int64_t sample_span;
    double deadline;
    long socket_buffer, curl_buffer;
    int socket_before, socket_after, socket_error;
    cJSON *metrics;
    cJSON *speed_samples;
    double last_sample;
    int64_t enqueued;
#endif
} Transfer;
static bool torbox(const Transfer *t) { return !strcmp(t->job->delivery, "torbox"); }
static bool guarded(const Transfer *t) { return torbox(t) || !strcmp(t->release->source, "vikingfile"); }
static bool address_allowed(const Transfer *t, const char *url) {
    return torbox(t) ? debrid_transfer_url_allowed(url) : provider_transfer_url_allowed(url);
}
static double seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static int64_t transfer_span(const Transfer *t) {
#ifdef ORBIT_BENCHMARK
    const Transfer *owner = t->owner ? t->owner : t;
    if (owner->sample_span) return owner->sample_span;
#endif
    return t->release->size;
}
static void header_value(char *out, size_t cap, const char *s, size_t n) {
    while (n && (*s == ' ' || *s == '\t')) {
        s++;
        n--;
    }
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' '))
        n--;
    if (n >= cap)
        n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
}
static bool strong_etag(const char *value) {
    size_t length = strlen(value);
    if (length < 2 || value[0] != '"' || value[length - 1] != '"') return false;
    for (size_t i = 1; i + 1 < length; i++)
        if ((unsigned char)value[i] < 33 || value[i] == '"' || (unsigned char)value[i] == 127) return false;
    return true;
}
static bool disposition_matches(const char *header, const char *expected) {
    bool plain = false, extended = false, plain_match = false, extended_match = false;
    const char *p = strchr(header, ';');
    while (p && *p) {
        p++;
        while (*p == ' ' || *p == '\t') p++;
        bool star = !strncasecmp(p, "filename*=", 10);
        bool filename = star || !strncasecmp(p, "filename=", 9);
        if (!filename) { p = strchr(p, ';'); continue; }
        p += star ? 10 : 9;
        while (*p == ' ' || *p == '\t') p++;
        bool quoted = *p == '"';
        if (quoted) p++;
        char value[512]; size_t n = 0;
        while (*p && (quoted ? *p != '"' : *p != ';')) {
            if (quoted && *p == '\\') { p++; if (!*p) return false; }
            if (n + 1 >= sizeof value) return false;
            value[n++] = *p++;
        }
        if (quoted) {
            if (*p != '"') return false;
            p++;
            while (*p == ' ' || *p == '\t') p++;
            if (*p && *p != ';') return false;
        } else while (n && (value[n-1] == ' ' || value[n-1] == '\t')) n--;
        value[n] = 0;
        if (star) {
            if (extended || strncasecmp(value, "UTF-8'", 6)) return false;
            const char *encoded = strchr(value + 6, '\'');
            if (!encoded) return false;
            int length = 0;
            char *decoded = curl_easy_unescape(NULL, encoded + 1, 0, &length);
            extended = true;
            extended_match = decoded && length == (int)strlen(expected) &&
                             !memcmp(decoded, expected, (size_t)length);
            curl_free(decoded);
        } else {
            if (plain) return false;
            plain = true; plain_match = !strcmp(value, expected);
        }
        if (!*p) break;
    }
    return extended ? extended_match : plain && plain_match;
}
static bool controlled(Transfer *t) {
    /* These flags are atomic: a writer saving queue state must not block the
     * network callback on orbit.mutex. Capture probes still check source policy. */
    if (atomic_load(&orbit.stop) || atomic_load(&t->job->pause) || atomic_load(&t->job->cancel) ||
        (t->cancel_signal && atomic_load(t->cancel_signal))) return true;
    Transfer *owner = t->owner ? t->owner : t;
#ifdef ORBIT_BENCHMARK
    if (owner->deadline && seconds() >= owner->deadline) return true;
#endif
    if (transfer_writer_failed(owner->writer)) return true;
    if (t->capture_probe) {
        pthread_mutex_lock(&orbit.mutex);
        bool enabled = source_enabled_locked(t->release);
        pthread_mutex_unlock(&orbit.mutex);
        return !enabled;
    }
    return false;
}
static size_t headers(char *s, size_t a, size_t b, void *arg) {
    Transfer *t = arg;
    size_t n = a * b;
    if (n > 5 && !strncmp(s, "HTTP/", 5)) {
        char line[100];
        header_value(line, sizeof line, s, n);
        char *p = strchr(line, ' ');
        t->status = p ? strtol(p + 1, NULL, 10) : 0;
        t->length = t->start = t->end = t->total = -1;
        /* libcurl calls us for every response in a redirect chain. None of the
         * previous response's error or retry metadata describes the new one. */
        t->etag[0] = t->modified[0] = t->type[0] = t->error[0] = t->disposition[0] = 0;
        t->retry_at = 0;
        t->accepted = false;
    } else if (n > 15 && !strncasecmp(s, "Content-Length:", 15)) {
        t->length = strtoll(s + 15, NULL, 10);
    } else if (n > 14 && !strncasecmp(s, "Content-Range:", 14)) {
        long long x, y, z;
        if (sscanf(s + 14, " bytes %lld-%lld/%lld", &x, &y, &z) == 3) {
            t->start = x;
            t->end = y;
            t->total = z;
        }
    } else if (n > 5 && !strncasecmp(s, "ETag:", 5)) {
        header_value(t->etag, sizeof t->etag, s + 5, n - 5);
    } else if (n > 14 && !strncasecmp(s, "Last-Modified:", 14)) {
        header_value(t->modified, sizeof t->modified, s + 14, n - 14);
    } else if (n > 13 && !strncasecmp(s, "Content-Type:", 13)) {
        header_value(t->type, sizeof t->type, s + 13, n - 13);
        for (char *p = t->type; *p; p++) *p = (char)tolower((unsigned char)*p);
    } else if (n > 9 && !strncasecmp(s, "Location:", 9) &&
               guarded(t)) {
        char location[8193], *base = NULL, *resolved = NULL;
        if (n - 9 >= sizeof location) return 0;
        header_value(location, sizeof location, s + 9, n - 9);
        CURLU *url = curl_url();
        curl_easy_getinfo(t->handle, CURLINFO_EFFECTIVE_URL, &base);
        bool allowed = url && base && !curl_url_set(url, CURLUPART_URL, base, 0) &&
            !curl_url_set(url, CURLUPART_URL, location, 0) &&
            !curl_url_get(url, CURLUPART_URL, &resolved, 0) && address_allowed(t, resolved);
        curl_free(resolved); curl_url_cleanup(url);
        if (!allowed) {
            copy_text(t->error, sizeof t->error, "Provider redirected to an unsupported download host.");
            return 0;
        }
    } else if (n > 20 && !strncasecmp(s, "Content-Disposition:", 20)) {
        header_value(t->disposition, sizeof t->disposition, s + 20, n - 20);
    } else if (n > 12 && !strncasecmp(s, "Retry-After:", 12)) {
        char v[100];
        header_value(v, sizeof v, s + 12, n - 12);
        char *end = NULL;
        long long wait = strtoll(v, &end, 10);
        time_t now = time(NULL);
        t->retry_at = (end && !*end && wait >= 0 && wait < 31536000) ? now + (time_t)wait
                                                                     : curl_getdate(v, NULL);
    } else if ((n == 2 && s[0] == '\r') || (n == 1 && s[0] == '\n')) {
        /* Redirects commonly have an HTML body. Validate the file response,
         * after libcurl follows Location, rather than rejecting that page. */
        if (t->probe || (t->status >= 100 && t->status < 200) ||
            (t->status >= 300 && t->status < 400))
            return n;
        if (t->status == 200 && t->offset == 0 && t->limit == t->release->size &&
            t->length == t->release->size && !t->owner)
            t->accepted = true;
        if (t->status == 206 && t->start == t->offset && t->total == t->release->size &&
            t->end == t->limit - 1 && t->length == t->limit - t->offset)
            t->accepted = true;
        if ((t->accepted || (t->range_probe && t->status == 200)) &&
            ((t->job->etag[0] && strcmp(t->job->etag, t->etag)) ||
             (!t->job->etag[0] && t->job->modified[0] && strcmp(t->job->modified, t->modified)))) {
            t->accepted = false;
            copy_text(t->error, sizeof t->error,
                      "Source changed during the request. Partial file preserved.");
        }
        if (strcmp(t->release->id, "orbit-network-check") &&
            (strstr(t->type, "text/") || strstr(t->type, "json") || strstr(t->type, "xml"))) {
            t->accepted = false;
            copy_text(t->error, sizeof t->error,
                      "Provider returned a page instead of a downloadable file.");
        }
        if ((t->capture_probe || torbox(t)) && t->disposition[0]) {
            if (!disposition_matches(t->disposition, t->release->filename)) {
                t->accepted = false;
                copy_text(t->error, sizeof t->error, "Provider returned a different filename.");
            }
        }
        if (t->range_probe && t->status == 200 && t->length == t->release->size && !t->error[0]) {
            /* Abort at headers; do not consume a full file for a one-byte probe. */
            t->unsupported_range = true;
            return 0;
        }
    }
    return n;
}
static size_t body(char *data, size_t a, size_t b, void *arg) {
    Transfer *t = arg;
    size_t n = a * b;
    if (t->status >= 300 && t->status < 400)
        return n;
    if (!t->accepted) {
        if (!t->error[0])
            copy_text(t->error, sizeof t->error,
                      "Invalid HTTP range or file size. Partial file preserved.");
        return 0;
    }
    if (controlled(t))
        return 0;
    if (t->offset + t->written + (int64_t)n > t->limit) {
        copy_text(t->error, sizeof t->error, "Provider sent more bytes than expected.");
        return 0;
    }
    if (t->range_probe) {
        t->written += (int64_t)n;
        return n;
    }
    Transfer *owner = t->owner ? t->owner : t;
    if (owner->error[0])
        return 0;
    if (transfer_writer_enqueue(owner->writer, (unsigned)t->segment, data, n)) return 0;
    t->written += (int64_t)n;
    unsigned count = t->job->range_count;
    owner->live[t->segment] = t->offset + t->written -
                             range_boundary(transfer_span(t), count, (unsigned)t->segment);
#ifdef ORBIT_BENCHMARK
    owner->enqueued += (int64_t)n;
    if (owner->speed_samples && seconds() - owner->last_sample >= 1) {
        owner->last_sample = seconds();
        cJSON *sample = cJSON_CreateObject();
        cJSON_AddNumberToObject(sample, "seconds", owner->last_sample - owner->started);
        cJSON_AddNumberToObject(sample, "bytes", (double)owner->enqueued);
        cJSON_AddItemToArray(owner->speed_samples, sample);
    }
#endif
    return n;
}
static int progress(void *arg, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un) {
    (void)dt;
    (void)dn;
    (void)ut;
    (void)un;
    return controlled(arg) ? 1 : 0;
}
static int before_request(void *arg, char *remote, char *local, int remote_port, int local_port) {
    (void)remote; (void)local; (void)remote_port; (void)local_port;
    Transfer *t = arg;
    char *url = NULL;
    curl_easy_getinfo(t->handle, CURLINFO_EFFECTIVE_URL, &url);
    if (controlled(t)) return CURL_PREREQFUNC_ABORT;
    if (address_allowed(t, url)) return CURL_PREREQFUNC_OK;
    copy_text(t->error, sizeof t->error, "Provider returned an unsupported download address.");
    return CURL_PREREQFUNC_ABORT;
}
#if defined(ORBIT_BENCHMARK) || !defined(ORBIT_DESKTOP)
static int configure_receive_buffer(curl_socket_t fd, long requested, int *before, int *after) {
    int original=0, actual=0, error=0;
    socklen_t length=sizeof original;
    if (getsockopt(fd,SOL_SOCKET,SO_RCVBUF,&original,&length)) error=errno;
    /* Never reduce a larger system buffer or fail a transfer if tuning is denied. */
    if (!error && requested>original) {
        int desired=(int)requested;
        if (setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&desired,sizeof desired)) error=errno;
    }
    length=sizeof actual;
    if (getsockopt(fd,SOL_SOCKET,SO_RCVBUF,&actual,&length) && !error) error=errno;
    if(before) *before=original;
    if(after) *after=actual;
    return error;
}
static int transfer_socket(void *arg, curl_socket_t fd, curlsocktype purpose) {
    (void)purpose;
#ifdef ORBIT_BENCHMARK
    Transfer *t = arg, *owner = t->owner ? t->owner : t;
    t->socket_error=configure_receive_buffer(fd,owner->socket_buffer,&t->socket_before,&t->socket_after);
#else
    (void)arg;
    /* Console measurements: 64 KiB defaults restrict high-latency throughput.
     * This console accepts a 1 MiB request as 512 KiB; the OS owns its cap. */
    (void)configure_receive_buffer(fd,1024*1024,NULL,NULL);
#endif
    return CURL_SOCKOPT_OK;
}
#endif
static CURL *request(Transfer *t) {
    CURL *c = curl_easy_init();
    if (!c)
        return NULL;
    t->handle = c;
    struct curl_blob ca = {(void *)orbit_ca, sizeof orbit_ca - 1, CURL_BLOB_COPY};
    curl_easy_setopt(c, CURLOPT_URL, t->release->url);
    curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &ca);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
#ifdef ORBIT_TEST
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#endif
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "OrbitStore/" ORBIT_VERSION);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, headers);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, t);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, t);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, t);
#if defined(ORBIT_BENCHMARK) || !defined(ORBIT_DESKTOP)
    curl_easy_setopt(c, CURLOPT_SOCKOPTFUNCTION, transfer_socket);
    curl_easy_setopt(c, CURLOPT_SOCKOPTDATA, t);
#endif
#ifdef ORBIT_BENCHMARK
    Transfer *owner = t->owner ? t->owner : t;
    if (owner->curl_buffer) curl_easy_setopt(c, CURLOPT_BUFFERSIZE, owner->curl_buffer);
    if (owner->deadline) {
        long left = (long)((owner->deadline - seconds()) * 1000);
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, left > 0 ? left : 1L);
    }
#endif
    if (guarded(t)) {
        curl_easy_setopt(c, CURLOPT_PROXY, "");
        curl_easy_setopt(c, CURLOPT_OPENSOCKETFUNCTION, provider_open_socket);
        curl_easy_setopt(c, CURLOPT_PREREQFUNCTION, before_request);
        curl_easy_setopt(c, CURLOPT_PREREQDATA, t);
    }
    return c;
}
static void set_range(CURL *c, int64_t offset, int64_t limit, struct curl_slist *conditions) {
    char range[64];
    snprintf(range, sizeof range, "%lld-%lld", (long long)offset, (long long)(limit - 1));
    curl_easy_setopt(c, CURLOPT_RANGE, range);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, conditions);
}
/* One bounded GET verifies actual range support, not an optional HEAD hint.
 * A valid full-file 200 means use the existing single-connection path. */
static int range_support(Transfer *owner, struct curl_slist *conditions, CURLcode *result) {
    Transfer probe = {
        .job = owner->job, .release = owner->release, .fd = -1, .limit = 1, .range_probe = true,
        .capture_probe = owner->capture_probe, .cancel_signal = owner->cancel_signal};
    CURL *c = request(&probe);
    if (!c) {
        copy_text(owner->error, sizeof owner->error, "Could not initialize range check.");
        return -1;
    }
    set_range(c, 0, 1, conditions);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    *result = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (probe.unsupported_range) {
        *result = CURLE_OK;
        return 0;
    }
    if (*result == CURLE_OK && probe.accepted && probe.written == 1) {
        if (owner->probe && guarded(owner)) {
            owner->status = probe.status;
            owner->length = probe.total;
            copy_text(owner->etag, sizeof owner->etag, probe.etag);
            copy_text(owner->modified, sizeof owner->modified, probe.modified);
        }
        return 1;
    }
    owner->status = probe.status;
    owner->retry_at = probe.retry_at;
    if (probe.error[0])
        copy_text(owner->error, sizeof owner->error, probe.error);
    else
        snprintf(owner->error, sizeof owner->error, "Range check failed (HTTP %ld, %s).",
                 probe.status, curl_easy_strerror(*result));
    return -1;
}
int transfer_validate_capture(Release *release, atomic_bool *cancel, char *etag,
                              char *modified, char *error, size_t cap) {
    if (!provider_capture_url(release, release->url)) {
        copy_text(error, cap, "Captured link does not match this download option."); return -1;
    }
    Job job = {.release = *release};
    Transfer owner = {.job = &job, .release = release, .fd = -1, .probe = true,
                      .capture_probe = true, .cancel_signal = cancel};
    CURLcode rc = CURLE_OK;
    if (range_support(&owner, NULL, &rc) != 1) {
        copy_text(error, cap, owner.error[0] ? owner.error : "Provider did not return the expected file byte.");
        return -1;
    }
    bool strong = strong_etag(owner.etag);
    if (!strong && (!owner.modified[0] || curl_getdate(owner.modified, NULL) < 0)) {
        copy_text(error, cap, "The provider did not supply a stable file identity. Nothing was queued.");
        return -1;
    }
    copy_text(etag, 256, strong ? owner.etag : "");
    copy_text(modified, 128, owner.modified);
    return 0;
}
static CURLcode run_ranges(Transfer *owner, struct curl_slist *conditions) {
    /* Independent bounded requests on one network worker; a separate writer
     * coalesces their buffers and owns all writes/checkpoints. */
    CURLM *multi = curl_multi_init();
    unsigned count = owner->job->range_count;
    CURL *handles[ORBIT_MAX_RANGES] = {0};
    bool added[ORBIT_MAX_RANGES] = {false};
    Transfer parts[ORBIT_MAX_RANGES] = {0};
    CURLcode result = CURLE_OK;
    if (!multi) {
        copy_text(owner->error, sizeof owner->error, "Could not initialize parallel transfer.");
        return CURLE_FAILED_INIT;
    }
    curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, (long)count);
    curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS, (long)count);
    curl_multi_setopt(multi, CURLMOPT_PIPELINING, 0L);
    for (unsigned i = 0; i < count; i++) {
        int64_t base = range_boundary(transfer_span(owner), count, i);
        int64_t limit = range_boundary(transfer_span(owner), count, i + 1);
        parts[i] = (Transfer){.job = owner->job,
                              .release = owner->release,
                              .fd = owner->fd,
                              .owner = owner,
                              .segment = i,
                              .offset = base + owner->live[i],
                              .limit = limit};
        if (parts[i].offset == limit)
            continue;
        handles[i] = request(&parts[i]);
        if (!handles[i]) {
            result = CURLE_FAILED_INIT;
            goto cleanup;
        }
        set_range(handles[i], parts[i].offset, limit, conditions);
        if (curl_multi_add_handle(multi, handles[i]) != CURLM_OK) {
            result = CURLE_FAILED_INIT;
            goto cleanup;
        }
        added[i] = true;
    }
    for (;;) {
        if (controlled(owner) || owner->error[0]) {
            result = CURLE_ABORTED_BY_CALLBACK;
            break;
        }
        int running = 0;
        CURLMcode rc = curl_multi_perform(multi, &running);
        if (rc != CURLM_OK) {
            result = CURLE_FAILED_INIT;
            break;
        }
        int remaining;
        CURLMsg *message;
        while ((message = curl_multi_info_read(multi, &remaining))) {
            if (message->msg != CURLMSG_DONE)
                continue;
            for (unsigned i = 0; i < count; i++) {
                Transfer *part = &parts[i];
                if (message->easy_handle != handles[i])
                    continue;
                if (message->data.result != CURLE_OK || !part->accepted ||
                    part->offset + part->written != part->limit) {
                    if (part->retry_at > owner->retry_at)
                        owner->retry_at = part->retry_at;
                    /* A sibling cancelled after a failure must not replace the
                     * provider's throttling status with its callback error. */
                    if (result != CURLE_OK && part->status != 429 && part->status != 503)
                        continue;
                    result = message->data.result == CURLE_OK
                                 ? (part->accepted ? CURLE_PARTIAL_FILE : CURLE_RANGE_ERROR)
                                 : message->data.result;
                    owner->status = part->status;
                    if (!owner->error[0]) {
                        if (part->error[0])
                            copy_text(owner->error, sizeof owner->error, part->error);
                        else
                            snprintf(owner->error, sizeof owner->error,
                                     "Transfer failed (HTTP %ld, %s).", part->status,
                                     curl_easy_strerror(result));
                    }
                }
            }
        }
        if (result != CURLE_OK || !running)
            break;
        if (curl_multi_poll(multi, NULL, 0, 200, NULL) != CURLM_OK) {
            result = CURLE_FAILED_INIT;
            break;
        }
    }
cleanup:
    for (unsigned i = 0; i < count; i++) {
#ifdef ORBIT_BENCHMARK
        if (handles[i] && owner->metrics) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddItemToArray(owner->metrics, m);
            cJSON_AddNumberToObject(m, "range", i);
            cJSON_AddNumberToObject(m, "bytes", (double)parts[i].written);
            cJSON_AddNumberToObject(m, "httpStatus", parts[i].status);
            cJSON_AddBoolToObject(m, "validated", parts[i].accepted);
            cJSON_AddNumberToObject(m, "socketBufferBefore", parts[i].socket_before);
            cJSON_AddNumberToObject(m, "socketBufferAfter", parts[i].socket_after);
            cJSON_AddNumberToObject(m, "socketOptionError", parts[i].socket_error);
            double value = 0;
            curl_easy_getinfo(handles[i], CURLINFO_NAMELOOKUP_TIME, &value);
            cJSON_AddNumberToObject(m, "dnsSeconds", value);
            curl_easy_getinfo(handles[i], CURLINFO_CONNECT_TIME, &value);
            cJSON_AddNumberToObject(m, "connectSeconds", value);
            curl_easy_getinfo(handles[i], CURLINFO_STARTTRANSFER_TIME, &value);
            cJSON_AddNumberToObject(m, "firstByteSeconds", value);
            char *url = NULL, *host = NULL;
            CURLU *parsed = curl_url();
            curl_easy_getinfo(handles[i], CURLINFO_EFFECTIVE_URL, &url);
            if (parsed && url && !curl_url_set(parsed, CURLUPART_URL, url, 0) &&
                !curl_url_get(parsed, CURLUPART_HOST, &host, 0))
                cJSON_AddStringToObject(m, "server", host);
            curl_free(host); curl_url_cleanup(parsed);
        }
#endif
        if (added[i])
            curl_multi_remove_handle(multi, handles[i]);
        if (handles[i])
            curl_easy_cleanup(handles[i]);
    }
    curl_multi_cleanup(multi);
    if (result != CURLE_OK && !owner->error[0])
        snprintf(owner->error, sizeof owner->error, "Transfer interrupted: %s.",
                 curl_easy_strerror(result));
    return result;
}
#ifdef ORBIT_BENCHMARK
static void sample_hash(int fd, int64_t bytes, cJSON *out) {
    EVP_MD_CTX *ctx=EVP_MD_CTX_new();
    if (!ctx) return;
    bool ok=EVP_DigestInit_ex(ctx,EVP_sha256(),NULL)==1;
    unsigned char block[65536], hash[32];
    int64_t offset=0;
    while(ok && offset<bytes) {
        size_t amount=bytes-offset<(int64_t)sizeof block ? (size_t)(bytes-offset) : sizeof block;
        ssize_t n=pread(fd,block,amount,offset);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) { ok=false; break; }
        ok=EVP_DigestUpdate(ctx,block,(size_t)n)==1; offset+=n;
    }
    unsigned length=0;
    if(ok) ok=EVP_DigestFinal_ex(ctx,hash,&length)==1 && length==32;
    EVP_MD_CTX_free(ctx);
    if(ok) {
        char hex[65];
        for(unsigned i=0;i<32;i++) snprintf(hex+2*i,3,"%02x",hash[i]);
        cJSON_AddStringToObject(out,"sampleSha256",hex);
    }
}
/* Bounded prefix sampling with the production request/validation/range engine,
 * writer and state checkpoints. Only the isolated benchmark ELF exposes this.
 * The caller owns an exclusive temporary fd and an isolated Orbit state directory.
 * release.size remains the real remote size for HTTP validation; job.total is
 * the bounded local prefix so tail ranges never create a game-sized sparse file. */
cJSON *transfer_benchmark(Job *j, int fd, unsigned count, int64_t bytes, unsigned duration,
                          long socket_buffer, long curl_buffer) {
    cJSON *out = cJSON_CreateObject();
    Transfer t = {.job=j, .release=&j->release, .fd=fd, .limit=j->release.size,
                  .length=-1, .probe=true, .socket_buffer=socket_buffer, .curl_buffer=curl_buffer};
    CURLcode rc = CURLE_OK;
    struct curl_slist *conditions = NULL;
    double began = seconds();
    if ((count != 2 && count != 4 && count != 8 && count != 16) || bytes < 1024*1024 || bytes > 512LL*1024*1024 ||
        bytes >= j->release.size || duration < 1 || duration > 90 ||
        (socket_buffer && socket_buffer != 256*1024 && socket_buffer != 1024*1024) ||
        (curl_buffer && curl_buffer != 256*1024) || !provider_supported(&j->release)) {
        copy_text(t.error, sizeof t.error, "Invalid bounded benchmark configuration.");
        goto finish_benchmark;
    }
    if (!strcmp(j->release.source, "vikingfile")) {
        if (range_support(&t, NULL, &rc) != 1) goto finish_benchmark;
    } else {
        CURL *c = request(&t);
        if (!c) { rc = CURLE_FAILED_INIT; goto finish_benchmark; }
        curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
        rc = curl_easy_perform(c);
        curl_easy_cleanup(c);
        if (rc != CURLE_OK || t.status != 200) goto finish_benchmark;
    }
    if (t.length != j->release.size || !strong_etag(t.etag)) {
        copy_text(t.error, sizeof t.error, "Benchmark requires the expected file size and strong ETag.");
        goto finish_benchmark;
    }
    copy_text(j->etag, sizeof j->etag, t.etag);
    char identity[300]; snprintf(identity, sizeof identity, "If-Match: %s", t.etag);
    conditions = curl_slist_append(NULL, identity);
    if (!conditions) { rc = CURLE_OUT_OF_MEMORY; goto finish_benchmark; }
    if (!strcmp(j->release.source, "archive") && range_support(&t, conditions, &rc) != 1)
        goto finish_benchmark;
    j->range_count = count; j->segmented = true; j->total = bytes;
    t.sample_span = bytes; t.probe = false;
    if (fsync(fd) || state_save_locked()) {
        copy_text(t.error, sizeof t.error, "Benchmark state could not be saved.");
        goto finish_benchmark;
    }
    cJSON_AddNumberToObject(out, "preflightSeconds", seconds() - began);
    t.started = seconds(); t.deadline = t.started + duration;
    t.writer = transfer_writer_start(j, fd, t.started, t.error, sizeof t.error);
    if (!t.writer) goto finish_benchmark;
    t.metrics = cJSON_AddArrayToObject(out, "ranges");
    t.speed_samples = cJSON_AddArrayToObject(out, "speedSamples");
    t.last_sample = t.started;
    cJSON *initial_sample = cJSON_CreateObject();
    cJSON_AddNumberToObject(initial_sample, "seconds", 0);
    cJSON_AddNumberToObject(initial_sample, "bytes", 0);
    cJSON_AddItemToArray(t.speed_samples, initial_sample);
    rc = run_ranges(&t, conditions);
    /* curl's millisecond timeout is rounded down when handles are prepared. */
    cJSON_AddBoolToObject(out, "timeLimitReached", seconds() >= t.deadline ||
        (rc == CURLE_OPERATION_TIMEDOUT && seconds() + .01 >= t.deadline));
    cJSON_AddNumberToObject(out, "networkSeconds", seconds() - t.started);
    cJSON *last_sample = cJSON_CreateObject();
    cJSON_AddNumberToObject(last_sample, "seconds", seconds() - t.started);
    cJSON_AddNumberToObject(last_sample, "bytes", (double)t.enqueued);
    cJSON_AddItemToArray(t.speed_samples, last_sample);
    bool writer_ok = transfer_writer_finish(t.writer, t.error, sizeof t.error) == 0;
    cJSON_AddBoolToObject(out, "writerSucceeded", writer_ok);
    if (!writer_ok) rc = CURLE_WRITE_ERROR;
    t.writer = NULL;
    cJSON_AddNumberToObject(out, "transferSeconds", seconds() - t.started);
    cJSON_AddNumberToObject(out, "MBps", j->received / (seconds() - t.started) / 1000000.0);
    if(rc==CURLE_OK && j->received==bytes) sample_hash(fd,bytes,out);
finish_benchmark:
    curl_slist_free_all(conditions);
    cJSON_AddStringToObject(out, "error", t.error);
    cJSON_AddNumberToObject(out, "curlCode", rc);
    cJSON_AddNumberToObject(out, "preflightStatus", t.status);
    cJSON_AddNumberToObject(out, "retryAt", (double)t.retry_at);
    cJSON_AddNumberToObject(out, "bytes", (double)j->received);
    cJSON_AddNumberToObject(out, "connections", count);
    cJSON_AddNumberToObject(out, "socketBufferRequested", socket_buffer);
    cJSON_AddNumberToObject(out, "curlBufferRequested", curl_buffer);
    cJSON_AddNumberToObject(out, "peakBufferedBytes", (double)j->transfer_stats.peak_buffered);
    cJSON_AddNumberToObject(out, "bufferWaitSeconds", j->transfer_stats.buffer_wait_seconds);
    cJSON_AddNumberToObject(out, "writeSeconds", j->transfer_stats.write_seconds);
    cJSON_AddNumberToObject(out, "syncSeconds", j->transfer_stats.sync_seconds);
    cJSON_AddNumberToObject(out, "writeCalls", (double)j->transfer_stats.write_calls);
    cJSON_AddNumberToObject(out, "syncCalls", (double)j->transfer_stats.sync_calls);
    return out;
}
#endif
static bool checksum(Transfer *t) {
    if (!t->release->sha256[0])
        return true;
    if (lseek(t->fd, 0, SEEK_SET) < 0)
        return false;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx)
        return false;
    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1;
    static unsigned char block[65536]; /* One worker thread: kept off its stack. */
    unsigned char digest[32] = {0};
    unsigned length = 0;
    while (ok) {
        ssize_t n = read(t->fd, block, sizeof block);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            ok = false;
            break;
        }
        if (!n)
            break;
        if (controlled(t)) {
            ok = false;
            break;
        }
        ok = EVP_DigestUpdate(ctx, block, (size_t)n) == 1;
    }
    if (ok)
        ok = EVP_DigestFinal_ex(ctx, digest, &length) == 1;
    EVP_MD_CTX_free(ctx);
    char hex[65];
    for (unsigned i = 0; i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    return ok && length == 32 && !strcmp(hex, t->release->sha256);
}
static bool retryable(long status, CURLcode rc) {
    return status == 429 || status == 503 || status == 502 || status == 504 ||
           rc == CURLE_COULDNT_CONNECT || rc == CURLE_COULDNT_RESOLVE_HOST ||
           rc == CURLE_OPERATION_TIMEDOUT || rc == CURLE_RECV_ERROR || rc == CURLE_PARTIAL_FILE;
}
static void run_job(Job *j) {
    Release resolved = j->release;
    Release *r = &resolved;
    Transfer t = {.job = j,
                  .release = r,
                  .fd = -1,
                  .length = -1,
                  .start = -1,
                  .end = -1,
                  .total = -1,
                  .limit = r->size,
                  .probe = true};
    memcpy(t.live, j->range_received, sizeof t.live);
    int dir = -1;
    char partial[192];
    snprintf(partial, sizeof partial, "%s.part", j->filename);
    CURLcode rc = CURLE_OK;
    CURL *c = NULL;
    struct curl_slist *conditions = NULL;
    bool done = false, debrid_waiting = false;
    dir = storage_open(j);
    if (dir < 0 || storage_check_writable(dir)) {
        copy_text(t.error, sizeof t.error, "Destination is missing or not writable.");
        goto finish;
    }
    struct stat st;
    if (fstatat(dir, j->filename, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        copy_text(t.error, sizeof t.error,
                  "Destination file already exists. It will not be overwritten.");
        goto finish;
    }
    t.fd = openat(dir, partial, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (t.fd < 0 || fstat(t.fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1) {
        copy_text(t.error, sizeof t.error, "Cannot open a safe partial file.");
        goto finish;
    }
    t.offset = st.st_size;
    int64_t required_length = st.st_size;
    if (j->segmented) {
        t.offset = required_length = 0;
        for (unsigned i = 0; i < j->range_count; i++) {
            t.offset += t.live[i];
            int64_t end = range_boundary(r->size, j->range_count, i) + t.live[i];
            if (t.live[i] && end > required_length) required_length = end;
        }
    }
    if (st.st_size < required_length || st.st_size > r->size || t.offset < 0) {
        copy_text(t.error, sizeof t.error,
                  "Partial file size is invalid. Delete the partial file before retrying.");
        goto finish;
    }
    struct statvfs fs;
    uint64_t disk_remaining = (uint64_t)(r->size - t.offset);
    if (j->segmented && st.st_blocks > 0) {
        uint64_t allocated = (uint64_t)st.st_blocks * 512;
        uint64_t unallocated = allocated >= (uint64_t)r->size ? 0 : (uint64_t)r->size - allocated;
        if (unallocated < disk_remaining)
            disk_remaining = unallocated;
    }
    if (fstatvfs(t.fd, &fs) || (uint64_t)fs.f_bavail * (fs.f_frsize ? fs.f_frsize : fs.f_bsize) <
                                   disk_remaining + 16 * 1024 * 1024) {
        copy_text(t.error, sizeof t.error, "Not enough free space to finish this download.");
        goto finish;
    }
    if (!strcmp(j->delivery, "torbox")) {
        pthread_mutex_lock(&orbit.mutex);
        copy_text(j->delivery_phase, sizeof j->delivery_phase, "Preparing with TorBox");
        pthread_mutex_unlock(&orbit.mutex);
        int prepared = debrid_resolve(j, r->url, sizeof r->url, &t.retry_at, t.error, sizeof t.error);
        if (prepared != 1) { debrid_waiting = prepared == 0; goto finish; }
        pthread_mutex_lock(&orbit.mutex);
        copy_text(j->delivery_phase, sizeof j->delivery_phase, "Downloading via TorBox");
        pthread_mutex_unlock(&orbit.mutex);
    }
    bool viking = guarded(&t);
    if (viking) {
        /* Storage signatures can be GET-specific. Verify one byte instead of
         * issuing HEAD, and request the /d/ route afresh for each transfer. */
        if (range_support(&t, NULL, &rc) != 1) {
            if (!t.error[0])
                copy_text(t.error, sizeof t.error, "Source did not return a valid byte range.");
            goto finish;
        }
    } else {
        c = request(&t);
        if (!c) {
            copy_text(t.error, sizeof t.error, "Could not initialize HTTPS.");
            goto finish;
        }
        curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
        rc = curl_easy_perform(c);
        curl_easy_cleanup(c);
        c = NULL;
        if (rc != CURLE_OK || t.status != 200) {
            snprintf(t.error, sizeof t.error, "Source check failed (HTTP %ld, %s).", t.status,
                     curl_easy_strerror(rc));
            goto finish;
        }
    }
    if (t.length != r->size) {
        copy_text(t.error, sizeof t.error, "Source size changed. Catalogue needs verification.");
        goto finish;
    }
    bool strong = strong_etag(t.etag);
    if ((t.offset || j->segmented) &&
        ((j->etag[0] && strcmp(j->etag, t.etag)) ||
         (!j->etag[0] && j->modified[0] && strcmp(j->modified, t.modified)) ||
         (!j->etag[0] && !j->modified[0]))) {
        copy_text(
            t.error, sizeof t.error,
            "Source identity cannot be verified for resume. Delete the partial file and retry.");
        goto finish;
    }
    pthread_mutex_lock(&orbit.mutex);
    if (!t.offset) {
        copy_text(j->etag, sizeof j->etag, strong ? t.etag : "");
        copy_text(j->modified, sizeof j->modified, t.modified);
    }
    j->received = t.offset;
    int saved = state_save_locked();
    pthread_mutex_unlock(&orbit.mutex);
    if (saved) {
        copy_text(t.error, sizeof t.error, "Could not persist source identity.");
        goto finish;
    }
    char h[400];
    if (j->etag[0]) {
        snprintf(h, sizeof h, "If-Match: %s", j->etag);
        conditions = curl_slist_append(NULL, h);
    } else if (j->modified[0]) {
        snprintf(h, sizeof h, "If-Unmodified-Since: %s", j->modified);
        conditions = curl_slist_append(NULL, h);
    }
    if ((j->etag[0] || j->modified[0]) && !conditions) {
        copy_text(t.error, sizeof t.error, "Could not prepare source identity check.");
        goto finish;
    }
    if (!j->segmented) {
        unsigned count = ORBIT_MAX_RANGES;
#ifdef ORBIT_TEST
        const char *connections = getenv("ORBIT_TEST_CONNECTIONS");
        if (connections && !strcmp(connections, "2")) count = 2;
#endif
        bool parallel = false;
        if (j->etag[0] && t.offset < range_boundary(r->size, count, 1) &&
            r->size - t.offset >= PARALLEL_MIN_REMAINING) {
            int supported = viking ? 1 : range_support(&t, conditions, &rc);
            if (supported < 0) goto finish;
            parallel = supported == 1;
        }
        /* Persist the layout before starting the writer or writing any tail.
         * Single-connection downloads also use a durable cursor, not st_size. */
        if (fsync(t.fd)) {
            copy_text(t.error, sizeof t.error, "Could not flush the existing partial file.");
            goto finish;
        }
        t.live[0] = t.offset;
        pthread_mutex_lock(&orbit.mutex);
        j->segmented = true;
        j->range_count = parallel ? count : 1;
        j->range_received[0] = t.offset;
        saved = state_save_locked();
        pthread_mutex_unlock(&orbit.mutex);
        if (saved) {
            copy_text(t.error, sizeof t.error, "Could not persist the download ranges.");
            goto finish;
        }
    }
    if (t.offset < r->size) {
        t.probe = false;
        t.started = seconds();
        t.writer = transfer_writer_start(j, t.fd, t.started, t.error, sizeof t.error);
        if (!t.writer) goto finish;
        if (j->range_count > 1) {
            rc = run_ranges(&t, conditions);
            if (rc != CURLE_OK || t.error[0]) goto finish;
        } else {
            c = request(&t);
            if (!c) {
                copy_text(t.error, sizeof t.error, "Could not initialize transfer.");
                goto finish;
            }
            set_range(c, t.offset, r->size, conditions);
            rc = curl_easy_perform(c);
            curl_easy_cleanup(c);
            c = NULL;
            if (rc != CURLE_OK || !t.accepted) {
                if (!t.error[0])
                    snprintf(t.error, sizeof t.error, "Transfer failed (HTTP %ld, %s).", t.status,
                             curl_easy_strerror(rc));
                goto finish;
            }
        }
    }
    if (t.writer) {
        int failed = transfer_writer_finish(t.writer, t.error, sizeof t.error);
        t.writer = NULL;
        if (failed) { rc = CURLE_WRITE_ERROR; goto finish; }
    }
    int64_t durable = 0;
    for (unsigned i = 0; i < j->range_count; i++) durable += j->range_received[i];
    if (fstat(t.fd, &st) || st.st_size != r->size || durable != r->size) {
        copy_text(t.error, sizeof t.error, "Downloaded file size or disk flush failed validation.");
        goto finish;
    }
    pthread_mutex_lock(&orbit.mutex);
    copy_text(j->status, sizeof j->status, "verifying");
    j->speed = 0;
    state_save_locked();
    pthread_mutex_unlock(&orbit.mutex);
    if (!checksum(&t)) {
        copy_text(t.error, sizeof t.error, "Checksum verification failed. Partial file preserved.");
        goto finish;
    }
    if (controlled(&t))
        goto finish;
    if (!storage_matches(j)) {
        copy_text(t.error, sizeof t.error, "Destination disconnected before finalization.");
        goto finish;
    }
    if (linkat(dir, partial, dir, j->filename, 0)) {
        /* exFAT does not support hard links. Under the single worker, recheck before rename. */
        if (errno != EPERM && errno != EOPNOTSUPP && errno != ENOSYS) {
            copy_text(t.error, sizeof t.error,
                      "Could not finalize without overwriting an existing file.");
            goto finish;
        }
        /* O_EXCL reserves the name; remove only our reservation if rename fails. */
        int reserve = openat(dir, j->filename, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (reserve < 0) {
            copy_text(t.error, sizeof t.error, "Destination filename is already in use.");
            goto finish;
        }
        close(reserve);
        if (renameat(dir, partial, dir, j->filename)) {
            unlinkat(dir, j->filename, 0);
            copy_text(t.error, sizeof t.error, "Could not finalize the completed file.");
            goto finish;
        }
    } else
        unlinkat(dir, partial, 0);
    fsync(dir);
    done = true;
finish:
    if (c)
        curl_easy_cleanup(c);
    curl_slist_free_all(conditions);
    if (t.writer) {
        if (transfer_writer_finish(t.writer, t.error, sizeof t.error)) {
            rc = CURLE_WRITE_ERROR;
            t.status = 0; /* A disk failure must not inherit a sibling's HTTP retry. */
        }
        t.writer = NULL;
    }
    if (t.fd >= 0) {
        if (!j->segmented) fsync(t.fd);
        struct stat actual;
        if (!j->segmented && !fstat(t.fd, &actual)) {
            pthread_mutex_lock(&orbit.mutex);
            j->received = actual.st_size;
            pthread_mutex_unlock(&orbit.mutex);
        }
        close(t.fd);
    }
    pthread_mutex_lock(&orbit.mutex);
    j->speed = 0;
    if (done) {
        copy_text(j->status, sizeof j->status, "complete");
        copy_text(j->verification, sizeof j->verification, r->sha256[0] ? "sha256" : "size");
        j->error[0] = 0;
    } else if (j->cancel) {
        copy_text(j->status, sizeof j->status, "cancelled");
        j->error[0] = 0;
        if (j->remove)
            job_delete_partial_locked(j, j->error, sizeof j->error);
    } else if (j->pause || orbit.stop) {
        copy_text(j->status, sizeof j->status, "paused");
        copy_text(j->error, sizeof j->error,
                  source_enabled_locked(r)
                      ? ""
                      : "Source disabled. Enable it in Sources before resuming.");
    } else if (debrid_waiting) {
        copy_text(j->status, sizeof j->status, "queued");
        j->retry_at = t.retry_at; j->error[0] = 0;
    } else if (retryable(t.status, rc) && j->attempts < 3) {
        j->attempts++;
        time_t fallback = time(NULL) + (time_t)(5U << j->attempts);
        j->retry_at = t.retry_at > fallback ? t.retry_at : fallback;
        orbit.host_retry_at = j->retry_at;
        copy_text(j->status, sizeof j->status, "retrying");
        copy_text(j->error, sizeof j->error, t.error);
    } else {
        copy_text(j->status, sizeof j->status, "error");
        copy_text(j->error, sizeof j->error, t.error[0] ? t.error : "Transfer interrupted.");
    }
    state_save_locked();
    diagnostics_event_locked("transfer-end", j, t.status, (int)rc);
    pthread_mutex_unlock(&orbit.mutex);
    if (dir >= 0)
        close(dir);
    OPENSSL_cleanse(resolved.url, sizeof resolved.url);
}
void *download_worker(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&orbit.mutex);
        if (orbit.stop) {
            pthread_mutex_unlock(&orbit.mutex);
            break;
        }
        Job *next = NULL;
        time_t now = time(NULL);
        if (now >= orbit.host_retry_at && !orbit.library_storage_busy)
            for (size_t i = 0; i < orbit.job_count; i++) {
                Job *j = &orbit.jobs[i];
                if ((!strcmp(j->status, "queued") && now >= j->retry_at) ||
                    (!strcmp(j->status, "retrying") && now >= j->retry_at)) {
                    if (!source_enabled_locked(&j->release)) {
                        copy_text(j->status, sizeof j->status, "paused");
                        copy_text(j->error, sizeof j->error,
                                  "Source disabled. Enable it in Sources before resuming.");
                        state_save_locked();
                        continue;
                    }
                    if (!next || j->order < next->order)
                        next = j;
                }
            }
        if (next) {
            copy_text(next->status, sizeof next->status, "downloading");
            next->error[0] = 0;
            state_save_locked();
            diagnostics_event_locked("transfer-start", next, 0, 0);
            pthread_mutex_unlock(&orbit.mutex);
            run_job(next);
        } else {
            struct timespec deadline = {.tv_sec = now + 1, .tv_nsec = 0};
            pthread_cond_timedwait(&orbit.changed, &orbit.mutex, &deadline);
            pthread_mutex_unlock(&orbit.mutex);
        }
    }
    return NULL;
}
