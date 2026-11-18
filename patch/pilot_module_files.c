#define _GNU_SOURCE
#include "pilot_internal.h"

#include <stdio.h>
#include <string.h>

#define PILOT_MODULE_FILE_NAME_CAPACITY 128u
#define PILOT_MODULE_FILE_PATH_CAPACITY 256u
#define PILOT_FSYS_OPEN_WRITE 2
#define PILOT_FSYS_SEEK_END 2

static const char *file_module_name(const PilotFileApi *file)
{
    return file ? (const char *)file->context : 0;
}

static const char *module_log_name(const PilotModuleLogApi *log)
{
    return log ? (const char *)log->context : 0;
}

PilotStatus pilot_file_read(const PilotFileApi *file,
                            const char *file_name,
                            char *buffer,
                            size_t capacity)
{
    if (!buffer || !capacity) return PILOT_STATUS_INVALID_ARGUMENT;
    size_t read_capacity = capacity > 1 ? capacity - 1 : capacity;
    size_t length = 0;
    PilotStatus status = pilot_file_read_bytes(file, file_name, buffer,
                                               read_capacity, &length);
    if (status != PILOT_STATUS_OK) return status;
    if (length >= capacity) return PILOT_STATUS_RANGE;
    buffer[length] = '\0';
    return PILOT_STATUS_OK;
}

PilotStatus pilot_file_write(const PilotFileApi *file,
                             const char *file_name,
                             const char *text)
{
    if (!text) return PILOT_STATUS_INVALID_ARGUMENT;
    return pilot_file_write_bytes(file, file_name, text, strlen(text));
}

PilotStatus pilot_file_append(const PilotFileApi *file,
                              const char *file_name,
                              const char *text)
{
    if (!text) return PILOT_STATUS_INVALID_ARGUMENT;
    return pilot_file_append_bytes(file, file_name, text, strlen(text));
}

PilotStatus pilot_file_read_bytes(const PilotFileApi *file,
                                  const char *file_name,
                                  void *buffer,
                                  size_t capacity,
                                  size_t *length_out)
{
    const char *module_name = file_module_name(file);
    if (!module_name) return PILOT_STATUS_INVALID_ARGUMENT;
    return pilot_module_file_read_impl(module_name, file_name, buffer,
                                       capacity, length_out);
}

PilotStatus pilot_file_write_bytes(const PilotFileApi *file,
                                   const char *file_name,
                                   const void *data,
                                   size_t length)
{
    const char *module_name = file_module_name(file);
    if (!module_name) return PILOT_STATUS_INVALID_ARGUMENT;
    return pilot_module_file_write_impl(module_name, file_name, data, length);
}

PilotStatus pilot_file_append_bytes(const PilotFileApi *file,
                                    const char *file_name,
                                    const void *data,
                                    size_t length)
{
    const char *module_name = file_module_name(file);
    if (!module_name) return PILOT_STATUS_INVALID_ARGUMENT;
    return pilot_module_file_append_impl(module_name, file_name, data, length);
}

PilotStatus pilot_module_log_print(const PilotModuleLogApi *log,
                                   const char *message)
{
    const char *module_name = module_log_name(log);
    if (!module_name || !message || !*message)
        return PILOT_STATUS_INVALID_ARGUMENT;
    size_t message_length = strlen(message);
    if (message_length > 768) return PILOT_STATUS_RANGE;

    char line[896];
    char timestamp[32];
    size_t used = 0;
    if (pilot_time_utc_impl(timestamp, sizeof(timestamp)) == PILOT_STATUS_OK) {
        int count = snprintf(line, sizeof(line), "%s ", timestamp);
        if (count <= 0 || (size_t)count >= sizeof(line))
            return PILOT_STATUS_RANGE;
        used = (size_t)count;
    }
    if (message_length > sizeof(line) - used - 2)
        return PILOT_STATUS_RANGE;
    memcpy(line + used, message, message_length);
    used += message_length;
    if (line[used - 1] != '\n') line[used++] = '\n';
    return pilot_module_file_append_impl(module_name, "module.log",
                                         line, used);
}

static int valid_file_name(const char *name)
{
    if (!name || !*name || name[0] == '.') return 0;
    size_t length = strlen(name);
    if (length >= PILOT_MODULE_FILE_NAME_CAPACITY || strstr(name, ".."))
        return 0;

    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)name[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ||
              ch == '.'))
            return 0;
    }
    return 1;
}

static PilotStatus module_file_path(const char *module_name,
                                   const char *file_name,
                                   char path[PILOT_MODULE_FILE_PATH_CAPACITY])
{
    if (!valid_file_name(file_name)) return PILOT_STATUS_INVALID_ARGUMENT;

    char module_dir[128];
    PilotStatus status = pilot_get_module_dir_impl(module_name, module_dir,
                                                   sizeof(module_dir));
    if (status != PILOT_STATUS_OK) return status;

    int length = snprintf(path, PILOT_MODULE_FILE_PATH_CAPACITY, "%s%s",
                          module_dir, file_name);
    if (length <= 0 ||
        (size_t)length >= PILOT_MODULE_FILE_PATH_CAPACITY)
        return PILOT_STATUS_RANGE;
    return PILOT_STATUS_OK;
}

static int file_api_ready(void)
{
    const struct pilot_fsys_api *api = &g_pilot.fsys;
    return api->open_file && api->read_file &&
           api->write_file && api->seek_file && api->flush_file &&
           api->close_file && api->pf2_errnum && api->pf2_inerrnum;
}

static PilotStatus flush_path(const char *path)
{
    struct pilot_fsys_api *api = &g_pilot.fsys;
    void *handle = 0;
    int status = api->open_file(path, &handle, PILOT_FSYS_OPEN_WRITE);
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

static PilotStatus write_all(void *handle, const void *data, size_t length)
{
    struct pilot_fsys_api *api = &g_pilot.fsys;
    const unsigned char *cursor = data;
    size_t written_total = 0;
    while (written_total < length) {
        int written = api->write_file(handle, cursor + written_total,
                                      length - written_total);
        if (written <= 0 || (size_t)written > length - written_total)
            return PILOT_STATUS_IO;
        written_total += (size_t)written;
    }
    return api->flush_file(handle) == 0
        ? PILOT_STATUS_OK : PILOT_STATUS_IO;
}

PilotStatus pilot_module_file_read_impl(const char *module_name,
                                        const char *file_name,
                                        void *buffer,
                                        size_t capacity,
                                        size_t *length_out)
{
    if (length_out) *length_out = 0;
    if (!__atomic_load_n(&g_pilot.optional_features_enabled, __ATOMIC_ACQUIRE))
        return PILOT_STATUS_DISABLED;
    if (!buffer || !capacity || capacity > PILOT_MODULE_FILE_TRANSFER_MAX ||
        !length_out)
        return capacity > PILOT_MODULE_FILE_TRANSFER_MAX
            ? PILOT_STATUS_RANGE : PILOT_STATUS_INVALID_ARGUMENT;
    if (!file_api_ready()) return PILOT_STATUS_UNSUPPORTED;

    char path[PILOT_MODULE_FILE_PATH_CAPACITY];
    PilotStatus result = module_file_path(module_name, file_name, path);
    if (result != PILOT_STATUS_OK) return result;

    int not_found = 0;
    pthread_mutex_lock(&g_pilot.log_lock);
    result = pilot_fsys_read_all(&g_pilot.fsys, path, buffer, capacity,
                                 length_out, &not_found);
    pthread_mutex_unlock(&g_pilot.log_lock);
    if (not_found) return PILOT_STATUS_NOT_FOUND;
    return result;
}

PilotStatus pilot_module_file_write_impl(const char *module_name,
                                         const char *file_name,
                                         const void *data,
                                         size_t length)
{
    if (!__atomic_load_n(&g_pilot.optional_features_enabled, __ATOMIC_ACQUIRE))
        return PILOT_STATUS_DISABLED;
    if ((!data && length) || length > PILOT_MODULE_FILE_TRANSFER_MAX)
        return length > PILOT_MODULE_FILE_TRANSFER_MAX
            ? PILOT_STATUS_RANGE : PILOT_STATUS_INVALID_ARGUMENT;
    if (!file_api_ready()) return PILOT_STATUS_UNSUPPORTED;

    char path[PILOT_MODULE_FILE_PATH_CAPACITY];
    PilotStatus result = module_file_path(module_name, file_name, path);
    if (result != PILOT_STATUS_OK) return result;

    static const char empty_file = '\0';
    if (!data) data = &empty_file;

    pthread_mutex_lock(&g_pilot.log_lock);
    result = pilot_fsys_write_new_file(path, data, length);
    if (result == PILOT_STATUS_OK && g_pilot.write_file)
        result = flush_path(path);
    pthread_mutex_unlock(&g_pilot.log_lock);
    return result;
}

PilotStatus pilot_module_file_append_impl(const char *module_name,
                                          const char *file_name,
                                          const void *data,
                                          size_t length)
{
    if (!__atomic_load_n(&g_pilot.optional_features_enabled, __ATOMIC_ACQUIRE))
        return PILOT_STATUS_DISABLED;
    if ((!data && length) || length > PILOT_MODULE_FILE_TRANSFER_MAX)
        return length > PILOT_MODULE_FILE_TRANSFER_MAX
            ? PILOT_STATUS_RANGE : PILOT_STATUS_INVALID_ARGUMENT;
    if (!file_api_ready()) return PILOT_STATUS_UNSUPPORTED;

    char path[PILOT_MODULE_FILE_PATH_CAPACITY];
    PilotStatus result = module_file_path(module_name, file_name, path);
    if (result != PILOT_STATUS_OK) return result;
    if (!length) return PILOT_STATUS_OK;

    struct pilot_fsys_api *api = &g_pilot.fsys;
    pthread_mutex_lock(&g_pilot.log_lock);

    void *handle = 0;
    int status = api->open_file(path, &handle, PILOT_FSYS_OPEN_WRITE);
    if (status == 0 && handle) {
        int64_t position = 0;
        status = api->seek_file(handle, 0, PILOT_FSYS_SEEK_END, &position);
        result = status == 0
            ? write_all(handle, data, length) : PILOT_STATUS_IO;
        if (api->close_file(handle) != 0 && result == PILOT_STATUS_OK)
            result = PILOT_STATUS_IO;
    } else if (pilot_fsys_is_not_found(api, status)) {
        result = pilot_fsys_write_new_file(path, data, length);
        if (result == PILOT_STATUS_OK && g_pilot.write_file)
            result = flush_path(path);
    } else {
        result = PILOT_STATUS_IO;
    }

    pthread_mutex_unlock(&g_pilot.log_lock);
    return result;
}
