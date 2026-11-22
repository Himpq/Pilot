#include "pilot_patch_runtime.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t ap_memory[256];
static uint8_t cp_memory[256];
static size_t patch_calls;
static size_t fail_patch_call;
static size_t corrupt_read_call;
static size_t read_calls;
static uint64_t patch_addresses[16];
static PilotPatchSpace patch_spaces[16];

static uint8_t *memory_for(PilotPatchSpace space, uint64_t address)
{
    assert(address < 256);
    return space == PILOT_PATCH_SPACE_AP ? ap_memory + address
                                         : cp_memory + address;
}

static PilotStatus fake_read(PilotPatchSpace space, uint64_t address,
                             void *output, size_t length)
{
    read_calls++;
    memcpy(output, memory_for(space, address), length);
    if (corrupt_read_call && read_calls == corrupt_read_call)
        ((uint8_t *)output)[0] ^= 0xff;
    return PILOT_STATUS_OK;
}

static PilotStatus fake_ap_read(uintptr_t address, void *output, size_t length)
{
    return fake_read(PILOT_PATCH_SPACE_AP, address, output, length);
}

static PilotStatus fake_cp_read(uint64_t address, void *output, size_t length)
{
    return fake_read(PILOT_PATCH_SPACE_CP, address, output, length);
}

static PilotStatus fake_patch(PilotPatchSpace space, uint64_t address,
                              const void *expected, const void *replacement,
                              size_t length)
{
    uint8_t *target = memory_for(space, address);
    size_t call = patch_calls++;
    patch_addresses[call] = address;
    patch_spaces[call] = space;
    if (call == fail_patch_call)
        return PILOT_STATUS_IO;
    if (memcmp(target, expected, length) != 0)
        return PILOT_STATUS_CONFLICT;
    memcpy(target, replacement, length);
    return PILOT_STATUS_OK;
}

static PilotStatus fake_ap_patch(uintptr_t address, const void *expected,
                                 const void *replacement, size_t length)
{
    return fake_patch(PILOT_PATCH_SPACE_AP, address, expected, replacement,
                      length);
}

static PilotStatus fake_cp_patch(uint64_t address, const void *expected,
                                 const void *replacement, size_t length)
{
    return fake_patch(PILOT_PATCH_SPACE_CP, address, expected, replacement,
                      length);
}

static PilotStatus fake_log(const char *message)
{
    assert(message && strstr(message, "pilot_patch id=") == message);
    return PILOT_STATUS_OK;
}

static const PilotLogApi log_api = {fake_log};
static PilotCpMemApi cp_api = {
    .struct_size = sizeof(PilotCpMemApi),
    .read = fake_cp_read,
    .patch = fake_cp_patch,
};
static PilotApi api = {
    .abi_version = PILOT_ABI_VERSION,
    .struct_size = sizeof(PilotApi),
    .feature_flags = PILOT_FEATURE_LOG | PILOT_FEATURE_APPFW_MEMORY |
                     PILOT_FEATURE_CP_MEMORY,
    .memory_read = fake_ap_read,
    .memory_patch = fake_ap_patch,
    .log = &log_api,
    .cp_mem = &cp_api,
};

static const uint8_t before_a[] = {1, 2};
static const uint8_t after_a[] = {11, 12};
static const uint8_t before_b[] = {3, 4};
static const uint8_t after_b[] = {13, 14};

static PilotPatchSite make_site(const char *id, PilotPatchSpace space,
                                uint64_t address, const uint8_t *before,
                                const uint8_t *after)
{
    PilotPatchSite site = {id, space, address, before, after, 2};
    return site;
}

static void reset_state(void)
{
    memset(ap_memory, 0, sizeof(ap_memory));
    memset(cp_memory, 0, sizeof(cp_memory));
    memcpy(ap_memory + 10, before_a, sizeof(before_a));
    memcpy(cp_memory + 20, before_b, sizeof(before_b));
    patch_calls = 0;
    fail_patch_call = SIZE_MAX;
    corrupt_read_call = 0;
    read_calls = 0;
    memset(patch_addresses, 0, sizeof(patch_addresses));
}

static void test_preflight_conflict_has_zero_writes(void)
{
    PilotPatchSite sites[] = {
        make_site("ap", PILOT_PATCH_SPACE_AP, 10, before_a, after_a),
        make_site("cp", PILOT_PATCH_SPACE_CP, 20, before_a, after_b),
    };
    PilotPatchReport report;
    reset_state();
    assert(pilot_patch_apply_sites(&api, sites, 2, 0, &report) ==
           PILOT_STATUS_CONFLICT);
    assert(report.checked == 2 && report.changed == 0 &&
           report.failed_index == 1 && patch_calls == 0);
}

static void test_apply_and_restore_reverse(void)
{
    PilotPatchSite sites[] = {
        make_site("ap", PILOT_PATCH_SPACE_AP, 10, before_a, after_a),
        make_site("cp", PILOT_PATCH_SPACE_CP, 20, before_b, after_b),
    };
    PilotPatchReport report;
    reset_state();
    assert(pilot_patch_apply_sites(&api, sites, 2, 0, &report) ==
           PILOT_STATUS_OK);
    assert(report.checked == 2 && report.changed == 2 &&
           report.failed_index == SIZE_MAX);
    assert(memcmp(ap_memory + 10, after_a, 2) == 0);
    assert(memcmp(cp_memory + 20, after_b, 2) == 0);
    patch_calls = 0;
    assert(pilot_patch_apply_sites(&api, sites, 2, 1, &report) ==
           PILOT_STATUS_OK);
    assert(patch_calls == 2 && patch_addresses[0] == 20 &&
           patch_spaces[0] == PILOT_PATCH_SPACE_CP &&
           patch_addresses[1] == 10 &&
           patch_spaces[1] == PILOT_PATCH_SPACE_AP);
    assert(memcmp(ap_memory + 10, before_a, 2) == 0);
    assert(memcmp(cp_memory + 20, before_b, 2) == 0);
}

static void test_midway_failure_counts_completed_writes(void)
{
    PilotPatchSite sites[] = {
        make_site("first", PILOT_PATCH_SPACE_AP, 10, before_a, after_a),
        make_site("second", PILOT_PATCH_SPACE_CP, 20, before_b, after_b),
    };
    PilotPatchReport report;
    reset_state();
    fail_patch_call = 1;
    assert(pilot_patch_apply_sites(&api, sites, 2, 0, &report) ==
           PILOT_STATUS_IO);
    assert(report.checked == 2 && report.changed == 1 &&
           report.failed_index == 1 && patch_calls == 2);
    assert(memcmp(ap_memory + 10, after_a, 2) == 0);
    assert(memcmp(cp_memory + 20, before_b, 2) == 0);
}

static void test_readback_failure_counts_mutation(void)
{
    PilotPatchSite site = make_site("readback", PILOT_PATCH_SPACE_AP, 10,
                                    before_a, after_a);
    PilotPatchReport report;
    reset_state();
    /* One preflight read, then one readback read. */
    corrupt_read_call = 2;
    assert(pilot_patch_apply_sites(&api, &site, 1, 0, &report) ==
           PILOT_STATUS_IO);
    assert(report.checked == 1 && report.changed == 1 &&
           report.failed_index == 0);
}

static void test_overlap_overflow_and_missing_feature(void)
{
    PilotPatchSite overlap[] = {
        make_site("a", PILOT_PATCH_SPACE_AP, 10, before_a, after_a),
        make_site("b", PILOT_PATCH_SPACE_AP, 11, before_b, after_b),
    };
    PilotPatchSite overflow = make_site("overflow", PILOT_PATCH_SPACE_CP,
                                        UINT64_MAX - 1, before_b, after_b);
    PilotPatchSite cp = make_site("cp", PILOT_PATCH_SPACE_CP, 20,
                                  before_b, after_b);
    PilotPatchReport report;
    uint32_t saved_flags = api.feature_flags;
    reset_state();
    assert(pilot_patch_apply_sites(&api, overlap, 2, 0, &report) ==
           PILOT_STATUS_CONFLICT);
    assert(report.failed_index == 1 && patch_calls == 0);
    assert(pilot_patch_apply_sites(&api, &overflow, 1, 0, &report) ==
           PILOT_STATUS_INVALID_ARGUMENT);
    assert(patch_calls == 0);
    api.feature_flags &= ~PILOT_FEATURE_CP_MEMORY;
    assert(pilot_patch_apply_sites(&api, &cp, 1, 0, &report) ==
           PILOT_STATUS_DISABLED);
    assert(patch_calls == 0 && report.checked == 0);
    api.feature_flags = saved_flags;
}

static void test_api_and_feature_rejections(void)
{
    PilotPatchSite ap = make_site("ap", PILOT_PATCH_SPACE_AP, 10,
                                  before_a, after_a);
    PilotPatchReport report;
    PilotApi broken = api;
    reset_state();
    broken.memory_patch = NULL;
    assert(pilot_patch_apply_sites(&broken, &ap, 1, 0, &report) ==
           PILOT_STATUS_UNSUPPORTED);
    assert(report.checked == 0 && patch_calls == 0);

    broken = api;
    broken.struct_size = (uint32_t)offsetof(PilotApi, cp_mem);
    assert(pilot_patch_apply_sites(&broken, &ap, 1, 0, &report) ==
           PILOT_STATUS_UNSUPPORTED);
    assert(report.checked == 0 && patch_calls == 0);
}

int main(void)
{
    test_preflight_conflict_has_zero_writes();
    test_apply_and_restore_reverse();
    test_midway_failure_counts_completed_writes();
    test_readback_failure_counts_mutation();
    test_overlap_overflow_and_missing_feature();
    test_api_and_feature_rejections();
    puts("pilot patch runtime tests passed");
    return 0;
}
