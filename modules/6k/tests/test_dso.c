#define _GNU_SOURCE
#include "../../../patch/pilot.h"
#include <assert.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static unsigned threads;
static void *(*callbacks[2])(void *);
/* Capture the real two backends' callbacks without executing device accesses. */
int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                    void *(*callback)(void *), void *arg)
{
    (void)attr;
    assert(threads < 2 && arg == NULL);
    callbacks[threads] = callback;
    *thread = ++threads;
    return 0;
}
static PilotStatus log_print(const char *line) { puts(line); return 0; }
int main(int argc, char **argv)
{
    assert(argc == 2);
    void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!handle) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    void *symbol = dlsym(handle, "pilot_module_init");
    PilotModuleInit init = NULL;
    assert(sizeof(init) == sizeof(symbol));
    memcpy(&init, &symbol, sizeof(init));
    assert(init);
    assert(dlsym(handle, "pilot_6k_encoder_init") == NULL);
    assert(dlsym(handle, "pilot_6k_buffers_init") == NULL);
    const PilotLogApi log = {log_print};
    PilotApi api = {.abi_version=PILOT_ABI_VERSION, .struct_size=sizeof(PilotApi), .feature_flags=PILOT_FEATURE_LOG, .log=&log};
    assert(init(&api) == PILOT_STATUS_OK && threads == 2);
    assert(callbacks[0] != callbacks[1]);
    assert(init(&api) == PILOT_STATUS_OK && threads == 2);
    puts("DSO_INIT_OK callbacks=2 hardware_accesses=0");
    return 0;
}
