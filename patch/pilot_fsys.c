#define _GNU_SOURCE
#include "pilot_internal.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PILOT_FSYS_NOT_FOUND_STATUS 0x80000051u
#define PILOT_FSYS_OPEN_READ 1
#define PILOT_FSYS_OPEN_WRITE 2
#define PILOT_FSYS_OPEN_CREATE 0xE
#define PILOT_FSYS_SEEK_END 2

static int resolve_symbol(const char *name,
                          void *destination,
                          size_t size,
                          const char **missing_symbol_out)
{
    void *symbol = dlsym(RTLD_DEFAULT, name);
    if (!symbol || size != sizeof(symbol)) {
        if (missing_symbol_out && !*missing_symbol_out)
            *missing_symbol_out = name;
        return 0;
    }
    memcpy(destination, &symbol, size);
    return 1;
}

int pilot_fsys_resolve(struct pilot_fsys_api *api,
                       const char **missing_symbol_out)
{
    if (missing_symbol_out) *missing_symbol_out = 0;
    if (!api) {
        if (missing_symbol_out) *missing_symbol_out = "<api>";
        return 0;
    }
    memset(api, 0, sizeof(*api));

    int ok = 1;
    ok &= resolve_symbol("Fsys_Stat", &api->stat_path,
                         sizeof(api->stat_path), missing_symbol_out);
    ok &= resolve_symbol("Fsys_Open", &api->open_file,
                         sizeof(api->open_file), missing_symbol_out);
    ok &= resolve_symbol("Fsys_Read", &api->read_file,
                         sizeof(api->read_file), missing_symbol_out);
    ok &= resolve_symbol("Fsys_Write", &api->write_file,
                         sizeof(api->write_file), missing_symbol_out);
    ok &= resolve_symbol("Fsys_Seek", &api->seek_file,
                         sizeof(api->seek_file), missing_symbol_out);
    ok &= resolve_symbol("Fsys_Flush", &api->flush_file,
                         sizeof(api->flush_file), missing_symbol_out);
    ok &= resolve_symbol("Fsys_Close", &api->close_file,
                         sizeof(api->close_file), missing_symbol_out);
    ok &= resolve_symbol("pf2_errnum", &api->pf2_errnum,
                         sizeof(api->pf2_errnum), missing_symbol_out);
    ok &= resolve_symbol("pf2_inerrnum", &api->pf2_inerrnum,
                         sizeof(api->pf2_inerrnum), missing_symbol_out);
    return ok;
}

static void set_check_diagnostic(
    struct pilot_fsys_check_diagnostic *diagnostic,
    const char *stage,
    int status,
    const struct pilot_fsys_api *api)
{
    if (!diagnostic) return;
    diagnostic->stage = stage;
    diagnostic->status = status;
    diagnostic->pf2_errno = api && api->pf2_errnum
        ? api->pf2_errnum() : -1;
    diagnostic->pf2_inner = api && api->pf2_inerrnum
        ? api->pf2_inerrnum() : -1;
}

int pilot_fsys_is_not_found(const struct pilot_fsys_api *api, int status)
{
    if (!api || (uint32_t)status != PILOT_FSYS_NOT_FOUND_STATUS)
        return 0;

    /* Fsys_Open can return NOT_FOUND while the PF2 error pair remains 0/0. */
    if (api->pf2_errnum) (void)api->pf2_errnum();
    if (api->pf2_inerrnum) (void)api->pf2_inerrnum();
    return 1;
}

int pilot_fsys_check_disable(const char *directory,
                             const char *marker,
                             const struct pilot_fsys_api *api,
                             struct pilot_fsys_check_diagnostic *diagnostic)
{
    if (diagnostic) {
        diagnostic->stage = "none";
        diagnostic->status = 0;
        diagnostic->pf2_errno = 0;
        diagnostic->pf2_inner = 0;
    }
    if (!directory || !marker || !api || !api->stat_path ||
        !api->open_file || !api->close_file || !api->pf2_errnum ||
        !api->pf2_inerrnum) {
        set_check_diagnostic(diagnostic, "invalid_args", -1, api);
        return -1;
    }

    struct stat parent_stat;
    memset(&parent_stat, 0, sizeof(parent_stat));
    int stat_status = api->stat_path(directory, &parent_stat);
    if (stat_status != 0) {
        set_check_diagnostic(diagnostic, "stat_directory", stat_status, api);
        return -1;
    }

    void *handle = 0;
    int status = api->open_file(marker, &handle, PILOT_FSYS_OPEN_READ);
    if (status == 0 && handle) {
        int close_status = api->close_file(handle);
        if (close_status == 0) return 1;
        set_check_diagnostic(diagnostic, "close_marker", close_status, api);
        return -1;
    }

    /* Read the thread-local PF2 error pair before making another Fsys call. */
    int not_found = pilot_fsys_is_not_found(api, status);
    set_check_diagnostic(diagnostic, "open_marker", status, api);
    if (not_found) {
        if (diagnostic) diagnostic->stage = "marker_absent";
        return 0;
    }
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
        if (count < 0) result = PILOT_STATUS_IO;
        else if (count > 0) result = PILOT_STATUS_RANGE;
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

PilotStatus pilot_fsys_write_new_file(const char *path,
                                      const void *data,
                                      size_t length)
{
    if (!path || (!data && length)) return PILOT_STATUS_INVALID_ARGUMENT;
    if (g_pilot.write_file) {
        return g_pilot.write_file(path, data, length) == 0
            ? PILOT_STATUS_OK : PILOT_STATUS_IO;
    }

    struct pilot_fsys_api *api = &g_pilot.fsys;
    if (!api->open_file || !api->write_file || !api->flush_file ||
        !api->close_file)
        return PILOT_STATUS_IO;

    void *handle = 0;
    int status = api->open_file(path, &handle, PILOT_FSYS_OPEN_CREATE);
    if (status != 0 || !handle) return PILOT_STATUS_IO;

    PilotStatus result = PILOT_STATUS_OK;
    if (length) {
        size_t written_total = 0;
        const char *cursor = (const char *)data;
        while (written_total < length) {
            int written = api->write_file(handle, cursor + written_total,
                                          length - written_total);
            if (written <= 0 || (size_t)written > length - written_total) {
                result = PILOT_STATUS_IO;
                break;
            }
            written_total += (size_t)written;
        }
    }
    if (result == PILOT_STATUS_OK && api->flush_file(handle) != 0)
        result = PILOT_STATUS_IO;
    if (api->close_file(handle) != 0 && result == PILOT_STATUS_OK)
        result = PILOT_STATUS_IO;
    return result;
}

static PilotStatus append_path_locked(const char *path,
                                      const char *line,
                                      size_t length)
{
    struct pilot_fsys_api *api = &g_pilot.fsys;
    if (!path || !line || !length || length > 1024 ||
        !api->open_file || !api->seek_file ||
        !api->write_file || !api->flush_file || !api->close_file ||
        !api->pf2_errnum || !api->pf2_inerrnum)
        return PILOT_STATUS_INVALID_ARGUMENT;

    void *handle = 0;
    int status = api->open_file(path, &handle, PILOT_FSYS_OPEN_WRITE);
    if (status == 0 && handle) {
        int64_t position = 0;
        status = api->seek_file(handle, 0, PILOT_FSYS_SEEK_END, &position);
        PilotStatus result = status == 0
            ? write_all(api, handle, line, length) : PILOT_STATUS_IO;
        if (api->close_file(handle) != 0 && result == PILOT_STATUS_OK)
            result = PILOT_STATUS_IO;
        return result;
    }

    if (!pilot_fsys_is_not_found(api, status)) return PILOT_STATUS_IO;
    if (pilot_fsys_write_new_file(path, line, length) != PILOT_STATUS_OK)
        return PILOT_STATUS_IO;

    /* The mount helper creates a new file; reopen it so Fsys_Flush confirms
       the initial record before the caller enters any module code. */
    handle = 0;
    status = api->open_file(path, &handle, PILOT_FSYS_OPEN_WRITE);
    if (status != 0 || !handle) {
        (void)pilot_fsys_is_not_found(api, status);
        return PILOT_STATUS_IO;
    }
    PilotStatus result = api->flush_file(handle) == 0
        ? PILOT_STATUS_OK : PILOT_STATUS_IO;
    if (api->close_file(handle) != 0 && result == PILOT_STATUS_OK)
        result = PILOT_STATUS_IO;
    return result;
}

static void set_fuse_state(enum pilot_fuse_state state,
                           uint64_t package,
                           enum pilot_fuse_state *state_out,
                           uint64_t *package_out)
{
    *state_out = state;
    *package_out = package;
}

PilotStatus pilot_fuse_read_state(enum pilot_fuse_state *state_out,
                                  uint64_t *package_out)
{
    if (state_out) *state_out = PILOT_FUSE_STATE_INVALID;
    if (package_out) *package_out = 0;
    if (!state_out || !package_out || !g_pilot.drive_letter)
        return PILOT_STATUS_INVALID_ARGUMENT;

    char path[48];
    int path_length = snprintf(path, sizeof(path), "%c:/pilot/fuse.log",
                               g_pilot.drive_letter);
    if (path_length <= 0 || (size_t)path_length >= sizeof(path))
        return PILOT_STATUS_RANGE;

    struct pilot_fsys_api *api = &g_pilot.fsys;
    if (!api->open_file || !api->read_file || !api->seek_file ||
        !api->close_file || !api->pf2_errnum || !api->pf2_inerrnum)
        return PILOT_STATUS_UNSUPPORTED;

    void *handle = 0;
    int status = api->open_file(path, &handle, PILOT_FSYS_OPEN_READ);
    if (status != 0 || !handle) {
        if (pilot_fsys_is_not_found(api, status)) {
            set_fuse_state(PILOT_FUSE_STATE_NONE, 0,
                           state_out, package_out);
            return PILOT_STATUS_OK;
        }
        return PILOT_STATUS_IO;
    }

    int64_t end = 0;
    status = api->seek_file(handle, 0, PILOT_FSYS_SEEK_END, &end);
    if (status != 0 || end < 0) {
        (void)api->close_file(handle);
        return PILOT_STATUS_IO;
    }
    if (end == 0) {
        int close_status = api->close_file(handle);
        if (close_status != 0) return PILOT_STATUS_IO;
        set_fuse_state(PILOT_FUSE_STATE_NONE, 0, state_out, package_out);
        return PILOT_STATUS_OK;
    }

    char tail[257];
    const size_t capacity = sizeof(tail) - 1;
    int64_t start = end > (int64_t)capacity
        ? end - (int64_t)capacity : 0;
    int64_t position = 0;
    status = api->seek_file(handle, start, 0, &position);
    size_t used = 0;
    PilotStatus read_result = status == 0 ? PILOT_STATUS_OK : PILOT_STATUS_IO;
    while (read_result == PILOT_STATUS_OK && used < capacity) {
        int count = api->read_file(handle, tail + used, capacity - used);
        if (count < 0 || (size_t)count > capacity - used) {
            read_result = PILOT_STATUS_IO;
            break;
        }
        if (count == 0) break;
        used += (size_t)count;
    }
    if (api->close_file(handle) != 0 && read_result == PILOT_STATUS_OK)
        read_result = PILOT_STATUS_IO;
    if (read_result != PILOT_STATUS_OK) return read_result;
    if (!used) {
        set_fuse_state(PILOT_FUSE_STATE_INVALID, 0, state_out, package_out);
        return PILOT_STATUS_OK;
    }
    tail[used] = '\0';
    if (tail[used - 1] != '\n') {
        set_fuse_state(PILOT_FUSE_STATE_INVALID, 0, state_out, package_out);
        return PILOT_STATUS_OK;
    }

    size_t line_end = used - 1;
    size_t line_start = line_end;
    while (line_start > 0 && tail[line_start - 1] != '\n') --line_start;
    size_t line_length = line_end - line_start;
    if (!line_length || line_length >= 128) {
        set_fuse_state(PILOT_FUSE_STATE_INVALID, 0, state_out, package_out);
        return PILOT_STATUS_OK;
    }

    char line[128];
    memcpy(line, tail + line_start, line_length);
    line[line_length] = '\0';
    char state_name[16] = {0};
    char package_text[17] = {0};
    int consumed = 0;
    int fields = sscanf(line,
                        "PILOT_FUSE state=%15s package=%16[0-9A-Fa-f]%n",
                        state_name, package_text, &consumed);
    if (fields != 2 || consumed != (int)line_length ||
        strlen(package_text) != 16) {
        set_fuse_state(PILOT_FUSE_STATE_INVALID, 0, state_out, package_out);
        return PILOT_STATUS_OK;
    }

    char *number_end = 0;
    unsigned long long package = strtoull(package_text, &number_end, 16);
    if (!number_end || *number_end) {
        set_fuse_state(PILOT_FUSE_STATE_INVALID, 0, state_out, package_out);
    } else if (strcmp(state_name, "ARMED") == 0) {
        set_fuse_state(PILOT_FUSE_STATE_ARMED, (uint64_t)package,
                       state_out, package_out);
    } else if (strcmp(state_name, "COMPLETE") == 0) {
        set_fuse_state(PILOT_FUSE_STATE_COMPLETE, (uint64_t)package,
                       state_out, package_out);
    } else if (strcmp(state_name, "TRIPPED") == 0) {
        set_fuse_state(PILOT_FUSE_STATE_TRIPPED, (uint64_t)package,
                       state_out, package_out);
    } else {
        set_fuse_state(PILOT_FUSE_STATE_INVALID, 0, state_out, package_out);
    }
    return PILOT_STATUS_OK;
}

PilotStatus pilot_fuse_append_state(enum pilot_fuse_state state,
                                    uint64_t package)
{
    const char *state_name = state == PILOT_FUSE_STATE_ARMED ? "ARMED"
        : state == PILOT_FUSE_STATE_COMPLETE ? "COMPLETE"
        : state == PILOT_FUSE_STATE_TRIPPED ? "TRIPPED" : 0;
    if (!state_name || !g_pilot.drive_letter)
        return PILOT_STATUS_INVALID_ARGUMENT;

    char path[48];
    char record[96];
    char visible_line[96];
    int path_length = snprintf(path, sizeof(path), "%c:/pilot/fuse.log",
                               g_pilot.drive_letter);
    int record_length = snprintf(record, sizeof(record),
                               "\nPILOT_FUSE state=%s package=%016llX\n",
                               state_name, (unsigned long long)package);
    int visible_length = snprintf(visible_line, sizeof(visible_line),
                                 "PILOT_FUSE state=%s package=%016llX\n",
                                 state_name, (unsigned long long)package);
    if (path_length <= 0 || (size_t)path_length >= sizeof(path) ||
        record_length <= 0 || (size_t)record_length >= sizeof(record) ||
        visible_length <= 0 || (size_t)visible_length >= sizeof(visible_line))
        return PILOT_STATUS_RANGE;

    pthread_mutex_lock(&g_pilot.log_lock);
    PilotStatus result = append_path_locked(path, record,
                                            (size_t)record_length);
    pthread_mutex_unlock(&g_pilot.log_lock);
    if (result != PILOT_STATUS_OK) return result;

    /* Keep the fuse transitions visible alongside the ordinary Pilot log. */
    (void)pilot_log_append(visible_line, (size_t)visible_length, 0);
    return PILOT_STATUS_OK;
}

PilotStatus pilot_log_append(const char *line,
                             size_t length,
                             int optional_api_call)
{
    if (!line || !length || length > 1024) return PILOT_STATUS_INVALID_ARGUMENT;
    if (optional_api_call && !g_pilot.optional_features_enabled)
        return PILOT_STATUS_DISABLED;
    if (!g_pilot.log_path[0])
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
            if (not_found && g_pilot.write_file) {
                result = g_pilot.write_file(g_pilot.log_path, line, length) == 0
                    ? PILOT_STATUS_OK : PILOT_STATUS_IO;
            } else if (not_found) {
                void *create_handle = 0;
                int create_status = api->open_file(g_pilot.log_path,
                                                   &create_handle,
                                                   PILOT_FSYS_OPEN_CREATE);
                if (create_status == 0 && create_handle) {
                    result = write_all(api, create_handle, line, length);
                    if (api->close_file(create_handle) != 0 &&
                        result == PILOT_STATUS_OK)
                        result = PILOT_STATUS_IO;
                }
            }
        }
    } else if (g_pilot.write_file) {
        result = g_pilot.write_file(g_pilot.log_path, line, length) == 0
            ? PILOT_STATUS_OK : PILOT_STATUS_IO;
    }
    pthread_mutex_unlock(&g_pilot.log_lock);
    return result;
}
