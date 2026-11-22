#ifndef PILOT_PATCH_RUNTIME_H
#define PILOT_PATCH_RUNTIME_H

#include "pilot.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PILOT_PATCH_MAX_SITES 128u
#define PILOT_PATCH_MAX_LENGTH 64u

typedef enum PilotPatchSpace {
    PILOT_PATCH_SPACE_AP = 0,
    PILOT_PATCH_SPACE_CP = 1
} PilotPatchSpace;

typedef struct PilotPatchSite {
    const char *id;
    PilotPatchSpace space;
    uint64_t address;
    const uint8_t *before;
    const uint8_t *after;
    size_t length;
} PilotPatchSite;

typedef struct PilotPatchReport {
    size_t checked;
    size_t changed;
    size_t attempted;
    size_t failed_index;
    PilotStatus status;
} PilotPatchReport;

PilotStatus pilot_patch_apply_sites(const PilotApi *api,
                                    const PilotPatchSite *sites,
                                    size_t count,
                                    int restore,
                                    PilotPatchReport *report);

#ifdef __cplusplus
}
#endif

#endif
