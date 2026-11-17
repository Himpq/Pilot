#ifndef PILOT_H
#define PILOT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PILOT_ABI_VERSION_1 1u
#define PILOT_MODULE_INIT_SYMBOL "pilot_module_init"

typedef int32_t PilotStatus;

enum {
    PILOT_STATUS_OK = 0,
    PILOT_STATUS_DISABLED = -1,
    PILOT_STATUS_INVALID_ARGUMENT = -2,
    PILOT_STATUS_RANGE = -3,
    PILOT_STATUS_CONFLICT = -4,
    PILOT_STATUS_IO = -5,
    PILOT_STATUS_SYSTEM = -6,
    PILOT_STATUS_UNSUPPORTED = -7
};

enum {
    PILOT_FEATURE_LOG = 1u << 0,
    PILOT_FEATURE_TIME_UTC = 1u << 1,
    PILOT_FEATURE_APPFW_MEMORY = 1u << 2,
    PILOT_FEATURE_MODULE_DIR = 1u << 3
};

typedef struct PilotApiV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t feature_flags;
    uint32_t reserved;

    PilotStatus (*log_write)(const char *message, size_t length);
    PilotStatus (*time_utc)(char *buffer, size_t capacity);
    PilotStatus (*memory_read)(uintptr_t address, void *output, size_t length);
    PilotStatus (*memory_patch)(uintptr_t address,
                                const void *expected,
                                const void *replacement,
                                size_t length);
    /* Creates the card-side module data directory and returns its Fsys path
     * with a trailing slash, for example X:/pilot/modules/example/.
     * module_name accepts ASCII letters, digits, underscore, dash, and dot;
     * it cannot start with a dot or contain '..'.
     */
    PilotStatus (*get_module_dir)(const char *module_name,
                                  char *buffer,
                                  size_t capacity);
} PilotApiV1;

typedef PilotStatus (*PilotModuleInitV1)(const PilotApiV1 *api);

const PilotApiV1 *pilot_a6700_v1_get_api(void);

#ifdef __cplusplus
}
#endif

#endif
