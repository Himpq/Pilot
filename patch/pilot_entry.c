#define _GNU_SOURCE
#include "pilot_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct pilot_card_log_job {
    char drive_letter;
    struct pilot_mount_args mount_args;
    pilot_mkdir_fn mkdir_path;
    pilot_write_file_fn write_file;
};

_Static_assert(offsetof(struct pilot_mount_args, mount_flags) == 8,
               "mount argument layout changed");

static atomic_flag pilot_started = ATOMIC_FLAG_INIT;
static unsigned int card_worker_state[52];
static pthread_mutex_t card_state_lock = PTHREAD_MUTEX_INITIALIZER;

struct pilot_runtime g_pilot = {
    .optional_features_enabled = 0,
    .time_feature_enabled = 0,
    .log_lock = PTHREAD_MUTEX_INITIALIZER,
};

__attribute__((visibility("default"), used))
const unsigned int pilot_a6700_v1_marker = 0x50494c31u;

static void pilot_runtime_log(const char *message)
{
    (void)mkdir("/tmp_wbshd/pilot", 0700);
    int fd = open("/tmp_wbshd/pilot/pilot-runtime.log",
                  O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;

    const char *cursor = message;
    size_t remaining = strlen(message);
    while (remaining) {
        ssize_t written = write(fd, cursor, remaining);
        if (written <= 0) break;
        cursor += written;
        remaining -= (size_t)written;
    }
    close(fd);
}

static int is_drive_letter(unsigned char value)
{
    return (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z');
}

static unsigned int drive_index(char letter)
{
    if (letter >= 'A' && letter <= 'Z')
        return (unsigned int)(letter - 'A');
    return 26u + (unsigned int)(letter - 'a');
}

static void pilot_set_feature_state(int enabled)
{
    __atomic_store_n(&g_pilot.optional_features_enabled, enabled,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_pilot.time_feature_enabled, enabled,
                     __ATOMIC_RELEASE);
}

void pilot_runtime_mount_worker(struct pilot_mount_args mount_args,
                                pilot_mkdir_fn mkdir_path,
                                pilot_write_file_fn write_file,
                                char drive_letter)
{
    g_pilot.drive_letter = drive_letter;
    g_pilot.mount_flags = mount_args.mount_flags;
    g_pilot.mkdir_path = mkdir_path;
    g_pilot.write_file = write_file;

    char directory[32];
    char marker[40];
    int directory_size = snprintf(directory, sizeof(directory), "%c:/pilot",
                                  drive_letter);
    int marker_size = snprintf(marker, sizeof(marker), "%c:/pilot/disable",
                               drive_letter);
    int log_size = snprintf(g_pilot.log_path, sizeof(g_pilot.log_path),
                            "%c:/pilot/log.txt", drive_letter);
    if (directory_size <= 0 || (size_t)directory_size >= sizeof(directory) ||
        marker_size <= 0 || (size_t)marker_size >= sizeof(marker) ||
        log_size <= 0 || (size_t)log_size >= sizeof(g_pilot.log_path)) {
        pilot_set_feature_state(0);
        pilot_runtime_log("PILOT_GATE=closed reason=path_format\n");
        return;
    }

    (void)mkdir_path(directory);

    if (!pilot_fsys_resolve(&g_pilot.fsys)) {
        pilot_set_feature_state(0);
        char receipt[112];
        snprintf(receipt, sizeof(receipt),
                 "PILOT_GATE=closed reason=fsys_api_unresolved drive=%c\n",
                 drive_letter);
        pilot_runtime_log(receipt);
    } else {
        int marker_state = pilot_fsys_check_disable(directory, marker,
                                                    &g_pilot.fsys);
        pilot_set_feature_state(marker_state == 0);
        if (marker_state == 1)
            pilot_runtime_log("PILOT_GATE=closed reason=pilot/disable\n");
        else if (marker_state < 0)
            pilot_runtime_log("PILOT_GATE=closed reason=disable_check_error\n");
        else
            pilot_runtime_log("PILOT_GATE=open reason=disable_marker_absent\n");
    }

    char line[256];
    char timestamp[32];
    int enabled = __atomic_load_n(&g_pilot.optional_features_enabled,
                                  __ATOMIC_ACQUIRE);
    if (enabled && pilot_time_utc_impl(timestamp, sizeof(timestamp)) ==
                       PILOT_STATUS_OK) {
        snprintf(line, sizeof(line),
                 "%s Pilot startup: Fsys mount succeeded on drive %c (mount_flags=%u, optional_features=enabled)\n",
                 timestamp, drive_letter, (unsigned int)mount_args.mount_flags);
    } else if (enabled) {
        snprintf(line, sizeof(line),
                 "Pilot startup: Fsys mount succeeded on drive %c (mount_flags=%u, optional_features=enabled, time=unavailable)\n",
                 drive_letter, (unsigned int)mount_args.mount_flags);
    } else {
        snprintf(line, sizeof(line),
                 "Pilot startup: Fsys mount succeeded on drive %c (mount_flags=%u, optional_features=disabled)\n",
                 drive_letter, (unsigned int)mount_args.mount_flags);
    }
    PilotStatus log_status = pilot_log_append(line, strlen(line), 0);
    pilot_runtime_log(log_status == PILOT_STATUS_OK
                          ? "PILOT_SD_LOG=written\n"
                          : "PILOT_SD_LOG=failed\n");

    if (enabled) pilot_load_modules();
}

static void *pilot_card_log_worker(void *opaque)
{
    struct pilot_card_log_job job = *(struct pilot_card_log_job *)opaque;
    free(opaque);
    pilot_runtime_mount_worker(job.mount_args, job.mkdir_path,
                               job.write_file, job.drive_letter);
    return 0;
}

__attribute__((visibility("default"), used))
void pilot_a6700_v1_on_mount(
    const struct pilot_mount_args *mount_args,
    pilot_get_drive_letter_fn get_drive_letter,
    pilot_mkdir_fn mkdir_path,
    pilot_write_file_fn write_file)
{
    if (!mount_args || !mount_args->drive_descriptor || !get_drive_letter ||
        !mkdir_path || !write_file)
        return;

    char drive_letter = get_drive_letter(mount_args->drive_descriptor);
    if (!is_drive_letter((unsigned char)drive_letter)) return;

    unsigned int index = drive_index(drive_letter);
    pthread_mutex_lock(&card_state_lock);
    if (card_worker_state[index] != 0) {
        pthread_mutex_unlock(&card_state_lock);
        return;
    }
    card_worker_state[index] = 1;
    pthread_mutex_unlock(&card_state_lock);

    struct pilot_card_log_job *job = malloc(sizeof(*job));
    if (!job) {
        pthread_mutex_lock(&card_state_lock);
        card_worker_state[index] = 0;
        pthread_mutex_unlock(&card_state_lock);
        pilot_runtime_log("Pilot mount worker allocation failed\n");
        return;
    }
    job->drive_letter = drive_letter;
    job->mount_args = *mount_args;
    job->mkdir_path = mkdir_path;
    job->write_file = write_file;

    pthread_t thread;
    int result = pthread_create(&thread, 0, pilot_card_log_worker, job);
    if (result == 0) {
        pthread_detach(thread);
        pthread_mutex_lock(&card_state_lock);
        card_worker_state[index] = 2;
        pthread_mutex_unlock(&card_state_lock);
        return;
    }

    free(job);
    pthread_mutex_lock(&card_state_lock);
    card_worker_state[index] = 0;
    pthread_mutex_unlock(&card_state_lock);
    pilot_runtime_log("Pilot mount worker creation failed\n");
}

static void *pilot_worker(void *unused)
{
    (void)unused;
    pilot_runtime_log("pilot_worker started\n");
    return 0;
}

__attribute__((constructor))
static void pilot_constructor(void)
{
    if (atomic_flag_test_and_set_explicit(&pilot_started, memory_order_acq_rel))
        return;

    pilot_runtime_log("pilot.so constructor entered\n");

    pthread_t thread;
    int result = pthread_create(&thread, 0, pilot_worker, 0);
    if (result == 0) {
        pthread_detach(thread);
        return;
    }

    pilot_runtime_log("pilot_worker create failed\n");
}
