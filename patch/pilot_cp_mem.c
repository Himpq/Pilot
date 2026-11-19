#define _GNU_SOURCE
#include "pilot_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PILOT_CP_PCIDMA_DEV "/dev/pcidma"
#define PILOT_CP_PCIDMA_TRANSFER 0x40185001u
#define PILOT_CP_PCIE_CH 0u
#define PILOT_CP_DMAC_CH 7u
#define PILOT_CP_PCIDMA_RD 0u
#define PILOT_CP_PCIDMA_WR 1u
#define PILOT_CP_IMAGE_RO_END 0x01000000ull
#define PILOT_CP_IOCTL_TIMEOUT_MS 35000u
#define PILOT_CP_POLL_STEP_MS 50u

struct pilot_pcidma_param {
    uint8_t pcie_ch;
    uint8_t dmac_ch;
    uint8_t dir;
    uint8_t pad;
    uint32_t size;
    uint64_t src;
    uint64_t dst;
};

_Static_assert(sizeof(struct pilot_pcidma_param) == 24,
               "pilot_pcidma_param layout");

static pthread_mutex_t g_cp_mem_lock = PTHREAD_MUTEX_INITIALIZER;

static void cp_dcache_clean_inval(const void *addr, size_t len)
{
    uintptr_t p = (uintptr_t)addr & ~63ull;
    uintptr_t end = (uintptr_t)addr + len;

    __asm__ volatile("dsb sy" ::: "memory");
    while (p < end) {
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
        p += 64ull;
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static void cp_sleep_ms(uint32_t ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000u;
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
}

static PilotStatus cp_alloc_bounce(uint8_t **buf_out, uint64_t *phys_out)
{
    uint8_t *buf;
    int pg;
    uint64_t entry = 0;
    uint64_t pfn;
    off_t off;

    buf = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED)
        return PILOT_STATUS_SYSTEM;

    memset(buf, 0xa5, 4096);
    (void)mlock(buf, 4096);

    pg = open("/proc/self/pagemap", O_RDONLY);
    if (pg < 0) {
        munmap(buf, 4096);
        return PILOT_STATUS_SYSTEM;
    }

    off = (off_t)(((uintptr_t)buf >> 12) * sizeof(uint64_t));
    if (pread(pg, &entry, sizeof(entry), off) != (ssize_t)sizeof(entry)) {
        close(pg);
        munmap(buf, 4096);
        return PILOT_STATUS_IO;
    }
    close(pg);

    if ((entry & (1ULL << 63)) == 0) {
        munmap(buf, 4096);
        return PILOT_STATUS_SYSTEM;
    }
    pfn = entry & ((1ULL << 55) - 1);
    if (pfn == 0) {
        munmap(buf, 4096);
        return PILOT_STATUS_SYSTEM;
    }

    *buf_out = buf;
    *phys_out = (pfn << 12) | ((uintptr_t)buf & 0xfffull);
    return PILOT_STATUS_OK;
}

static PilotStatus cp_run_ioctl(int fd,
                                uint8_t dir,
                                uint32_t size,
                                uint64_t src,
                                uint64_t dst)
{
    struct pilot_pcidma_param param;
    pid_t pid;
    int status = 0;
    uint32_t elapsed = 0;

    memset(&param, 0, sizeof(param));
    param.pcie_ch = PILOT_CP_PCIE_CH;
    param.dmac_ch = PILOT_CP_DMAC_CH;
    param.dir = dir;
    param.size = size;
    param.src = src;
    param.dst = dst;

    pid = fork();
    if (pid == 0) {
        if (ioctl(fd, PILOT_CP_PCIDMA_TRANSFER, &param) < 0)
            _exit(errno ? (errno & 0x7f) : 1);
        _exit(0);
    }
    if (pid < 0)
        return PILOT_STATUS_SYSTEM;

    while (elapsed < PILOT_CP_IOCTL_TIMEOUT_MS) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
                return PILOT_STATUS_OK;
            return PILOT_STATUS_IO;
        }
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return PILOT_STATUS_SYSTEM;
        }
        cp_sleep_ms(PILOT_CP_POLL_STEP_MS);
        elapsed += PILOT_CP_POLL_STEP_MS;
    }

    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return PILOT_STATUS_IO;
}

static PilotStatus cp_read_locked(uint64_t cp_address,
                                  void *output,
                                  size_t length)
{
    uint8_t *buf = NULL;
    uint64_t phys = 0;
    int fd;
    PilotStatus status;

    status = cp_alloc_bounce(&buf, &phys);
    if (status != PILOT_STATUS_OK)
        return status;

    fd = open(PILOT_CP_PCIDMA_DEV, O_RDWR);
    if (fd < 0) {
        munmap(buf, 4096);
        return PILOT_STATUS_NOT_FOUND;
    }

    cp_dcache_clean_inval(buf, length);
    status = cp_run_ioctl(fd, PILOT_CP_PCIDMA_RD, (uint32_t)length,
                          cp_address, phys);
    if (status == PILOT_STATUS_OK) {
        cp_dcache_clean_inval(buf, length);
        memcpy(output, buf, length);
    }

    close(fd);
    munmap(buf, 4096);
    return status;
}

static PilotStatus cp_write_locked(uint64_t cp_address,
                                   const void *input,
                                   size_t length)
{
    uint8_t *buf = NULL;
    uint64_t phys = 0;
    int fd;
    PilotStatus status;

    status = cp_alloc_bounce(&buf, &phys);
    if (status != PILOT_STATUS_OK)
        return status;

    memcpy(buf, input, length);
    cp_dcache_clean_inval(buf, length);

    fd = open(PILOT_CP_PCIDMA_DEV, O_RDWR);
    if (fd < 0) {
        munmap(buf, 4096);
        return PILOT_STATUS_NOT_FOUND;
    }

    status = cp_run_ioctl(fd, PILOT_CP_PCIDMA_WR, (uint32_t)length,
                          phys, cp_address);

    close(fd);
    munmap(buf, 4096);
    return status;
}

PilotStatus pilot_cp_mem_read_impl(uint64_t cp_address,
                                   void *output,
                                   size_t length)
{
    PilotStatus status;

    if (!__atomic_load_n(&g_pilot.optional_features_enabled, __ATOMIC_ACQUIRE))
        return PILOT_STATUS_DISABLED;
    if (!output || !length)
        return PILOT_STATUS_INVALID_ARGUMENT;
    if (length > PILOT_CP_MEM_TRANSFER_MAX)
        return PILOT_STATUS_RANGE;

    pthread_mutex_lock(&g_cp_mem_lock);
    status = cp_read_locked(cp_address, output, length);
    pthread_mutex_unlock(&g_cp_mem_lock);
    return status;
}

PilotStatus pilot_cp_mem_write_impl(uint64_t cp_address,
                                    const void *input,
                                    size_t length)
{
    PilotStatus status;

    if (!__atomic_load_n(&g_pilot.optional_features_enabled, __ATOMIC_ACQUIRE))
        return PILOT_STATUS_DISABLED;
    if (!input || !length)
        return PILOT_STATUS_INVALID_ARGUMENT;
    if (length > PILOT_CP_MEM_TRANSFER_MAX ||
        cp_address < PILOT_CP_IMAGE_RO_END)
        return PILOT_STATUS_RANGE;

    pthread_mutex_lock(&g_cp_mem_lock);
    status = cp_write_locked(cp_address, input, length);
    pthread_mutex_unlock(&g_cp_mem_lock);
    return status;
}

PilotStatus pilot_cp_mem_patch_impl(uint64_t cp_address,
                                    const void *expected,
                                    const void *replacement,
                                    size_t length)
{
    uint8_t current[PILOT_CP_MEM_TRANSFER_MAX];
    uint8_t verify[PILOT_CP_MEM_TRANSFER_MAX];
    PilotStatus status;

    if (!__atomic_load_n(&g_pilot.optional_features_enabled, __ATOMIC_ACQUIRE))
        return PILOT_STATUS_DISABLED;
    if (!replacement || !length)
        return PILOT_STATUS_INVALID_ARGUMENT;
    if (length > PILOT_CP_MEM_TRANSFER_MAX ||
        cp_address < PILOT_CP_IMAGE_RO_END)
        return PILOT_STATUS_RANGE;

    pthread_mutex_lock(&g_cp_mem_lock);
    status = cp_read_locked(cp_address, current, length);
    if (status == PILOT_STATUS_OK && expected &&
        memcmp(current, expected, length) != 0) {
        status = PILOT_STATUS_CONFLICT;
    }
    if (status == PILOT_STATUS_OK)
        status = cp_write_locked(cp_address, replacement, length);
    if (status == PILOT_STATUS_OK) {
        status = cp_read_locked(cp_address, verify, length);
        if (status == PILOT_STATUS_OK &&
            memcmp(verify, replacement, length) != 0)
            status = PILOT_STATUS_IO;
    }
    pthread_mutex_unlock(&g_cp_mem_lock);
    return status;
}
