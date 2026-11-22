#include "../../../patch/pilot.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

PilotStatus pilot_module_init(const PilotApi *api);
static unsigned calls;
static const char *scenario;
PilotStatus pilot_6k_encoder_init(const PilotApi *api)
{
    assert(api && calls == 0);
    ++calls;
    return strcmp(scenario, "encoder_fail") == 0 ? PILOT_STATUS_SYSTEM : PILOT_STATUS_OK;
}
PilotStatus pilot_6k_buffers_init(const PilotApi *api)
{
    assert(api && calls == 1);
    ++calls;
    return strcmp(scenario, "buffers_fail") == 0 ? PILOT_STATUS_SYSTEM : PILOT_STATUS_OK;
}
static PilotStatus log_print(const char *line) { assert(strstr(line, "PILOT_6K_INIT") == line); return 0; }
int main(int argc, char **argv)
{
    assert(argc == 2);
    scenario = argv[1];
    const PilotLogApi log = {log_print};
    PilotApi api = {.abi_version=PILOT_ABI_VERSION, .struct_size=sizeof(PilotApi), .feature_flags=PILOT_FEATURE_LOG, .log=&log};
    assert(pilot_module_init(NULL) == PILOT_STATUS_UNSUPPORTED);
    api.feature_flags = 0;
    assert(pilot_module_init(&api) == PILOT_STATUS_DISABLED && calls == 0);
    api.feature_flags = PILOT_FEATURE_LOG;
    PilotStatus expected = strcmp(scenario, "ok") == 0 ? PILOT_STATUS_OK : PILOT_STATUS_SYSTEM;
    assert(pilot_module_init(&api) == expected);
    assert(calls == (strcmp(scenario, "encoder_fail") == 0 ? 1u : 2u));
    unsigned previous = calls;
    assert(pilot_module_init(&api) == expected && calls == previous);
    printf("ENTRY_OK scenario=%s calls=%u\n", scenario, calls);
    return 0;
}
