#define _GNU_SOURCE
#include "pilot_internal.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define PILOT_MODULE_DIR_NAME_MAX 64u

static pthread_mutex_t module_dir_lock = PTHREAD_MUTEX_INITIALIZER;

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

static int is_directory(const char *path)
{
    struct stat info;
    memset(&info, 0, sizeof(info));
    int status = g_pilot.fsys.stat_path(path, &info);
    if (status == 0) return S_ISDIR(info.st_mode) ? 1 : -1;
    if (!pilot_fsys_is_not_found(&g_pilot.fsys, status)) return -1;

    int mkdir_status = g_pilot.mkdir_path(path);
    if (mkdir_status == 0) return 1;

    memset(&info, 0, sizeof(info));
    status = g_pilot.fsys.stat_path(path, &info);
    if (status == 0 && S_ISDIR(info.st_mode)) return 1;
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

    pthread_mutex_lock(&module_dir_lock);
    int pilot_ok = is_directory(pilot_dir);
    int modules_ok = pilot_ok > 0 ? is_directory(modules_dir) : 0;
    int module_ok = modules_ok > 0 ? is_directory(module_dir) : 0;
    pthread_mutex_unlock(&module_dir_lock);

    if (pilot_ok < 0 || modules_ok < 0 || module_ok < 0)
        return PILOT_STATUS_IO;
    return PILOT_STATUS_OK;
}
