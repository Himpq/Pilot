#define _GNU_SOURCE
#include "../../patch/pilot.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define CP_PCIDMA_DEV      "/dev/pcidma"
#define CP_PCIDMA_TRANSFER 0x40185001u
#define CP_PCIDMA_RD       0u
#define CP_PCIDMA_WR       1u
#define CP_RO_END          0x01000000ULL

/* A6700 CP .bss addresses (verified in cpapp-b.bin + live a6700_cp_dmmcfg.bin) */
#define CP_SCALER_UNITS_VA 0x0199EEB0ULL /* u8: 0=by size class(2), 3=4 units (0x4b515c) */
#define CP_IDX3_SLOT2_VA   0x0199ECC8ULL /* u32: 0x02d9 -> 0x043f */
#define CP_BLK_IDX_TBL     0x02579D00ULL /* u16[block_id] -> block_idx (0xc01310) */
#define CP_BLK_HDR_ARR     0x023A3D00ULL /* 80B(0x50) per block_idx; [+0]=records_ptr (0xc012de) */
#define CP_UNIT_ROW_446    0x025B71D8ULL /* (u32 id=0x446, u32 key_ptr) -> (0x43f, key_ptr_498) */
#define CP_UNIT_ROW_498    0x025B72B0ULL /* (u32 id=0x498, u32 key_ptr -> [0x5f2, 0xffffffff]) */

#define CP_5F2_NEW_SIZE    0x02180000u   /* 33.50 MiB x 3 = 100.5 MiB <= 102 MiB (parent 0x39) */
#define CP_57F_NEW_SIZE    0x02200000u   /* 34.00 MiB x 2 = 68.0 MiB <= 71 MiB (parent 0x6fa) */

struct cp_pcidma_param {
    uint8_t pcie_ch;
    uint8_t dmac_ch;
    uint8_t dir;
    uint8_t pad;
    uint32_t size;
    uint64_t src;
    uint64_t dst;
};

struct cp_6k_shm {
    volatile uint32_t done;
    volatile int32_t rc;
    uint32_t ok_mask;
    uint32_t su_old;
    uint32_t id446;
    uint32_t kp446;
    uint32_t id498;
    uint32_t kp498;
    uint32_t rec_46;
    uint32_t rec_8a;
};

static void cp_dcache_clean_inval(const void *addr, uint32_t len) {
    uint64_t p = (uint64_t)(uintptr_t)addr & ~63ULL;
    uint64_t end = (uint64_t)(uintptr_t)addr + (uint64_t)len;
    __asm__ volatile("dsb sy" ::: "memory");
    while (p < end) {
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
        p += 64ULL;
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint16_t rd_u16(const uint8_t *b, uint32_t off) {
    return (uint16_t)b[off] | ((uint16_t)b[off + 1] << 8);
}

static uint32_t rd_u32(const uint8_t *b, uint32_t off) {
    return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) |
           ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}

static void wr_u32(uint8_t *b, uint32_t off, uint32_t v) {
    b[off] = (uint8_t)(v & 0xffu);
    b[off + 1] = (uint8_t)((v >> 8) & 0xffu);
    b[off + 2] = (uint8_t)((v >> 16) & 0xffu);
    b[off + 3] = (uint8_t)((v >> 24) & 0xffu);
}

static void make_rec(uint8_t rec[20], uint32_t align_shift, uint32_t ext_id, uint32_t sz, uint32_t fl) {
    wr_u32(rec, 0, 0u);
    wr_u32(rec, 4, align_shift);
    wr_u32(rec, 8, ext_id);
    wr_u32(rec, 12, sz);
    wr_u32(rec, 16, fl);
}

/* Single-fd DMA helper called inside the single forked child (zero extra forks!) */
static int32_t dma_op(int fd, uint8_t *page, uint64_t phys, uint8_t dir,
                      uint64_t cp_addr, uint8_t *buf, uint32_t len) {
    if (len == 0 || len > 64u) return -22;
    if (dir == CP_PCIDMA_WR && cp_addr < CP_RO_END) return -34;
    if (dir == CP_PCIDMA_WR) memcpy(page, buf, len);
    cp_dcache_clean_inval(page, 64);

    struct cp_pcidma_param param;
    param.pcie_ch = 0;
    param.dmac_ch = 7;
    param.dir = dir;
    param.pad = 0;
    param.size = len;
    param.src = (dir == CP_PCIDMA_RD) ? cp_addr : phys;
    param.dst = (dir == CP_PCIDMA_RD) ? phys : cp_addr;

    if (ioctl(fd, CP_PCIDMA_TRANSFER, &param) < 0)
        return -(int32_t)(errno ? errno : 5);
    if (dir == CP_PCIDMA_RD) {
        cp_dcache_clean_inval(page, 64);
        memcpy(buf, page, len);
    }
    return 0;
}

static uint32_t lookup_blk(int fd, uint8_t *page, uint64_t phys, uint32_t blk_id) {
    uint8_t b2[4], b4[4];
    if (dma_op(fd, page, phys, CP_PCIDMA_RD, CP_BLK_IDX_TBL + (uint64_t)blk_id * 2ULL, b2, 2) != 0)
        return 0;
    uint16_t bidx = rd_u16(b2, 0);
    if (bidx >= 0x400u) return 0;
    if (dma_op(fd, page, phys, CP_PCIDMA_RD, CP_BLK_HDR_ARR + (uint64_t)bidx * 0x50ULL, b4, 4) != 0)
        return 0;
    return rd_u32(b4, 0);
}

static void log_line(const PilotApi *api, const char *s) {
    if (api && api->mlog && api->mlog->print)
        (void)api->mlog->print(api->mlog, s);
}

static char *app_s(char *p, const char *s) {
    while (*s) *p++ = *s++;
    *p = 0;
    return p;
}

static char *app_hex(char *p, uint64_t v, int nibbles) {
    static const char H[] = "0123456789abcdef";
    for (int i = nibbles - 1; i >= 0; i--)
        *p++ = H[(v >> (i * 4)) & 0xf];
    *p = 0;
    return p;
}

static const PilotApi *g_api;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_started;

static void *worker(void *arg) {
    (void)arg;
    const PilotApi *api = g_api;
    char msg[256];
    char *p;

    /* Run right after pilot_hs6k30_6k_ddrcd_ippp finishes at t=10s */
    sleep(5);
    log_line(api, "CP_6K_OG_FAST_START");

    uint8_t *page = (uint8_t *)mmap(0, 4096, PROT_READ | PROT_WRITE,
                                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        log_line(api, "CP_6K_OG_MMAP_FAIL");
        return 0;
    }
    memset(page, 0, 4096);
    (void)mlock(page, 4096);

    int pg = open("/proc/self/pagemap", O_RDONLY);
    uint64_t entry = 0;
    ssize_t pr = -1;
    if (pg >= 0) {
        off_t off = (off_t)(((uint64_t)(uintptr_t)page >> 12) * sizeof(uint64_t));
        pr = pread(pg, &entry, sizeof(entry), off);
        close(pg);
    }
    if (pr != (ssize_t)sizeof(entry) || !(entry & (1ULL << 63)) || !(entry & ((1ULL << 55) - 1))) {
        munmap(page, 4096);
        log_line(api, "CP_6K_OG_PHYS_FAIL");
        return 0;
    }
    uint64_t phys = ((entry & ((1ULL << 55) - 1)) << 12) | ((uint64_t)(uintptr_t)page & 0xfffULL);
    struct cp_6k_shm *shm = (struct cp_6k_shm *)(page + 2048);
    shm->done = 0;
    shm->rc = -1;
    shm->ok_mask = 0;
    shm->su_old = 0xff;

    /* ONE SINGLE fork() for all 5 CP RAM patches (< 0.3s total!) */
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open(CP_PCIDMA_DEV, O_RDWR);
        if (fd < 0) {
            shm->rc = -(int32_t)(errno ? errno : 2);
            shm->done = 1;
            _exit(1);
        }

        uint32_t ok_mask = 0;

        /* 1) Scaler units: 0x0199EEB0 (u8: 0 -> 3 = 4 scaler units) */
        uint8_t su = 0xff;
        if (dma_op(fd, page, phys, CP_PCIDMA_RD, CP_SCALER_UNITS_VA, &su, 1) == 0 && (su == 0u || su == 3u)) {
            uint8_t nv = 3u, rb = 0;
            if (dma_op(fd, page, phys, CP_PCIDMA_WR, CP_SCALER_UNITS_VA, &nv, 1) == 0 &&
                dma_op(fd, page, phys, CP_PCIDMA_RD, CP_SCALER_UNITS_VA, &rb, 1) == 0 && rb == 3u)
                ok_mask |= 1u;
        }
        shm->su_old = su;

        /* 2) Unit-ID table: rename unused 0x446 -> 0x43f and point to 0x498's key_ptr (0x5f2) */
        uint8_t r446[8], r498[8];
        if (dma_op(fd, page, phys, CP_PCIDMA_RD, CP_UNIT_ROW_446, r446, 8) == 0 &&
            dma_op(fd, page, phys, CP_PCIDMA_RD, CP_UNIT_ROW_498, r498, 8) == 0) {
            uint32_t id446 = rd_u32(r446, 0), kp446 = rd_u32(r446, 4);
            uint32_t id498 = rd_u32(r498, 0), kp498 = rd_u32(r498, 4);
            shm->id446 = id446; shm->kp446 = kp446;
            shm->id498 = id498; shm->kp498 = kp498;
            if ((id446 == 0x446u || id446 == 0x43fu) && id498 == 0x498u && kp498 >= 0x025b8000u) {
                uint8_t nw[8], rb[8];
                wr_u32(nw, 0, 0x0000043fu);
                wr_u32(nw, 4, kp498);
                if (dma_op(fd, page, phys, CP_PCIDMA_WR, CP_UNIT_ROW_446, nw, 8) == 0 &&
                    dma_op(fd, page, phys, CP_PCIDMA_RD, CP_UNIT_ROW_446, rb, 8) == 0 &&
                    rd_u32(rb, 0) == 0x43fu && rd_u32(rb, 4) == kp498)
                    ok_mask |= 2u;
            }
        }

        /* 3) Live 0x0257BCA4 Block 0x46 (3 x 0x5f2): bump 29.4 MiB -> 33.5 MiB */
        uint32_t rec_46 = lookup_blk(fd, page, phys, 0x46u);
        shm->rec_46 = rec_46;
        if (rec_46 >= 0x02570000u && rec_46 < 0x02600000u) {
            uint8_t cur[60];
            if (dma_op(fd, page, phys, CP_PCIDMA_RD, (uint64_t)rec_46, cur, 60) == 0 &&
                rd_u32(cur, 8) == 0x05f2u &&
                (rd_u32(cur, 12) == 0x01d657d0u || rd_u32(cur, 12) == CP_5F2_NEW_SIZE)) {
                uint8_t nw[60], rb[20];
                for (int i = 0; i < 3; i++)
                    make_rec(nw + i * 20, 7u, 0x05f2u, CP_5F2_NEW_SIZE, 0x0000010fu);
                if (dma_op(fd, page, phys, CP_PCIDMA_WR, (uint64_t)rec_46, nw, 60) == 0 &&
                    dma_op(fd, page, phys, CP_PCIDMA_RD, (uint64_t)rec_46, rb, 20) == 0 &&
                    rd_u32(rb, 12) == CP_5F2_NEW_SIZE)
                    ok_mask |= 4u;
            }
        }

        /* 4) Live 0x0257BCA4 Block 0x8a (0x57f = unit 0x43e): 3 x 19.25 MiB -> 2 x 34.0 MiB + sentinel */
        uint32_t rec_8a = lookup_blk(fd, page, phys, 0x8au);
        shm->rec_8a = rec_8a;
        if (rec_8a >= 0x02570000u && rec_8a < 0x02600000u) {
            uint8_t cur[20];
            if (dma_op(fd, page, phys, CP_PCIDMA_RD, (uint64_t)rec_8a, cur, 20) == 0 &&
                rd_u32(cur, 8) == 0x057fu &&
                (rd_u32(cur, 12) == 0x01341200u || rd_u32(cur, 12) == CP_57F_NEW_SIZE)) {
                uint8_t nw[60], rb[20];
                make_rec(nw + 0,  7u, 0x057fu, CP_57F_NEW_SIZE, 0x0000010fu);
                make_rec(nw + 20, 7u, 0x057fu, CP_57F_NEW_SIZE, 0x0000010fu);
                make_rec(nw + 40, 4u, 0xffffffffu, 0xffffffffu, 0x0000010fu);
                if (dma_op(fd, page, phys, CP_PCIDMA_WR, (uint64_t)rec_8a, nw, 60) == 0 &&
                    dma_op(fd, page, phys, CP_PCIDMA_RD, (uint64_t)rec_8a, rb, 20) == 0 &&
                    rd_u32(rb, 12) == CP_57F_NEW_SIZE)
                    ok_mask |= 8u;
            }
        }

        /* 5) Only if 0x43f unit row succeeded (ok_mask & 2), switch idx3 Slot2: 0x2d9 -> 0x43f */
        if (ok_mask & 2u) {
            uint8_t s2[4];
            if (dma_op(fd, page, phys, CP_PCIDMA_RD, CP_IDX3_SLOT2_VA, s2, 4) == 0) {
                uint32_t cur_id = rd_u32(s2, 0);
                if (cur_id == 0x02d9u || cur_id == 0x043fu) {
                    uint8_t nw[4], rb[4];
                    wr_u32(nw, 0, 0x0000043fu);
                    if (dma_op(fd, page, phys, CP_PCIDMA_WR, CP_IDX3_SLOT2_VA, nw, 4) == 0 &&
                        dma_op(fd, page, phys, CP_PCIDMA_RD, CP_IDX3_SLOT2_VA, rb, 4) == 0 &&
                        rd_u32(rb, 0) == 0x043fu)
                        ok_mask |= 16u;
                }
            }
        }

        close(fd);
        shm->ok_mask = ok_mask;
        shm->rc = 0;
        shm->done = 1;
        _exit(0);
    }

    if (pid < 0) {
        munmap(page, 4096);
        log_line(api, "CP_6K_OG_FORK_FAIL");
        return 0;
    }

    int status = 0;
    for (uint32_t ms = 0; ms < 8000; ms += 10) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) break;
        if (shm->done) break;
        usleep(10000);
    }
    if (!shm->done) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        log_line(api, "CP_6K_OG_TIMEOUT");
    } else {
        p = app_s(msg, "CP_6K_OG_FAST_DONE rc="); p = app_hex(p, (uint32_t)shm->rc, 8);
        p = app_s(p, " mask="); p = app_hex(p, shm->ok_mask, 2);
        p = app_s(p, " su="); p = app_hex(p, shm->su_old, 2);
        p = app_s(p, " p46="); p = app_hex(p, shm->rec_46, 8);
        p = app_s(p, " p8a="); p = app_hex(p, shm->rec_8a, 8);
        log_line(api, msg);
    }
    munmap(page, 4096);
    return 0;
}

__attribute__((visibility("default")))
PilotStatus pilot_module_init(const PilotApi *api) {
    if (!api || api->abi_version != PILOT_ABI_VERSION || api->struct_size < sizeof(PilotApi))
        return PILOT_STATUS_INVALID_ARGUMENT;
    pthread_mutex_lock(&g_lock);
    if (g_started) {
        pthread_mutex_unlock(&g_lock);
        return PILOT_STATUS_OK;
    }
    g_api = api;
    g_started = 1;
    pthread_mutex_unlock(&g_lock);
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&tid, &attr, worker, 0);
    pthread_attr_destroy(&attr);
    return rc == 0 ? PILOT_STATUS_OK : PILOT_STATUS_SYSTEM;
}
