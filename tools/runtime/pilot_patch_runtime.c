#include "pilot_patch_runtime.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static void report_init(PilotPatchReport *report)
{
    if (!report)
        return;
    report->checked = 0;
    report->changed = 0;
    report->attempted = 0;
    report->failed_index = SIZE_MAX;
    report->status = PILOT_STATUS_OK;
}

static PilotStatus fail(PilotPatchReport *report,
                        size_t index,
                        PilotStatus status)
{
    if (report) {
        report->failed_index = index;
        report->status = status;
    }
    return status;
}

static void log_site(const PilotApi *api,
                     const PilotPatchSite *site,
                     PilotStatus status)
{
    char line[192];
    const char *space = site->space == PILOT_PATCH_SPACE_AP ? "AP" : "CP";

    if (!api || !api->log || !api->log->print)
        return;
    (void)snprintf(line, sizeof(line),
                   "pilot_patch id=%s space=%s address=0x%" PRIx64 " status=%" PRId32,
                   site->id ? site->id : "(null)", space,
                   site->address, status);
    (void)api->log->print(line);
}

static int range_end(uint64_t address, size_t length, uint64_t *end_out)
{
    uint64_t n = (uint64_t)length;
    if ((size_t)n != length || n > UINT64_MAX - address)
        return 0;
    *end_out = address + n;
    return 1;
}

static const uint8_t *expected_bytes(const PilotPatchSite *site, int restore)
{
    return restore ? site->after : site->before;
}

static const uint8_t *replacement_bytes(const PilotPatchSite *site,
                                       int restore)
{
    return restore ? site->before : site->after;
}

static PilotStatus read_site(const PilotApi *api,
                             const PilotPatchSite *site,
                             void *buffer)
{
    if (site->space == PILOT_PATCH_SPACE_AP)
        return api->memory_read((uintptr_t)site->address, buffer, site->length);
    return api->cp_mem->read(site->address, buffer, site->length);
}

static PilotStatus patch_site(const PilotApi *api,
                              const PilotPatchSite *site,
                              const void *expected,
                              const void *replacement)
{
    if (site->space == PILOT_PATCH_SPACE_AP)
        return api->memory_patch((uintptr_t)site->address, expected,
                                 replacement, site->length);
    return api->cp_mem->patch(site->address, expected, replacement,
                              site->length);
}

PilotStatus pilot_patch_apply_sites(const PilotApi *api,
                                    const PilotPatchSite *sites,
                                    size_t count,
                                    int restore,
                                    PilotPatchReport *report)
{
    uint8_t current[PILOT_PATCH_MAX_LENGTH];
    size_t i;
    size_t order;
    uint32_t need_features = 0;
    int need_ap = 0;
    int need_cp = 0;
    PilotStatus status;

    report_init(report);
    if (!api || (!sites && count) || count == 0 ||
        count > PILOT_PATCH_MAX_SITES || (restore != 0 && restore != 1))
        return fail(report, SIZE_MAX, PILOT_STATUS_INVALID_ARGUMENT);
    if (api->abi_version != PILOT_ABI_VERSION ||
        api->struct_size < sizeof(PilotApi))
        return fail(report, SIZE_MAX, PILOT_STATUS_UNSUPPORTED);
    if (!(api->feature_flags & PILOT_FEATURE_LOG) ||
        !api->log || !api->log->print)
        return fail(report, SIZE_MAX, PILOT_STATUS_DISABLED);

    for (i = 0; i < count; ++i) {
        uint64_t end;
        if (!sites[i].id || !sites[i].before || !sites[i].after ||
            sites[i].length == 0 ||
            sites[i].length > PILOT_PATCH_MAX_LENGTH ||
            (sites[i].space != PILOT_PATCH_SPACE_AP &&
             sites[i].space != PILOT_PATCH_SPACE_CP) ||
            !range_end(sites[i].address, sites[i].length, &end))
            return fail(report, i, PILOT_STATUS_INVALID_ARGUMENT);
        if (sites[i].space == PILOT_PATCH_SPACE_AP) {
            if (sites[i].address > UINTPTR_MAX || end - 1 > UINTPTR_MAX)
                return fail(report, i, PILOT_STATUS_RANGE);
            need_ap = 1;
        } else {
            need_cp = 1;
        }
        if (i) {
            size_t j;
            for (j = 0; j < i; ++j) {
                uint64_t prev_end;
                if (sites[i].space != sites[j].space)
                    continue;
                if (!range_end(sites[j].address, sites[j].length, &prev_end))
                    return fail(report, j, PILOT_STATUS_RANGE);
                if (sites[i].address < prev_end && sites[j].address < end)
                    return fail(report, i, PILOT_STATUS_CONFLICT);
            }
        }
    }

    if (need_ap) {
        need_features |= PILOT_FEATURE_APPFW_MEMORY;
        if (!api->memory_read || !api->memory_patch)
            return fail(report, SIZE_MAX, PILOT_STATUS_UNSUPPORTED);
    }
    if (need_cp) {
        need_features |= PILOT_FEATURE_CP_MEMORY;
        if (!api->cp_mem ||
            api->cp_mem->struct_size < offsetof(PilotCpMemApi, patch) +
                                           sizeof(api->cp_mem->patch) ||
            !api->cp_mem->read || !api->cp_mem->patch)
            return fail(report, SIZE_MAX, PILOT_STATUS_UNSUPPORTED);
    }
    if ((api->feature_flags & need_features) != need_features)
        return fail(report, SIZE_MAX, PILOT_STATUS_DISABLED);

    for (i = 0; i < count; ++i) {
        status = read_site(api, &sites[i], current);
        if (status != PILOT_STATUS_OK) {
            log_site(api, &sites[i], status);
            return fail(report, i, status);
        }
        if (report)
            report->checked++;
        if (memcmp(current, expected_bytes(&sites[i], restore),
                   sites[i].length) != 0) {
            status = PILOT_STATUS_CONFLICT;
            log_site(api, &sites[i], status);
            return fail(report, i, status);
        }
    }

    for (order = 0; order < count; ++order) {
        i = restore ? count - 1 - order : order;
        if (report) report->attempted++;
        status = patch_site(api, &sites[i],
                            expected_bytes(&sites[i], restore),
                            replacement_bytes(&sites[i], restore));
        log_site(api, &sites[i], status);
        if (status != PILOT_STATUS_OK)
            return fail(report, i, status);
        if (report)
            report->changed++;

        status = read_site(api, &sites[i], current);
        if (status == PILOT_STATUS_OK &&
            memcmp(current, replacement_bytes(&sites[i], restore),
                   sites[i].length) != 0)
            status = PILOT_STATUS_IO;
        if (status != PILOT_STATUS_OK) {
            log_site(api, &sites[i], status);
            return fail(report, i, status);
        }
    }

    if (report)
        report->status = PILOT_STATUS_OK;
    return PILOT_STATUS_OK;
}
