#include "orbit.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

unsigned source_flag(const char *id) {
    if (!strcmp(id, "archive"))
        return ORBIT_SOURCE_ARCHIVE;
    if (!strcmp(id, "vikingfile"))
        return ORBIT_SOURCE_VIKINGFILE;
    return 0;
}

bool source_enabled_locked(const Release *r) {
    if (!r)
        return false;
    /* The pinned connection diagnostic is not a game source. */
    if (!strcmp(r->id, "orbit-network-check"))
        return true;
    return orbit.sources.notice_version == ORBIT_SOURCE_NOTICE_VERSION &&
           orbit.sources.acknowledged_at > 0 && (orbit.sources.enabled & source_flag(r->source));
}

cJSON *sources_json_locked(void) {
    static const struct {
        const char *id, *label;
        bool ready;
    } providers[] = {{"vikingfile", "Vikingfile", true}, {"archive", "Archive.org", true}};
    cJSON *o = cJSON_CreateObject(), *enabled = cJSON_AddArrayToObject(o, "enabled"),
          *options = cJSON_AddArrayToObject(o, "options");
    bool acknowledged = orbit.sources.notice_version == ORBIT_SOURCE_NOTICE_VERSION &&
                        orbit.sources.acknowledged_at > 0;
    cJSON_AddBoolToObject(o, "acknowledged", acknowledged);
    cJSON_AddNumberToObject(o, "noticeVersion", ORBIT_SOURCE_NOTICE_VERSION);
    for (size_t i = 0; i < sizeof providers / sizeof providers[0]; i++) {
        if (acknowledged && (orbit.sources.enabled & source_flag(providers[i].id)))
            cJSON_AddItemToArray(enabled, cJSON_CreateString(providers[i].id));
        unsigned count = 0;
        for (size_t j = 0; j < orbit.release_count; j++)
            if (!strcmp(orbit.releases[j].source, providers[i].id))
                count++;
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "id", providers[i].id);
        cJSON_AddStringToObject(p, "label", providers[i].label);
        cJSON_AddNumberToObject(p, "releaseCount", count);
        cJSON_AddBoolToObject(p, "downloadReady", providers[i].ready);
        cJSON_AddItemToArray(options, p);
    }
    return o;
}

int sources_set_locked(const cJSON *input, char *err, size_t cap) {
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(input, "enabled"), *x;
    if (!cJSON_IsArray(enabled) || cJSON_GetArraySize(enabled) > 2) {
        copy_text(err, cap, "Select Vikingfile, Archive.org, both, or neither.");
        return 400;
    }
    unsigned flags = 0;
    cJSON_ArrayForEach(x, enabled) {
        unsigned flag = cJSON_IsString(x) ? source_flag(x->valuestring) : 0;
        if (!flag || (flags & flag)) {
            copy_text(err, cap, "Unknown or duplicate source.");
            return 400;
        }
        flags |= flag;
    }
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(input, "noticeVersion");
    if (!cJSON_IsNumber(version) || version->valuedouble != ORBIT_SOURCE_NOTICE_VERSION ||
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(input, "acknowledged"))) {
        copy_text(err, cap,
                  "Read and acknowledge the current download notice before saving sources.");
        return 400;
    }
    /* Settings and queued-job pauses are committed together. A failed write changes neither. */
    SourceSettings old = orbit.sources;
    Job *previous = orbit.job_count ? malloc(orbit.job_count * sizeof(Job)) : NULL;
    if (orbit.job_count && !previous) {
        copy_text(err, cap, "Could not save source settings. Try again.");
        return 503;
    }
    if (previous)
        memcpy(previous, orbit.jobs, orbit.job_count * sizeof(Job));
    orbit.sources.enabled = flags;
    orbit.sources.notice_version = ORBIT_SOURCE_NOTICE_VERSION;
    if (old.notice_version != ORBIT_SOURCE_NOTICE_VERSION || old.acknowledged_at <= 0)
        orbit.sources.acknowledged_at = time(NULL);
    for (size_t i = 0; i < orbit.job_count; i++) {
        Job *j = &orbit.jobs[i];
        if (source_enabled_locked(&j->release))
            continue;
        bool active = !strcmp(j->status, "downloading") || !strcmp(j->status, "verifying");
        if (active || !strcmp(j->status, "queued") || !strcmp(j->status, "retrying")) {
            /* Network callbacks read pause without orbit.mutex. Do not expose
             * an active pause until this settings transaction has committed. */
            if (!active) {
                j->pause = true;
                copy_text(j->status, sizeof j->status, "paused");
            }
            copy_text(j->error, sizeof j->error,
                      "Source disabled. Enable it in Sources before resuming.");
        }
    }
    if (state_save_locked()) {
        orbit.sources = old;
        if (previous) {
            for (size_t i = 0; i < orbit.job_count; i++) {
                orbit.jobs[i].pause = previous[i].pause;
                copy_text(orbit.jobs[i].status, sizeof orbit.jobs[i].status, previous[i].status);
                copy_text(orbit.jobs[i].error, sizeof orbit.jobs[i].error, previous[i].error);
            }
        }
        free(previous);
        copy_text(err, cap, "Could not save source settings. Check console storage.");
        return 503;
    }
    for (size_t i = 0; i < orbit.job_count; i++) {
        Job *j = &orbit.jobs[i];
        if (!source_enabled_locked(&j->release) &&
            (!strcmp(j->status, "downloading") || !strcmp(j->status, "verifying"))) j->pause = true;
    }
    free(previous);
    browser_source_changed_locked();
    pthread_cond_signal(&orbit.changed);
    return 200;
}
