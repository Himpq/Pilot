#ifndef PILOT_H
#define PILOT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PILOT_ABI_VERSION 2u
#define PILOT_MODULE_INIT_SYMBOL "pilot_module_init"
#define PILOT_MODULE_FILE_TRANSFER_MAX (1024u * 1024u)
#define PILOT_CP_MEM_TRANSFER_MAX 4096u

typedef int32_t PilotStatus;

enum {
    PILOT_STATUS_OK = 0,
    PILOT_STATUS_DISABLED = -1,
    PILOT_STATUS_INVALID_ARGUMENT = -2,
    PILOT_STATUS_RANGE = -3,
    PILOT_STATUS_CONFLICT = -4,
    PILOT_STATUS_IO = -5,
    PILOT_STATUS_SYSTEM = -6,
    PILOT_STATUS_UNSUPPORTED = -7,
    PILOT_STATUS_NOT_FOUND = -8
};

enum {
    PILOT_FEATURE_LOG = 1u << 0,
    PILOT_FEATURE_TIME_UTC = 1u << 1,
    PILOT_FEATURE_APPFW_MEMORY = 1u << 2,
    PILOT_FEATURE_MODULE_DIR = 1u << 3,
    PILOT_FEATURE_MODULE_FILES = 1u << 4,
    PILOT_FEATURE_CP_MEMORY = 1u << 5
};

typedef struct PilotFileApi PilotFileApi;
typedef struct PilotLogApi PilotLogApi;
typedef struct PilotModuleLogApi PilotModuleLogApi;
typedef struct PilotCpMemApi PilotCpMemApi;

struct PilotFileApi {
    uint32_t struct_size;
    void *context;
    PilotStatus (*read)(const PilotFileApi *file,
                        const char *file_name,
                        char *buffer,
                        size_t capacity);
    PilotStatus (*write)(const PilotFileApi *file,
                         const char *file_name,
                         const char *text);
    PilotStatus (*append)(const PilotFileApi *file,
                          const char *file_name,
                          const char *text);
    PilotStatus (*read_bytes)(const PilotFileApi *file,
                              const char *file_name,
                              void *buffer,
                              size_t capacity,
                              size_t *length_out);
    PilotStatus (*write_bytes)(const PilotFileApi *file,
                               const char *file_name,
                               const void *data,
                               size_t length);
    PilotStatus (*append_bytes)(const PilotFileApi *file,
                                const char *file_name,
                                const void *data,
                                size_t length);
};

struct PilotLogApi {
    PilotStatus (*print)(const char *message);
};

struct PilotModuleLogApi {
    void *context;
    PilotStatus (*print)(const PilotModuleLogApi *log,
                         const char *message);
};

typedef void (*PilotCpMemCallback)(PilotStatus status, void *user_data);

/* CP RAM read/write/patch:
 * - Code/.rodata addresses 0x00128000..0x011D7FFF (excluding 0x00C12D68..0x00C12FBF)
 *   use the CP UIPC v3/v4 DACR patcher (route 0x90030001, status 0x00C12FA0).
 * - Other data addresses >= 0x01000000 use /dev/pcidma directly.
 * - write_async/patch_async deep-copy input buffers, run on a detached worker
 *   thread without blocking the caller, and invoke callback(status, user_data).
 *   Callers must ensure user_data outlives the callback (static/global/heap).
 */
struct PilotCpMemApi {
    uint32_t struct_size;
    uint32_t reserved;
    PilotStatus (*read)(uint64_t cp_address, void *output, size_t length);
    PilotStatus (*write)(uint64_t cp_address, const void *input, size_t length);
    PilotStatus (*patch)(uint64_t cp_address,
                         const void *expected,
                         const void *replacement,
                         size_t length);
    PilotStatus (*write_async)(uint64_t cp_address,
                               const void *input,
                               size_t length,
                               PilotCpMemCallback callback,
                               void *user_data);
    PilotStatus (*patch_async)(uint64_t cp_address,
                               const void *expected,
                               const void *replacement,
                               size_t length,
                               PilotCpMemCallback callback,
                               void *user_data);
};

typedef struct PilotApi {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t feature_flags;
    uint32_t reserved;

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
    /* Module-bound namespaces. file and mlog point to persistent tables
     * scoped to the module filename stem from modules.json. */
    const PilotFileApi *file;
    const PilotLogApi *log;
    const PilotModuleLogApi *mlog;
    const PilotCpMemApi *cp_mem;
} PilotApi;

typedef PilotStatus (*PilotModuleInit)(const PilotApi *api);

const PilotApi *pilot_a6700_get_api(void);

#ifdef __cplusplus
}
#endif

#endif
