#define _GNU_SOURCE
#include "pilot_internal.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

PilotStatus pilot_time_utc_impl(char *buffer, size_t capacity)
{
    if (!buffer || capacity < 21) return PILOT_STATUS_INVALID_ARGUMENT;
    if (!g_pilot.optional_features_enabled || !g_pilot.time_feature_enabled)
        return PILOT_STATUS_DISABLED;

    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) return PILOT_STATUS_SYSTEM;

    struct tm utc;
    if (!gmtime_r(&now.tv_sec, &utc)) return PILOT_STATUS_SYSTEM;
    if (strftime(buffer, capacity, "%Y-%m-%dT%H:%M:%SZ", &utc) == 0)
        return PILOT_STATUS_RANGE;
    return PILOT_STATUS_OK;
}
