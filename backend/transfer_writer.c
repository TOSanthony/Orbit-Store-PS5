#include "transfer_writer.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    unsigned char *data;
    size_t head, used;
    int64_t written;
} Buffer;
struct TransferWriter {
    Job *job;
    int fd;
    unsigned count;
    size_t capacity, buffered, peak;
    unsigned char *memory, *block;
    Buffer ranges[ORBIT_MAX_RANGES];
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    atomic_bool failed;
    bool closing;
    char error[256];
    double started, last_checkpoint, wait_seconds, write_seconds, sync_seconds;
    int64_t initial, written;
    uint64_t write_calls, sync_calls;
#ifdef ORBIT_TEST
    unsigned sync_delay_ms, write_delay_ms;
    int64_t fail_write_after, fail_sync_after;
#endif
};
static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static void wait_briefly(TransferWriter *w) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 50 * 1000 * 1000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    pthread_cond_timedwait(&w->changed, &w->mutex, &ts);
}
#ifdef ORBIT_TEST
static void test_delay(unsigned ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}
static int64_t test_number(const char *name, int64_t fallback) {
    const char *value = getenv(name);
    if (!value || !*value) return fallback;
    char *end;
    long long n = strtoll(value, &end, 10);
    return *end || n < 0 ? fallback : n;
}
#endif
static void fail(TransferWriter *w, const char *message) {
    if (!w->error[0]) copy_text(w->error, sizeof w->error, message);
    atomic_store(&w->failed, true);
    pthread_mutex_lock(&w->mutex);
    pthread_cond_broadcast(&w->changed);
    pthread_mutex_unlock(&w->mutex);
}
static int checkpoint(TransferWriter *w) {
    double begin = now();
    w->sync_calls++;
#ifdef ORBIT_TEST
    test_delay(w->sync_delay_ms);
    if (w->fail_sync_after >= 0 && (int64_t)w->sync_calls > w->fail_sync_after) {
        w->sync_seconds += now() - begin;
        fail(w, "Downloaded bytes could not be flushed to disk.");
        return -1;
    }
#endif
    int result;
    do { result = fsync(w->fd); } while (result && errno == EINTR);
    w->sync_seconds += now() - begin;
    if (result) {
        fail(w, "Downloaded bytes could not be flushed to disk.");
        return -1;
    }
    /* This thread is the ONLY file writer. Its cursors therefore describe
     * precisely the bytes covered by the preceding fsync. No buffer mutex is
     * held during disk I/O or queue-state persistence: the network can enqueue. */
    pthread_mutex_lock(&orbit.mutex);
    for (unsigned i = 0; i < w->count; i++) w->job->range_received[i] = w->ranges[i].written;
    result = state_save_locked();
    pthread_mutex_unlock(&orbit.mutex);
    if (result) { fail(w, "Queue state could not be saved."); return -1; }
    w->last_checkpoint = now();
    return 0;
}
static void publish_progress(TransferWriter *w) {
    pthread_mutex_lock(&orbit.mutex);
    w->job->received = w->written;
    double elapsed = now() - w->started;
    w->job->speed = elapsed > 0 ? (w->written - w->initial) / elapsed : 0;
    pthread_mutex_unlock(&orbit.mutex);
}
static unsigned ready_range(const TransferWriter *w, unsigned next) {
    unsigned first = next;
    bool found = false;
    for (unsigned i = 0; i < w->count; i++) {
        unsigned index = (next + i) % w->count;
        if (w->ranges[index].used >= TRANSFER_WRITE_BYTES) return index;
        if (!found && w->ranges[index].used) { first = index; found = true; }
    }
    return first;
}
static void *writer_main(void *arg) {
    TransferWriter *w = arg;
    unsigned next = 0;
    double last_storage = 0;
    bool dirty = false;
    for (;;) {
        pthread_mutex_lock(&w->mutex);
        while (!w->buffered && !w->closing) {
            wait_briefly(w);
            if (dirty && now() - w->last_checkpoint >= 2) break;
        }
        size_t size = 0;
        unsigned index = ready_range(w, next);
        Buffer *b = &w->ranges[index];
        /* Coalesce short callbacks, but never wait on a slow range when another
         * already has a full write ready. Blocking one curl callback for buffer
         * space also blocks its sibling handles on the shared network thread. */
        double coalesce_until = now() + .05;
        while (b->used && b->used < TRANSFER_WRITE_BYTES && !w->closing && now() < coalesce_until) {
            unsigned ready = ready_range(w, next);
            if (w->ranges[ready].used >= TRANSFER_WRITE_BYTES) {
                index = ready; b = &w->ranges[index]; break;
            }
            wait_briefly(w);
        }
        size = b->used < TRANSFER_WRITE_BYTES ? b->used : TRANSFER_WRITE_BYTES;
        if (size) {
            size_t first = w->capacity - b->head;
            if (first > size) first = size;
            memcpy(w->block, b->data + b->head, first);
            memcpy(w->block + first, b->data, size - first);
            b->head = (b->head + size) % w->capacity;
            b->used -= size;
            w->buffered -= size;
            pthread_cond_broadcast(&w->changed);
        }
        bool finished = w->closing && !size && !w->buffered;
        pthread_mutex_unlock(&w->mutex);
        if (finished) break;
        if (size) {
            if (now() - last_storage >= 1) {
                last_storage = now();
                if (!storage_matches(w->job)) {
                    fail(w, "Destination drive disconnected. Reconnect the original drive.");
                    return NULL;
                }
            }
            double begin = now();
            size_t sent = 0;
            while (sent < size) {
                ssize_t result;
#ifdef ORBIT_TEST
                test_delay(w->write_delay_ms);
                if (w->fail_write_after >= 0 && w->written - w->initial >= w->fail_write_after) {
                    errno = ENOSPC; result = -1;
                } else
#endif
                result = pwrite(w->fd, w->block + sent, size - sent,
                                (off_t)(range_boundary(w->job->total, w->count, index) + b->written));
                w->write_calls++;
                if (result < 0 && errno == EINTR) continue;
                if (result <= 0) {
                    w->write_seconds += now() - begin;
                    fail(w, "Destination write failed. Partial file preserved.");
                    /* A failed write does not make previously successful writes
                     * durable. Leave the last saved cursor unchanged. */
                    return NULL;
                }
                sent += (size_t)result;
                b->written += result;
                w->written += result;
            }
            w->write_seconds += now() - begin;
            dirty = true;
            next = (index + 1) % w->count;
            publish_progress(w);
        }
        if (dirty && now() - w->last_checkpoint >= 2) {
            if (checkpoint(w)) return NULL;
            dirty = false;
        }
    }
    /* Always finish on a flush, even on pause or an interrupted HTTP request. */
    checkpoint(w);
    return NULL;
}
TransferWriter *transfer_writer_start(Job *job, int fd, double started, char *error, size_t cap) {
    TransferWriter *w = calloc(1, sizeof *w);
    if (!w) goto failure;
    atomic_init(&w->failed, false);
    w->job = job; w->fd = fd; w->count = job->range_count;
    if (!w->count || w->count > ORBIT_MAX_RANGES) goto failure;
    w->capacity = TRANSFER_BUFFER_BYTES / w->count;
    w->memory = malloc(TRANSFER_BUFFER_BYTES);
    w->block = malloc(TRANSFER_WRITE_BYTES);
    if (!w->memory || !w->block) goto failure;
    for (unsigned i = 0; i < w->count; i++) {
        w->ranges[i].data = w->memory + i * w->capacity;
        w->ranges[i].written = job->range_received[i];
        w->initial += job->range_received[i];
    }
    w->written = w->initial;
    w->started = started; w->last_checkpoint = now();
#ifdef ORBIT_TEST
    w->sync_delay_ms = (unsigned)test_number("ORBIT_TEST_WRITER_SYNC_MS", 0);
    w->write_delay_ms = (unsigned)test_number("ORBIT_TEST_WRITER_WRITE_MS", 0);
    w->fail_write_after = test_number("ORBIT_TEST_WRITER_FAIL_WRITE_AFTER", -1);
    w->fail_sync_after = test_number("ORBIT_TEST_WRITER_FAIL_SYNC_AFTER", -1);
#endif
    if (pthread_mutex_init(&w->mutex, NULL)) goto failure;
    if (pthread_cond_init(&w->changed, NULL)) { pthread_mutex_destroy(&w->mutex); goto failure; }
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (!rc) {
        rc = pthread_attr_setstacksize(&attr, ORBIT_THREAD_STACK);
        if (!rc) rc = pthread_create(&w->thread, &attr, writer_main, w);
        pthread_attr_destroy(&attr);
    }
    if (!rc) return w;
    pthread_cond_destroy(&w->changed);
    pthread_mutex_destroy(&w->mutex);
failure:
    if (w) { free(w->memory); free(w->block); free(w); }
    copy_text(error, cap, "Could not start the buffered disk writer.");
    return NULL;
}
bool transfer_writer_failed(const TransferWriter *w) { return w && atomic_load(&w->failed); }
int transfer_writer_enqueue(TransferWriter *w, unsigned range, const void *data, size_t size) {
    if (!w || range >= w->count || size > w->capacity) return -1;
    Buffer *b = &w->ranges[range];
    pthread_mutex_lock(&w->mutex);
    double begin = now();
    bool waited = false;
    while (w->capacity - b->used < size && !w->closing && !atomic_load(&w->failed) &&
           !atomic_load(&orbit.stop) && !atomic_load(&w->job->pause) && !atomic_load(&w->job->cancel)) {
        waited = true;
        wait_briefly(w);
    }
    if (waited) w->wait_seconds += now() - begin;
    if (w->closing || atomic_load(&w->failed) || atomic_load(&orbit.stop) ||
        atomic_load(&w->job->pause) || atomic_load(&w->job->cancel)) {
        pthread_mutex_unlock(&w->mutex);
        return -1;
    }
    size_t tail = (b->head + b->used) % w->capacity, first = w->capacity - tail;
    if (first > size) first = size;
    memcpy(b->data + tail, data, first);
    memcpy(b->data, (const unsigned char *)data + first, size - first);
    b->used += size;
    w->buffered += size;
    if (w->buffered > w->peak) w->peak = w->buffered;
    pthread_cond_signal(&w->changed);
    pthread_mutex_unlock(&w->mutex);
    return 0;
}
int transfer_writer_finish(TransferWriter *w, char *error, size_t cap) {
    pthread_mutex_lock(&w->mutex);
    w->closing = true;
    pthread_cond_broadcast(&w->changed);
    pthread_mutex_unlock(&w->mutex);
    pthread_join(w->thread, NULL);
    int result = atomic_load(&w->failed) ? -1 : 0;
    if (result) copy_text(error, cap, w->error);
    pthread_mutex_lock(&orbit.mutex);
    w->job->received = 0;
    for (unsigned i = 0; i < w->count; i++) w->job->received += w->job->range_received[i];
    w->job->transfer_stats.peak_buffered = w->peak;
    w->job->transfer_stats.write_calls = w->write_calls;
    w->job->transfer_stats.sync_calls = w->sync_calls;
    w->job->transfer_stats.buffer_wait_seconds = w->wait_seconds;
    w->job->transfer_stats.write_seconds = w->write_seconds;
    w->job->transfer_stats.sync_seconds = w->sync_seconds;
    pthread_mutex_unlock(&orbit.mutex);
    pthread_cond_destroy(&w->changed);
    pthread_mutex_destroy(&w->mutex);
    free(w->memory); free(w->block); free(w);
    return result;
}
