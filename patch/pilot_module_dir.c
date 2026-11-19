#define _GNU_SOURCE
#include "pilot_internal.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define PILOT_MODULE_DIR_NAME_MAX 64u
#define PILOT_FSYS_NOT_FOUND_STATUS 0x80000051u
#define PILOT_MODULE_DIR_LOG_LIMIT 16u

static pthread_mutex_t module_dir_lock = PTHREAD_MUTEX_INITIALIZER;
/* Guarded by module_dir_lock. Caps the total number of diagnostic lines. */
static unsigned int module_dir_log_budget = PILOT_MODULE_DIR_LOG_LIMIT;

struct dir_stat_result {
    int status;
    unsigned int mode;
    int pf2_errno;
    int pf2_inner;
    int mkdir_result;
};

/* Log records are filled while holding module_dir_lock but emitted after it
 * is released. Keeping all I/O outside the lock removes any lock-order or
 * re-entry hazard, and pilot_log_append never reaches back into this file. */
struct dir_log_entry {
    const char *event;
    const char *path;
    struct dir_stat_result result;
};

static int valid_module_name(const char *name)
{
    if (!name || !*name || name[0] == '.') return 0;
    size_t length = strlen(name);
    if (length > PILOT_MODULE_DIR_NAME_MAX || strstr(name, "..")) return 0;

    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)name[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ||
              ch == '.'))
            return 0;
    }
    return 1;
}

/* Pure memory work, called under module_dir_lock: no I/O, no external calls. */
static void fill_dir_log(struct dir_log_entry *entry,
                          const char *event,
                          const char *path,
                          const struct dir_stat_result *result)
{
    if (!module_dir_log_budget) return;
    --module_dir_log_budget;
    entry->event = event;
    entry->path = path;
    entry->result = *result;
}

static void emit_dir_log(const struct dir_log_entry *entry)
{
    if (!entry->event) return;
    char line[256];
    int length = snprintf(line, sizeof(line),
                          "Pilot module_dir %s path=%s status=0x%08X mode=0x%04X "
                          "pf2=%d inner=%d mkdir=%d\n",
                          entry->event, entry->path,
                          (unsigned int)entry->result.status,
                          entry->result.mode & 0xffffu,
                          entry->result.pf2_errno, entry->result.pf2_inner,
                          entry->result.mkdir_result);
    if (length > 0 && (size_t)length < sizeof(line))
        (void)pilot_log_append(line, (size_t)length, 0);
}

/* Fsys_Stat on the camera is not guaranteed to fill a glibc-compatible
 * st_mode. Record it for diagnosis, but never use it to decide whether a
 * directory exists: a zero status means the path resolves, and a path that
 * is occupied by a regular file will fail later on file creation anyway. */
static void probe_directory(const char *path,
                             struct dir_stat_result *result)
{
    struct stat info;
    memset(&info, 0, sizeof(info));
    memset(result, 0, sizeof(*result));
    result->mkdir_result = -1;

    result->status = g_pilot.fsys.stat_path(path, &info);
    result->mode = (unsigned int)info.st_mode;
    /* The PF2 error pair belongs to this Fsys call; consume it immediately. */
    result->pf2_errno = g_pilot.fsys.pf2_errnum
        ? g_pilot.fsys.pf2_errnum() : -1;
    result->pf2_inner = g_pilot.fsys.pf2_inerrnum
        ? g_pilot.fsys.pf2_inerrnum() : -1;
}

/* Fsys_Stat may report a missing path with the NOT_FOUND status, with a
 * PF2 ENOENT pair, or with both. The host shim only ever produces the
 * first form, so the extra checks matter only on the camera. */
static int path_is_missing(const struct dir_stat_result *result)
{
    return (uint32_t)result->status == PILOT_FSYS_NOT_FOUND_STATUS ||
           result->pf2_errno == ENOENT || result->pf2_inner == ENOENT;
}

/* Straight-line code: no loop, no retry, no recursion. At most one mkdir and
 * two stat calls per directory level. */
static int is_directory(const char *path, struct dir_log_entry *log_entry)
{
    struct dir_stat_result before;
    probe_directory(path, &before);
    if (before.status == 0) return 1;

    if (!path_is_missing(&before)) {
        fill_dir_log(log_entry, "stat_failed", path, &before);
        return -1;
    }

    before.mkdir_result = g_pilot.mkdir_path(path);

    struct dir_stat_result after;
    probe_directory(path, &after);
    after.mkdir_result = before.mkdir_result;
    if (after.status == 0) {
        fill_dir_log(log_entry, "created", path, &after);
        return 1;
    }

    fill_dir_log(log_entry, "mkdir_failed", path, &after);
    return -1;
}

PilotStatus pilot_get_module_dir_impl(const char *module_name,
                                       char *buffer,
                                       size_t capacity)
{
    if (!__atomic_load_n(&g_pilot.optional_features_enabled, __ATOMIC_ACQUIRE))
        return PILOT_STATUS_DISABLED;
    if (!valid_module_name(module_name) || !buffer || !capacity ||
        !g_pilot.drive_letter || !g_pilot.mkdir_path ||
        !g_pilot.fsys.stat_path || !g_pilot.fsys.pf2_errnum ||
        !g_pilot.fsys.pf2_inerrnum)
        return PILOT_STATUS_INVALID_ARGUMENT;

    char pilot_dir[32];
    char modules_dir[48];
    char module_dir[128];
    int pilot_size = snprintf(pilot_dir, sizeof(pilot_dir), "%c:/pilot",
                              g_pilot.drive_letter);
    int modules_size = snprintf(modules_dir, sizeof(modules_dir),
                                "%c:/pilot/modules", g_pilot.drive_letter);
    int module_size = snprintf(module_dir, sizeof(module_dir),
                               "%c:/pilot/modules/%s",
                               g_pilot.drive_letter, module_name);
    if (pilot_size <= 0 || (size_t)pilot_size >= sizeof(pilot_dir) ||
        modules_size <= 0 || (size_t)modules_size >= sizeof(modules_dir) ||
        module_size <= 0 || (size_t)module_size >= sizeof(module_dir))
        return PILOT_STATUS_RANGE;

    int output_size = snprintf(buffer, capacity, "%s/", module_dir);
    if (output_size <= 0 || (size_t)output_size >= capacity)
        return PILOT_STATUS_RANGE;

    struct dir_log_entry entries[3] = {{0}, {0}, {0}};
    pthread_mutex_lock(&module_dir_lock);
    int pilot_ok = is_directory(pilot_dir, &entries[0]);
    int modules_ok = pilot_ok > 0 ? is_directory(modules_dir, &entries[1]) : 0;
    int module_ok = modules_ok > 0 ? is_directory(module_dir, &entries[2]) : 0;
    pthread_mutex_unlock(&module_dir_lock);

    /* Emitted after unlock; pointers still reference this stack frame. */
    emit_dir_log(&entries[0]);
    emit_dir_log(&entries[1]);
    emit_dir_log(&entries[2]);

    if (pilot_ok < 0 || modules_ok < 0 || module_ok < 0)
        return PILOT_STATUS_IO;
    return PILOT_STATUS_OK;
}
