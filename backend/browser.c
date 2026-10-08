#include "orbit.h"
#include "browser_platform.h"
#include "browser_match.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* One user-requested session, held only in memory. The queue receives a validated
 * canonical route, never arbitrary browser bytes or a signed storage URL. */
static struct {
    pthread_t thread;
    pthread_cond_t changed;
    bool ready, stop, pending, busy, thread_started;
    atomic_bool cancel;
    char id[33], state[16], message[256], job_id[33], title[128];
    Release release;
    Storage storage;
} session = {.changed = PTHREAD_COND_INITIALIZER};

static bool available(void) {
#ifdef ORBIT_TEST
    return true;
#else
    return !orbit.desktop;
#endif
}
#ifndef ORBIT_TEST
static time_t monotonic(void) {
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) ? 0 : now.tv_sec;
}
#endif
static void state_locked(const char *state, const char *message) {
    copy_text(session.state, sizeof session.state, state);
    copy_text(session.message, sizeof session.message, message);
}
static bool selected_locked(void) {
    const Release *r = release_find(session.release.id), *p = &session.release;
    return r && source_enabled_locked(r) && r->size == p->size &&
        !strcmp(r->source, p->source) && !strcmp(r->game_id, p->game_id) &&
        !strcmp(r->title_id, p->title_id) && !strcmp(r->filename, p->filename) &&
        !strcmp(r->format, p->format) && !strcmp(r->sha256, p->sha256) &&
        !strcmp(r->url, p->url) && !strcmp(r->browser_url, p->browser_url);
}
static bool active(void) {
    pthread_mutex_lock(&orbit.mutex);
    bool ok = !session.stop && !orbit.stop && !atomic_load(&session.cancel) && selected_locked();
    pthread_mutex_unlock(&orbit.mutex);
    return ok;
}
static void delay(void) {
    struct timespec t = {.tv_nsec = 100000000};
    nanosleep(&t, NULL);
}
#ifndef ORBIT_TEST
static bool native_active(void *context) {
    return active() && probe_target_active(context);
}
#endif
static bool capture(Release *r, char *error, size_t cap) {
#ifdef ORBIT_TEST
    /* Compile-time-only simulator: not exposed through the API or in ELF builds. */
    const char *candidate = getenv("ORBIT_TEST_CAPTURE_URL");
    const char *wait = getenv("ORBIT_TEST_CAPTURE_DELAY_MS");
    unsigned steps = wait ? (unsigned)strtoul(wait, NULL, 10) / 100 : 1;
    if (steps > 300) steps = 300;
    for (unsigned i = 0; i < steps && active(); i++) delay();
    if (!active()) return false;
    if (!candidate || !provider_capture_url(r, candidate)) {
        copy_text(error, cap, "No matching download route was found."); return false;
    }
    copy_text(r->url, sizeof r->url, candidate);
    return true;
#else
    if (!active()) return false;
    if (probe_browser_open(r->browser_url)) {
        copy_text(error, cap, "The PS5 browser could not open. Return to Orbit and try again.");
        return false;
    }
    ProbeTarget target = {.cancel = &session.cancel, .deadline = monotonic() + 120};
    int found = 0;
    for (unsigned i = 0; i < 100 && active(); i++) {
        found = probe_target_find(&target);
        if (found) break;
        delay();
    }
    if (found != 1) {
        copy_text(error, cap, "The PS5 browser is unavailable or cannot be identified safely.");
        return false;
    }
    BrowserMatch match = {.release = r};
    ProbeReader reader = {.context = &target, .read = probe_target_read,
        .active = native_active, .match = browser_match, .match_context = &match,
        .remaining = 128U * 1024U * 1024U};
    while (native_active(&target) && reader.remaining) {
        ProbeResult result = probe_target_scan(&target, &reader, "https://vikingfile.com/d/");
        if (result == PROBE_FOUND && active()) {
            copy_text(r->url, sizeof r->url, match.url);
            memset(&match, 0, sizeof match);
            return true;
        }
        if (result != PROBE_ABSENT) break;
        delay();
    }
    copy_text(error, cap, target.identity_changed
        ? "The PS5 browser restarted. Return to Orbit and try again."
        : "No matching download was found within the verification window. Return to Orbit to try again.");
    return false;
#endif
}
static void *worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&orbit.mutex);
    while (!session.stop) {
        while (!session.pending && !session.stop)
            pthread_cond_wait(&session.changed, &orbit.mutex);
        if (session.stop) break;
        session.pending = false;
        Release resolved = session.release;
        state_locked("waiting", "On the Vikingfile page, complete any verification and select Download. Then return to Orbit Store and open Downloads. Orbit checks the file before adding it to your queue.");
        pthread_mutex_unlock(&orbit.mutex);
        char error[256] = {0}, etag[256] = {0}, modified[128] = {0};
        bool ok = capture(&resolved, error, sizeof error);
        pthread_mutex_lock(&orbit.mutex);
        ok = ok && !session.stop && !atomic_load(&session.cancel) && selected_locked();
        if (ok) state_locked("validating", "Checking the selected file before adding it to Downloads…");
        pthread_mutex_unlock(&orbit.mutex);
        if (ok) ok = !transfer_validate_capture(&resolved, &session.cancel, etag, modified, error, sizeof error);
        pthread_mutex_lock(&orbit.mutex);
        if (session.stop || atomic_load(&session.cancel)) {
            state_locked("cancelled", "Verification cancelled. No download was added.");
        } else if (!selected_locked()) {
            state_locked("failed", "The source or catalogue option changed. Start verification again.");
        } else if (ok) {
            Job *job = NULL;
            if (job_create_captured_locked(&resolved, &session.storage, etag, modified,
                                          error, sizeof error, &job) == 201) {
                copy_text(session.job_id, sizeof session.job_id, job->id);
                state_locked("queued", "Your verified file has been added to Downloads.");
            } else state_locked("failed", error);
        } else state_locked("failed", error[0] ? error : "Verification stopped. Return to Orbit to try again.");
        /* No pending verification survives a restart. No browser bytes are saved. */
        memset(&resolved, 0, sizeof resolved);
        memset(&session.release, 0, sizeof session.release);
        session.busy = false;
    }
    pthread_mutex_unlock(&orbit.mutex);
    return NULL;
}
int browser_start(void) {
    pthread_mutex_lock(&orbit.mutex);
    /* Readiness only. No thread or native service until an explicit request. */
    session.ready = true;
    pthread_mutex_unlock(&orbit.mutex);
    return 0;
}
static int start_worker_locked(void) {
    if (session.thread_started) return 0;
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc) return rc;
    rc = pthread_attr_setstacksize(&attr, ORBIT_THREAD_STACK);
    if (!rc) rc = pthread_create(&session.thread, &attr, worker, NULL);
    pthread_attr_destroy(&attr);
    session.thread_started = !rc;
    return rc;
}
void browser_stop(void) {
    pthread_mutex_lock(&orbit.mutex);
    bool join = session.thread_started;
    session.stop = true;
    atomic_store(&session.cancel, true);
    pthread_cond_signal(&session.changed);
    pthread_mutex_unlock(&orbit.mutex);
    if (join) pthread_join(session.thread, NULL);
}
cJSON *browser_status(void) {
    cJSON *o = cJSON_CreateObject();
    pthread_mutex_lock(&orbit.mutex);
    cJSON_AddBoolToObject(o, "available", session.ready && available() && !session.stop);
    cJSON_AddBoolToObject(o, "active", session.busy);
    cJSON_AddStringToObject(o, "id", session.id);
    cJSON_AddStringToObject(o, "state", session.state[0] ? session.state : "idle");
    cJSON_AddStringToObject(o, "message", session.message);
    cJSON_AddStringToObject(o, "jobId", session.job_id);
    cJSON_AddStringToObject(o, "title", session.title);
    pthread_mutex_unlock(&orbit.mutex);
    return o;
}
int browser_action(const cJSON *input, char *error, size_t cap) {
    int code = 202;
    const char *action = json_text(input, "action");
    pthread_mutex_lock(&orbit.mutex);
    if (!strcmp(action, "cancel")) {
        if (!session.busy || strcmp(json_text(input, "id"), session.id)) {
            copy_text(error, cap, "This verification session is no longer active."); code = 409;
        } else {
            atomic_store(&session.cancel, true);
            state_locked("cancelled", "Verification cancelled. No download was added.");
        }
    } else if (strcmp(action, "start")) {
        copy_text(error, cap, "Unknown browser action."); code = 400;
    } else if (!session.ready || session.stop || !available()) {
        copy_text(error, cap, "Provider verification is available on the PS5 only."); code = 409;
    } else if (session.busy) {
        copy_text(error, cap, "Finish or cancel the current verification first."); code = 409;
    } else {
        Release *r = release_find(json_text(input, "releaseId"));
        Storage drives[ORBIT_MAX_STORAGE], *s = NULL;
        size_t count = storage_list(drives);
        for (size_t i = 0; i < count; i++)
            if (!strcmp(drives[i].id, json_text(input, "storageId"))) s = &drives[i];
        if (!r || !provider_browser_supported(r)) {
            copy_text(error, cap, "This option has no supported provider browser page."); code = 400;
        } else if (!s) {
            copy_text(error, cap, "Reconnect the selected drive first."); code = 409;
        } else if ((code = job_check_capture_locked(r, s, error, cap)) == 200) {
            if (start_worker_locked()) {
                copy_text(error, cap, "Provider verification could not start. Try again.");
                state_locked("failed", error);
                code = 503;
                goto done;
            }
            session.release = *r; session.storage = *s;
            copy_text(session.title, sizeof session.title, r->title);
            random_hex(session.id, 16);
            session.job_id[0] = 0;
            session.busy = session.pending = true;
            atomic_store(&session.cancel, false);
            state_locked("opening", "Opening Vikingfile on your PS5. Select Download on that page, then return to Orbit Store to follow progress.");
            pthread_cond_signal(&session.changed);
            code = 202;
        }
    }
done:
    pthread_mutex_unlock(&orbit.mutex);
    return code;
}

void browser_source_changed_locked(void) {
    if (session.busy && !source_enabled_locked(&session.release)) atomic_store(&session.cancel, true);
}
