#define _GNU_SOURCE
#include "pilot_internal.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#define PILOT_FSYS_NOT_FOUND_STATUS 0x80000051u
#define PILOT_FSYS_OPEN_READ 1
#define PILOT_FSYS_OPEN_WRITE 2
#define PILOT_FSYS_SEEK_END 2

static int resolve_symbol(const char *name, void *destination, size_t size)
{
    void *symbol = dlsym(RTLD_DEFAULT, name);
    if (!symbol || size != sizeof(symbol)) return 0;
    memcpy(destination, &symbol, size);
    return 1;
}

int pilot_fsys_resolve(struct pilot_fsys_api *api)
{
    if (!api) return 0;
    memset(api, 0, sizeof(*api));

    int ok = 1;
    ok &= resolve_symbol("Fsys_Stat", &api->stat_path, sizeof(api->stat_path));
    ok &= resolve_symbol("Fsys_Open", &api->open_file, sizeof(api->open_file));
    ok &= resolve_symbol("Fsys_Read", &api->read_file, sizeof(api->read_file));
    ok &= resolve_symbol("Fsys_Write", &api->write_file, sizeof(api->write_file));
    ok &= resolve_symbol("Fsys_Seek", &api->seek_file, sizeof(api->seek_file));
    ok &= resolve_symbol("Fsys_Flush", &api->flush_file, sizeof(api->flush_file));
    ok &= resolve_symbol("Fsys_Close", &api->close_file, sizeof(api->close_file));
    ok &= resolve_symbol("pf2_errnum", &api->pf2_errnum, sizeof(api->pf2_errnum));
    ok &= resolve_symbol("pf2_inerrnum", &api->pf2_inerrnum, sizeof(api->pf2_inerrnum));
    return ok;
}

int pilot_fsys_is_not_found(const struct pilot_fsys_api *api, int status)
{
    if (!api || !api->pf2_errnum || !api->pf2_inerrnum ||
        (uint32_t)status != PILOT_FSYS_NOT_FOUND_STATUS)
        return 0;

    int error_number = api->pf2_errnum();
    int inner_error = api->pf2_inerrnum();
    return error_number == ENOENT && inner_error == 0;
}

int pilot_fsys_check_disable(const char *directory,
                             const char *marker,
                             const struct pilot_fsys_api *api)
{
    if (!directory || !marker || !api || !api->stat_path ||
        !api->open_file || !api->close_file || !api->pf2_errnum ||
        !api->pf2_inerrnum)
        return -1;

    struct stat parent_stat;
    memset(&parent_stat, 0, sizeof(parent_stat));
    if (api->stat_path(directory, &parent_stat) != 0) return -1;

    void *handle = 0;
    int status = api->open_file(marker, &handle, PILOT_FSYS_OPEN_READ);
    if (status == 0 && handle) {
        int close_status = api->close_file(handle);
        return close_status == 0 ? 1 : -1;
    }

    /* Read the thread-local PF2 error pair before making another Fsys call. */
    if (pilot_fsys_is_not_found(api, status)) return 0;
    return -1;
}

static PilotStatus read_status_to_pilot(int status)
{
    return status < 0 ? PILOT_STATUS_IO : PILOT_STATUS_OK;
}

PilotStatus pilot_fsys_read_all(const struct pilot_fsys_api *api,
                                const char *path,
                                void *buffer,
                                size_t capacity,
                                size_t *length_out,
                                int *not_found_out)
{
    if (length_out) *length_out = 0;
    if (not_found_out) *not_found_out = 0;
    if (!api || !api->open_file || !api->read_file || !api->close_file ||
        !path || !buffer || !capacity || !length_out)
        return PILOT_STATUS_INVALID_ARGUMENT;

    void *handle = 0;
    int status = api->open_file(path, &handle, PILOT_FSYS_OPEN_READ);
    if (status != 0 || !handle) {
        int not_found = pilot_fsys_is_not_found(api, status);
        if (not_found_out) *not_found_out = not_found;
        return not_found ? PILOT_STATUS_UNSUPPORTED : PILOT_STATUS_IO;
    }

    size_t used = 0;
    PilotStatus result = PILOT_STATUS_OK;
    while (used < capacity) {
        int count = api->read_file(handle, (char *)buffer + used,
                                   capacity - used);
        if (count < 0) {
            result = PILOT_STATUS_IO;
            break;
        }
        if (count == 0) break;
        if ((size_t)count > capacity - used) {
            result = PILOT_STATUS_IO;
            break;
        }
        used += (size_t)count;
    }

    if (result == PILOT_STATUS_OK && used == capacity) {
        unsigned char extra;
        int count = api->read_file(handle, &extra, sizeof(extra));
        if (count != 0) result = PILOT_STATUS_RANGE;
    }

    int close_status = api->close_file(handle);
    if (result == PILOT_STATUS_OK && close_status != 0)
        result = read_status_to_pilot(close_status);
    if (result == PILOT_STATUS_OK) *length_out = used;
    return result;
}

static PilotStatus write_all(struct pilot_fsys_api *api,
                             void *handle,
                             const char *line,
                             size_t length)
{
    size_t written_total = 0;
    while (written_total < length) {
        int written = api->write_file(handle, line + written_total,
                                      length - written_total);
        if (written <= 0 || (size_t)written > length - written_total)
            return PILOT_STATUS_IO;
        written_total += (size_t)written;
    }
    if (api->flush_file(handle) != 0) return PILOT_STATUS_IO;
    return PILOT_STATUS_OK;
}

PilotStatus pilot_log_append(const char *line,
                             size_t length,
                             int optional_api_call)
{
    if (!line || !length || length > 1024) return PILOT_STATUS_INVALID_ARGUMENT;
    if (optional_api_call && !g_pilot.optional_features_enabled)
        return PILOT_STATUS_DISABLED;
    if (!g_pilot.log_path[0] || !g_pilot.write_file)
        return PILOT_STATUS_IO;

    pthread_mutex_lock(&g_pilot.log_lock);
    struct pilot_fsys_api *api = &g_pilot.fsys;
    PilotStatus result = PILOT_STATUS_IO;
    if (api->open_file && api->write_file && api->seek_file &&
        api->flush_file && api->close_file && api->pf2_errnum &&
        api->pf2_inerrnum) {
        void *handle = 0;
        int status = api->open_file(g_pilot.log_path, &handle,
                                    PILOT_FSYS_OPEN_WRITE);
        if (status == 0 && handle) {
            int64_t position = 0;
            status = api->seek_file(handle, 0, PILOT_FSYS_SEEK_END, &position);
            if (status == 0)
                result = write_all(api, handle, line, length);
            if (api->close_file(handle) != 0 && result == PILOT_STATUS_OK)
                result = PILOT_STATUS_IO;
        } else {
            /* Fsys_Open failure state belongs to this worker; consume it now. */
            int not_found = pilot_fsys_is_not_found(api, status);
            if (not_found)
                result = g_pilot.write_file(g_pilot.log_path, line, length) == 0
                    ? PILOT_STATUS_OK : PILOT_STATUS_IO;
        }
    } else {
        result = g_pilot.write_file(g_pilot.log_path, line, length) == 0
            ? PILOT_STATUS_OK : PILOT_STATUS_IO;
    }
    pthread_mutex_unlock(&g_pilot.log_lock);
    return result;
}
