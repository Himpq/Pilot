#include "pilot_internal.h"

#include <stdio.h>
#include <string.h>

static PilotStatus api_log_write(const char *message, size_t length)
{
    if (!message || !length || length > 768)
        return PILOT_STATUS_INVALID_ARGUMENT;
    if (!__atomic_load_n(&g_pilot.optional_features_enabled, __ATOMIC_ACQUIRE))
        return PILOT_STATUS_DISABLED;

    char line[896];
    char timestamp[32];
    size_t used = 0;
    if (pilot_time_utc_impl(timestamp, sizeof(timestamp)) == PILOT_STATUS_OK) {
        int count = snprintf(line, sizeof(line), "%s ", timestamp);
        if (count <= 0 || (size_t)count >= sizeof(line))
            return PILOT_STATUS_RANGE;
        used = (size_t)count;
    }

    if (length > sizeof(line) - used - 2) return PILOT_STATUS_RANGE;
    memcpy(line + used, message, length);
    used += length;
    if (line[used - 1] != '\n') line[used++] = '\n';
    return pilot_log_append(line, used, 1);
}

static PilotApiV1 g_api = {
    .abi_version = PILOT_ABI_VERSION_1,
    .struct_size = sizeof(PilotApiV1),
    .feature_flags = 0,
    .reserved = 0,
    .log_write = api_log_write,
    .time_utc = pilot_time_utc_impl,
    .memory_read = pilot_memory_read_impl,
    .memory_patch = pilot_memory_patch_impl,
    .get_module_dir = pilot_get_module_dir_impl,
};

__attribute__((visibility("default"), used))
const PilotApiV1 *pilot_v1_get_api(void)
{
    uint32_t features = 0;
    if (__atomic_load_n(&g_pilot.optional_features_enabled, __ATOMIC_ACQUIRE)) {
        features = PILOT_FEATURE_LOG | PILOT_FEATURE_TIME_UTC |
                   PILOT_FEATURE_APPFW_MEMORY | PILOT_FEATURE_MODULE_DIR;
    }
    g_api.feature_flags = features;
    return &g_api;
}
