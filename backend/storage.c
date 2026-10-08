#include "orbit.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
/* Use the same directory operations as downloads. access() can disagree with
 * actual I/O under console credentials; it must not hide an otherwise usable
 * internal or M.2 drive. Creation/writes are checked only on download actions. */
static const char *inspect_destination(int root, dev_t device) {
    struct stat expected, actual;
    if (fstatat(root, "homebrew", &expected, AT_SYMLINK_NOFOLLOW))
        return errno == ENOENT ? NULL : "destination-stat-failed";
    if (!S_ISDIR(expected.st_mode)) return "destination-not-directory";
    if (expected.st_dev != device) return "destination-on-other-device";
    int fd = openat(root, "homebrew", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return "destination-not-accessible";
    const char *reason = NULL;
    if (fstat(fd, &actual) || !S_ISDIR(actual.st_mode) ||
        actual.st_dev != expected.st_dev || actual.st_ino != expected.st_ino)
        reason = "destination-changed";
    close(fd);
    return reason;
}
/* Return a fixed, path-free reason for diagnostics. Listing never creates files. */
static const char *inspect(Storage *s, const char *id, const char *label, const char *root,
                           bool external) {
    struct stat st, parent, opened;
    struct statvfs fs;
    if (lstat(root, &st)) return errno == ENOENT ? "not-present" : "root-stat-failed";
    if (!S_ISDIR(st.st_mode)) return "root-not-directory";
    if (statvfs(root, &fs)) return "capacity-unavailable";
    if (fs.f_flag & ST_RDONLY) return "read-only-filesystem";
    if (external && !orbit.desktop) {
        if (stat("/mnt", &parent)) return "mount-check-failed";
        if (st.st_dev == parent.st_dev) return "not-mounted";
    }
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return "root-not-accessible";
    const char *reason;
    if (fstat(fd, &opened) || !S_ISDIR(opened.st_mode) ||
        opened.st_dev != st.st_dev || opened.st_ino != st.st_ino)
        reason = "root-changed";
    else
        reason = inspect_destination(fd, st.st_dev);
    close(fd);
    if (reason) return reason;
    memset(s, 0, sizeof *s);
    copy_text(s->id, sizeof s->id, id);
    copy_text(s->label, sizeof s->label, label);
    copy_text(s->root, sizeof s->root, root);
    s->device = st.st_dev;
    s->inode = st.st_ino;
    s->external = external;
    uint64_t unit = fs.f_frsize ? fs.f_frsize : fs.f_bsize;
    s->free_bytes = (uint64_t)fs.f_bavail * unit;
    s->total_bytes = (uint64_t)fs.f_blocks * unit;
    return NULL;
}
static bool inspect_report(Storage *s, const char *id, const char *label, const char *root,
                           bool external, cJSON *report) {
    const char *reason = inspect(s, id, label, root, external);
    if (report) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", id);
        cJSON_AddBoolToObject(item, "available", !reason);
        cJSON_AddStringToObject(item, "reason", reason ? reason : "ready");
        cJSON_AddItemToArray(report, item);
    }
    return !reason;
}
static size_t enumerate(Storage *out, cJSON *report) {
    size_t n = 0;
    if (orbit.desktop) {
        if (orbit.desktop_storage[0] &&
            inspect_report(out, "desktop", "Desktop test storage", orbit.desktop_storage, false, report))
            n++;
        return n;
    }
    for (int i = 0; i < 8; i++) {
        char id[24], label[64], root[64];
        snprintf(id, sizeof id, "usb%d", i);
        snprintf(label, sizeof label, "USB storage %d", i + 1);
        snprintf(root, sizeof root, "/mnt/usb%d", i);
        if (inspect_report(&out[n], id, label, root, true, report))
            n++;
    }
    for (int i = 0; i < 8; i++) {
        char id[24], label[64], root[64];
        snprintf(id, sizeof id, "ext%d", i);
        snprintf(label, sizeof label, "External storage %d", i + 1);
        if (i == 1) copy_text(label, sizeof label, "M.2 SSD");
        snprintf(root, sizeof root, "/mnt/ext%d", i);
        if (inspect_report(&out[n], id, label, root, true, report))
            n++;
    }
    if (inspect_report(&out[n], "internal", "Internal storage", "/data", false, report))
        n++;
    return n;
}
size_t storage_list(Storage *out) {
    return enumerate(out, NULL);
}
cJSON *storage_diagnostics(void) {
    Storage out[ORBIT_MAX_STORAGE];
    cJSON *report = cJSON_CreateArray();
    enumerate(out, report);
    return report;
}
/* The drive new downloads start with ("" lets clients choose: the first external drive). Creating
 * a download also sets it, so the last drive used stays the default. */
int storage_prefer(const char *id, char *error, size_t cap) {
    if (*id) {
        Storage s[ORBIT_MAX_STORAGE];
        size_t n = storage_list(s), i = 0;
        while (i < n && strcmp(s[i].id, id))
            i++;
        if (i == n) {
            copy_text(error, cap, "Selected storage is not connected or writable.");
            return 409;
        }
    }
    pthread_mutex_lock(&orbit.mutex);
    char previous[sizeof orbit.preferred_storage];
    copy_text(previous, sizeof previous, orbit.preferred_storage);
    copy_text(orbit.preferred_storage, sizeof orbit.preferred_storage, id);
    int code = 200;
    if (state_save_locked()) {
        copy_text(orbit.preferred_storage, sizeof orbit.preferred_storage, previous);
        copy_text(error, cap, "Could not save the default drive.");
        code = 503;
    }
    pthread_mutex_unlock(&orbit.mutex);
    return code;
}
bool storage_matches(const Job *j) {
    Storage a[ORBIT_MAX_STORAGE];
    size_t n = storage_list(a);
    for (size_t i = 0; i < n; i++)
        if (!strcmp(a[i].id, j->storage_id) && !strcmp(a[i].root, j->root) &&
            a[i].device == j->device && a[i].inode == j->inode)
            return true;
    return false;
}
int storage_open(const Job *j) {
    if (!storage_matches(j)) {
        errno = ENODEV;
        return -1;
    }
    int root = open(j->root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (root < 0)
        return -1;
    struct stat st;
    if (fstat(root, &st) || st.st_dev != j->device || st.st_ino != j->inode) {
        close(root);
        errno = ENODEV;
        return -1;
    }
    int fd = openat(root, "homebrew", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0 && errno == ENOENT) {
        /* Opening an existing destination must not require permission to
         * create entries in its parent. Create only when it is absent. */
        if (!mkdirat(root, "homebrew", 0755) || errno == EEXIST)
            fd = openat(root, "homebrew", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    }
    int error = errno;
    close(root);
    if (fd < 0) {
        errno = error;
        return -1;
    }
    if (fstat(fd, &st) || st.st_dev != j->device) {
        close(fd);
        errno = ENODEV;
        return -1;
    }
    return fd;
}
/* Explicit download preflight, never discovery: exclusively create a private
 * file relative to the verified destination and unlink it before writing so
 * failed writes leave no probe behind. Never truncate or remove a collision. */
int storage_check_writable(int directory) {
    for (int attempt = 0; attempt < 4; attempt++) {
        char nonce[33], name[64];
        random_hex(nonce, 16);
        snprintf(name, sizeof name, ".orbit-write-%s.tmp", nonce);
        int fd = openat(directory, name, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            return -1;
        }
        int error = 0;
        if (unlinkat(directory, name, 0)) {
            error = errno;
        } else {
            ssize_t n;
            do { n = write(fd, "", 1); } while (n < 0 && errno == EINTR);
            if (n != 1) error = n < 0 ? errno : EIO;
        }
        if (close(fd) && !error) error = errno;
        if (error) { errno = error; return -1; }
        return 0;
    }
    errno = EEXIST;
    return -1;
}
