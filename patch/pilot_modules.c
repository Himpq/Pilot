#define _GNU_SOURCE
#include "pilot_internal.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PILOT_MANIFEST_CAPACITY 4096u
#define PILOT_MODULE_NAME_CAPACITY 65u
#define PILOT_MODULE_MAX_BYTES (2u * 1024u * 1024u)
#define PILOT_MODULE_MAX_TOTAL_BYTES (8u * 1024u * 1024u)
#define PILOT_MODULE_STAGE_DIR "/tmp_wbshd/pilot/modules"
#define PILOT_FNV64_OFFSET 14695981039346656037ull
#define PILOT_FNV64_PRIME 1099511628211ull

struct json_cursor {
    const char *cursor;
    const char *end;
};

static void json_skip_space(struct json_cursor *json)
{
    while (json->cursor < json->end &&
           (*json->cursor == ' ' || *json->cursor == '\t' ||
            *json->cursor == '\r' || *json->cursor == '\n'))
        ++json->cursor;
}

static int json_expect(struct json_cursor *json, char expected)
{
    json_skip_space(json);
    if (json->cursor == json->end || *json->cursor != expected) return 0;
    ++json->cursor;
    return 1;
}

static int parse_modules_json(const char *data,
                              size_t length,
                              char names[PILOT_MODULE_MAX_COUNT]
                                         [PILOT_MODULE_NAME_CAPACITY],
                              size_t *count_out)
{
    struct json_cursor json = {data, data + length};
    size_t count = 0;
    if (!json_expect(&json, '{')) return 0;

    json_skip_space(&json);
    if (json.cursor == json.end || *json.cursor++ != '"') return 0;
    static const char key[] = "modules";
    for (size_t i = 0; i < sizeof(key) - 1; ++i) {
        if (json.cursor == json.end || *json.cursor++ != key[i]) return 0;
    }
    if (!json_expect(&json, '"') || !json_expect(&json, ':') ||
        !json_expect(&json, '['))
        return 0;

    json_skip_space(&json);
    while (json.cursor < json.end && *json.cursor != ']') {
        if (count >= PILOT_MODULE_MAX_COUNT || !json_expect(&json, '"'))
            return 0;
        size_t name_length = 0;
        while (json.cursor < json.end && *json.cursor != '"') {
            unsigned char ch = (unsigned char)*json.cursor++;
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ||
                  ch == '.'))
                return 0;
            if (name_length + 1 >= PILOT_MODULE_NAME_CAPACITY) return 0;
            names[count][name_length++] = (char)ch;
        }
        if (json.cursor == json.end || *json.cursor++ != '"' ||
            name_length < 4 ||
            strcmp(names[count] + name_length - 3, ".so") != 0 ||
            names[count][0] == '.' || strstr(names[count], ".."))
            return 0;
        names[count][name_length] = '\0';
        ++count;

        json_skip_space(&json);
        if (json.cursor < json.end && *json.cursor == ',') {
            ++json.cursor;
            json_skip_space(&json);
            continue;
        }
        break;
    }

    if (!json_expect(&json, ']') || !json_expect(&json, '}')) return 0;
    json_skip_space(&json);
    if (json.cursor != json.end) return 0;
    *count_out = count;
    return 1;
}

static int module_data_name(const char *filename,
                            char name[PILOT_MODULE_NAME_CAPACITY])
{
    size_t length = filename ? strlen(filename) : 0;
    if (length <= 3 || strcmp(filename + length - 3, ".so") != 0 ||
        length - 3 >= PILOT_MODULE_NAME_CAPACITY)
        return 0;
    memcpy(name, filename, length - 3);
    name[length - 3] = '\0';
    return 1;
}

static int valid_filename(const char *name)
{
    if (!name || !*name || name[0] == '.' || strstr(name, "..")) return 0;
    size_t length = strlen(name);
    if (length < 4 || length >= PILOT_MODULE_NAME_CAPACITY ||
        strcmp(name + length - 3, ".so") != 0)
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

static void module_log(const char *message)
{
    const PilotApi *api = pilot_a6700_get_api();
    if (api && api->log && api->log->print)
        (void)api->log->print(message);
}

static int ensure_directory(const char *path)
{
    if (mkdir(path, 0700) == 0) return 1;
    int error = errno;
    if (error != EEXIST) return 0;

    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return 0;
    if (close(fd) == 0) return 1;
    return 0;
}

static int ensure_stage_directory(void)
{
    return ensure_directory("/tmp_wbshd/pilot") &&
           ensure_directory(PILOT_MODULE_STAGE_DIR);
}

static void log_system_error(const char *operation, const char *path, int error)
{
    char message[256];
    int length = snprintf(message, sizeof(message),
                          "Pilot %s failed path=%s errno=%d",
                          operation, path, error);
    if (length > 0 && (size_t)length < sizeof(message))
        module_log(message);
}

static uint64_t package_hash_update(uint64_t hash,
                                    const void *data,
                                    size_t length)
{
    const unsigned char *bytes = data;
    for (size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= PILOT_FNV64_PRIME;
    }
    return hash;
}

static PilotStatus stage_module(const char *name,
                                char destination[160],
                                size_t *staged_total_bytes,
                                uint64_t *package_hash)
{
    if (!valid_filename(name) || !staged_total_bytes ||
        !package_hash ||
        *staged_total_bytes > PILOT_MODULE_MAX_TOTAL_BYTES)
        return PILOT_STATUS_INVALID_ARGUMENT;

    *package_hash = package_hash_update(*package_hash, name, strlen(name) + 1);

    char source[160];
    int source_size = snprintf(source, sizeof(source), "%c:/pilot/%s",
                               g_pilot.drive_letter, name);
    int destination_size = snprintf(destination, 160,
                                    PILOT_MODULE_STAGE_DIR "/%s",
                                    name);
    if (source_size <= 0 || (size_t)source_size >= sizeof(source) ||
        destination_size <= 0 || destination_size >= 160)
        return PILOT_STATUS_RANGE;

    void *input = 0;
    int open_status = g_pilot.fsys.open_file(source, &input, 1);
    if (open_status != 0 || !input) {
        (void)pilot_fsys_is_not_found(&g_pilot.fsys, open_status);
        char message[224];
        int length = snprintf(message, sizeof(message),
                              "Pilot source open failed path=%s fsys=0x%08x",
                              source, (unsigned int)open_status);
        if (length > 0 && (size_t)length < sizeof(message))
            module_log(message);
        return PILOT_STATUS_IO;
    }

    int output = open(destination, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (output < 0) {
        int error = errno;
        (void)g_pilot.fsys.close_file(input);
        log_system_error("stage open", destination, error);
        return PILOT_STATUS_IO;
    }

    char buffer[4096];
    size_t total = 0;
    PilotStatus result = PILOT_STATUS_OK;
    for (;;) {
        int count = g_pilot.fsys.read_file(input, buffer, sizeof(buffer));
        if (count < 0) {
            module_log("Pilot source read failed");
            result = PILOT_STATUS_IO;
            break;
        }
        if (count == 0) break;
        if ((size_t)count > sizeof(buffer) ||
            total > PILOT_MODULE_MAX_BYTES - (size_t)count ||
            total > PILOT_MODULE_MAX_TOTAL_BYTES - *staged_total_bytes ||
            (size_t)count > PILOT_MODULE_MAX_TOTAL_BYTES -
                                *staged_total_bytes - total) {
            module_log("Pilot modules exceed 2 MiB per-module or 8 MiB total staging limit");
            result = PILOT_STATUS_RANGE;
            break;
        }

        *package_hash = package_hash_update(*package_hash, buffer,
                                            (size_t)count);

        size_t remaining = (size_t)count;
        const char *cursor = buffer;
        while (remaining) {
            ssize_t written = write(output, cursor, remaining);
            if (written <= 0) {
                int error = errno;
                log_system_error("stage write", destination, error);
                result = PILOT_STATUS_IO;
                break;
            }
            cursor += written;
            remaining -= (size_t)written;
        }
        if (result != PILOT_STATUS_OK) break;
        total += (size_t)count;
    }

    if (result == PILOT_STATUS_OK && fsync(output) != 0) {
        log_system_error("stage fsync", destination, errno);
        result = PILOT_STATUS_IO;
    }
    if (g_pilot.fsys.close_file(input) != 0 && result == PILOT_STATUS_OK)
    {
        module_log("Pilot source close failed");
        result = PILOT_STATUS_IO;
    }
    if (close(output) != 0 && result == PILOT_STATUS_OK) {
        log_system_error("stage close", destination, errno);
        result = PILOT_STATUS_IO;
    }
    if (result != PILOT_STATUS_OK) {
        (void)unlink(destination);
    } else {
        *staged_total_bytes += total;
    }
    return result;
}

void pilot_load_modules(void)
{
    if (!__atomic_load_n(&g_pilot.optional_features_enabled, __ATOMIC_ACQUIRE))
        return;

    char manifest_path[64];
    int path_size = snprintf(manifest_path, sizeof(manifest_path),
                             "%c:/pilot/modules.json", g_pilot.drive_letter);
    if (path_size <= 0 || (size_t)path_size >= sizeof(manifest_path)) {
        __atomic_store_n(&g_pilot.optional_features_enabled, 0, __ATOMIC_RELEASE);
        return;
    }

    char manifest[PILOT_MANIFEST_CAPACITY];
    size_t manifest_length = 0;
    int not_found = 0;
    PilotStatus read_status = pilot_fsys_read_all(&g_pilot.fsys,
                                                  manifest_path,
                                                  manifest,
                                                  sizeof(manifest) - 1,
                                                  &manifest_length,
                                                  &not_found);
    if (read_status != PILOT_STATUS_OK) {
        if (not_found) {
            module_log("Pilot modules.json not found; no modules loaded");
            if (pilot_fuse_append_state(PILOT_FUSE_STATE_COMPLETE,
                                        PILOT_FNV64_OFFSET) != PILOT_STATUS_OK)
                module_log("Pilot module fuse completion record failed");
            return;
        }
        __atomic_store_n(&g_pilot.optional_features_enabled, 0, __ATOMIC_RELEASE);
        return;
    }
    manifest[manifest_length] = '\0';

    char names[PILOT_MODULE_MAX_COUNT][PILOT_MODULE_NAME_CAPACITY] = {{0}};
    size_t count = 0;
    if (!parse_modules_json(manifest, manifest_length, names, &count)) {
        module_log("Pilot modules.json rejected; optional features disabled");
        __atomic_store_n(&g_pilot.optional_features_enabled, 0, __ATOMIC_RELEASE);
        return;
    }

    uint64_t package_hash = package_hash_update(PILOT_FNV64_OFFSET,
                                               manifest, manifest_length);
    if (count == 0) {
        if (pilot_fuse_append_state(PILOT_FUSE_STATE_COMPLETE,
                                    package_hash) != PILOT_STATUS_OK)
            module_log("Pilot module fuse completion record failed");
        return;
    }

    if (!ensure_stage_directory()) {
        log_system_error("stage directory", PILOT_MODULE_STAGE_DIR, errno);
        __atomic_store_n(&g_pilot.optional_features_enabled, 0, __ATOMIC_RELEASE);
        return;
    }
    size_t staged_total_bytes = 0;
    char staged_paths[PILOT_MODULE_MAX_COUNT][160] = {{0}};
    for (size_t i = 0; i < count; ++i) {
        if (!__atomic_load_n(&g_pilot.optional_features_enabled, __ATOMIC_ACQUIRE))
            return;

        PilotStatus stage_status = stage_module(names[i], staged_paths[i],
                                                &staged_total_bytes,
                                                &package_hash);
        if (stage_status != PILOT_STATUS_OK) {
            char message[192];
            int length = snprintf(message, sizeof(message),
                                  "Pilot module staging failed name=%s status=0x%08x; remaining modules skipped",
                                  names[i], (unsigned int)stage_status);
            if (length > 0 && (size_t)length < sizeof(message))
                module_log(message);
            __atomic_store_n(&g_pilot.optional_features_enabled, 0, __ATOMIC_RELEASE);
            return;
        }
    }

    enum pilot_fuse_state previous_state = PILOT_FUSE_STATE_INVALID;
    uint64_t previous_package = 0;
    PilotStatus fuse_status = pilot_fuse_read_state(&previous_state,
                                                    &previous_package);
    if (fuse_status != PILOT_STATUS_OK ||
        previous_state == PILOT_FUSE_STATE_INVALID) {
        (void)pilot_fuse_append_state(PILOT_FUSE_STATE_TRIPPED, package_hash);
        module_log("Pilot module fuse state unreadable; optional modules disabled");
        __atomic_store_n(&g_pilot.optional_features_enabled, 0, __ATOMIC_RELEASE);
        return;
    }
    if (previous_package == package_hash &&
        (previous_state == PILOT_FUSE_STATE_ARMED ||
         previous_state == PILOT_FUSE_STATE_TRIPPED)) {
        if (previous_state == PILOT_FUSE_STATE_ARMED)
            (void)pilot_fuse_append_state(PILOT_FUSE_STATE_TRIPPED,
                                          package_hash);
        module_log("Pilot module fuse tripped; this module package is quarantined");
        __atomic_store_n(&g_pilot.optional_features_enabled, 0, __ATOMIC_RELEASE);
        return;
    }

    if (pilot_fuse_append_state(PILOT_FUSE_STATE_ARMED,
                                package_hash) != PILOT_STATUS_OK) {
        module_log("Pilot module fuse arm record failed; modules not loaded");
        __atomic_store_n(&g_pilot.optional_features_enabled, 0, __ATOMIC_RELEASE);
        return;
    }

    int traversal_complete = 1;
    for (size_t i = 0; i < count; ++i) {
        if (!__atomic_load_n(&g_pilot.optional_features_enabled, __ATOMIC_ACQUIRE)) {
            traversal_complete = 0;
            break;
        }

        void *handle = dlopen(staged_paths[i], RTLD_NOW | RTLD_GLOBAL);
        if (!handle) {
            const char *error = dlerror();
            char message[256];
            int length = snprintf(message, sizeof(message),
                                  "Pilot dlopen failed path=%s error=%s",
                                  staged_paths[i], error ? error : "unknown");
            if (length > 0 && (size_t)length < sizeof(message))
                module_log(message);
            __atomic_store_n(&g_pilot.optional_features_enabled, 0, __ATOMIC_RELEASE);
            traversal_complete = 0;
            break;
        }

        void *symbol = dlsym(handle, PILOT_MODULE_INIT_SYMBOL);
        PilotModuleInit init = 0;
        if (symbol && sizeof(init) == sizeof(symbol))
            memcpy(&init, &symbol, sizeof(init));
        if (!init) {
            module_log("Pilot module init symbol missing; remaining modules skipped");
            __atomic_store_n(&g_pilot.optional_features_enabled, 0, __ATOMIC_RELEASE);
            g_pilot.module_handles[g_pilot.module_count++] = handle;
            traversal_complete = 0;
            break;
        }

        char data_name[PILOT_MODULE_NAME_CAPACITY];
        const PilotApi *module_api = 0;
        PilotStatus api_status = module_data_name(names[i], data_name)
            ? pilot_api_for_module(data_name, &module_api)
            : PILOT_STATUS_INVALID_ARGUMENT;
        if (api_status != PILOT_STATUS_OK || !module_api) {
            char message[192];
            int length = snprintf(message, sizeof(message),
                                  "Pilot module API binding failed name=%s status=%d",
                                  names[i], api_status);
            if (length > 0 && (size_t)length < sizeof(message))
                module_log(message);
            __atomic_store_n(&g_pilot.optional_features_enabled, 0, __ATOMIC_RELEASE);
            g_pilot.module_handles[g_pilot.module_count++] = handle;
            traversal_complete = 0;
            break;
        }

        if (init(module_api) != PILOT_STATUS_OK) {
            module_log("Pilot module initialization failed; remaining modules skipped");
            __atomic_store_n(&g_pilot.optional_features_enabled, 0, __ATOMIC_RELEASE);
            g_pilot.module_handles[g_pilot.module_count++] = handle;
            traversal_complete = 0;
            break;
        }

        if (g_pilot.module_count < PILOT_MODULE_MAX_COUNT)
            g_pilot.module_handles[g_pilot.module_count++] = handle;
        else {
            module_log("Pilot module limit reached; remaining modules skipped");
            __atomic_store_n(&g_pilot.optional_features_enabled, 0, __ATOMIC_RELEASE);
            traversal_complete = 0;
            break;
        }

        module_log("Pilot module initialized");
    }

    if (traversal_complete &&
        pilot_fuse_append_state(PILOT_FUSE_STATE_COMPLETE,
                                package_hash) != PILOT_STATUS_OK)
        module_log("Pilot module fuse completion record failed");
}
