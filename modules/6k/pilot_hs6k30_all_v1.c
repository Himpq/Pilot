/* A6700 6K30 Unified RAM Pilot Module v1
 *
 * Aggregates all three RAM-only patches into one SD-card Pilot module:
 *   1. AP (libmpr.so in current appFw process via /proc/self/maps + mprotect):
 *      - VPC Row 5 (0x27E60C) & Row 7 (0x27E63C): 3840x2160 -> 5760x3240 (0x1680 x 0x0CA8)
 *      - VQuality 0x22 (0x27EF7A), 0x29 (0x27EF81), 0x2B (0x27EF83): -> 0x5D (QT=0x0F / 300M)
 *   2. AV-CAM (physical 0x401393678..0x401393687 via /dev/mem pread64 + mmap):
 *      - Tier 3 16-byte row [0, 0x473, 0x476, 0x479] -> Tier 2 [0x486, 0x474, 0x477, 0x47A]
 *   3. CP (BSS table 0x0199EC2C + 0x90..0x9F via PilotApi->cp_mem):
 *      - Entry idx=3 (4K UHD slot @ 0x0199ECBC, 16 bytes, in writable CP .bss >= 0x01000000):
 *        +0x90 (0x0199ECBC): 0x08700F00 -> 0x0CA81680 (5760x3240)
 *        +0x94 (0x0199ECC0): 0x0000043E (unchanged, pre-checked)
 *        +0x98 (0x0199ECC4): 0x08700F00 -> 0x0CA81680 (5760x3240)
 *        +0x9C (0x0199ECC8): 0x000002D9 (unchanged, pre-checked)
 *
 * SD card configuration:
 *   - Optional A:/pilot/modules/pilot_hs6k30_all_v1/mode.cfg:
 *       "apply" (default if missing), "status", "undo" / "restore"
 *   - Output written to A:/pilot/modules/pilot_hs6k30_all_v1/result.txt and module.log
 */

#define _GNU_SOURCE
#include "../../patch/pilot.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t i32;
typedef int64_t i64;

#define MEM_PATH "/dev/mem"
#define PAGE_SIZE  0x1000ULL

/* --- Layer 1: AP libmpr.so --- */
#define TBL_VA         0x27E590ULL
#define GOT_VIDEO_COMB 0x2A8B78ULL
#define QUAL_TBL_VA    0x27EF58ULL

static const u8 stock_table[288] = {
    0,0,0,0,0,15,112,8,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,128,7,56,4,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,15,112,8,1,0,0,0,1,0,0,0,1,0,0,0,0,0,0,0,
    0,0,0,0,128,7,56,4,1,0,0,0,1,0,0,0,1,0,0,0,0,0,0,0,
    1,0,0,0,0,30,224,16,1,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,
    1,0,0,0,0,15,112,8,1,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,
    1,0,0,0,128,7,56,4,1,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,
    1,0,0,0,0,15,112,8,1,0,0,0,1,0,0,0,1,0,0,0,0,0,0,0,
    1,0,0,0,128,7,56,4,1,0,0,0,1,0,0,0,1,0,0,0,0,0,0,0,
    0,0,0,0,0,15,112,8,1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,
    0,0,0,0,128,7,56,4,1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,
    0,0,0,0,0,5,208,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};
static const u8 stock_quality[64] = {
    18,17,16,19,20,21,22,23,24,32,51,50,49,48,52,83,
    93,89,54,87,84,190,81,191,192,56,188,189,58,62,63,53,
    193,194,74,75,196,79,85,86,88,55,195,91,73,92,96,76,
    77,78,80,82,0,0,0,0,2,3,0,1,0,4,0,0
};
#ifndef PILOT_HS6K30_KEEP_VQ
#define PILOT_HS6K30_KEEP_VQ 0
#endif
#ifndef PILOT_HS6K30_SKIP_CP
#define PILOT_HS6K30_SKIP_CP 0
#endif
#ifndef PILOT_HS6K30_AVCAM_ONLY
#define PILOT_HS6K30_AVCAM_ONLY 0
#endif
#ifndef PILOT_HS6K30_3K1728
#define PILOT_HS6K30_3K1728 0
#endif
#ifndef PILOT_HS6K30_4K2304
#define PILOT_HS6K30_4K2304 0
#endif
#ifndef PILOT_HS6K30_5K2880
#define PILOT_HS6K30_5K2880 0
#endif
#ifndef PILOT_HS6K30_4K2592
#define PILOT_HS6K30_4K2592 0
#endif
#ifndef PILOT_HS6K30_CUSTOM_W
#define PILOT_HS6K30_CUSTOM_W 0
#endif
#ifndef PILOT_HS6K30_CUSTOM_H
#define PILOT_HS6K30_CUSTOM_H 0
#endif
#ifndef PILOT_HS6K30_3K_OPENGATE
#define PILOT_HS6K30_3K_OPENGATE 0
#endif
#ifndef PILOT_HS6K30_CP_ONLY_90
#define PILOT_HS6K30_CP_ONLY_90 0
#endif
#if PILOT_HS6K30_3K_OPENGATE
#undef PILOT_HS6K30_CUSTOM_W
#undef PILOT_HS6K30_CUSTOM_H
#define PILOT_HS6K30_CUSTOM_W 3072
#define PILOT_HS6K30_CUSTOM_H 2048
#endif
#define PILOT_HS6K30_HAS_CUSTOM ((PILOT_HS6K30_CUSTOM_W) > 0 && (PILOT_HS6K30_CUSTOM_H) > 0)
#define PILOT_HS6K30_CLONE_8K (PILOT_HS6K30_HAS_CUSTOM || PILOT_HS6K30_3K1728 || PILOT_HS6K30_4K2304 || PILOT_HS6K30_4K2592 || PILOT_HS6K30_5K2880)
#define PILOT_HS6K30_PATCH_TIER3 ((PILOT_HS6K30_HAS_CUSTOM && (((PILOT_HS6K30_CUSTOM_W) * (PILOT_HS6K30_CUSTOM_H)) > 0x870000)) || PILOT_HS6K30_4K2304 || PILOT_HS6K30_4K2592 || PILOT_HS6K30_5K2880)
static const u64 ap_offsets[5] = {0x27e60cULL, 0x27e63cULL, 0x27ef7aULL, 0x27ef81ULL, 0x27ef83ULL};
static const u8 ap_before[11]  = {0,15,112,8, 0,15,112,8, 74,55,91};
#if PILOT_HS6K30_HAS_CUSTOM
static const u8 ap_after[11]   = {
    (u8)(PILOT_HS6K30_CUSTOM_W & 0xff), (u8)((PILOT_HS6K30_CUSTOM_W >> 8) & 0xff),
    (u8)(PILOT_HS6K30_CUSTOM_H & 0xff), (u8)((PILOT_HS6K30_CUSTOM_H >> 8) & 0xff),
    (u8)(PILOT_HS6K30_CUSTOM_W & 0xff), (u8)((PILOT_HS6K30_CUSTOM_W >> 8) & 0xff),
    (u8)(PILOT_HS6K30_CUSTOM_H & 0xff), (u8)((PILOT_HS6K30_CUSTOM_H >> 8) & 0xff),
    74,55,91
};
#elif PILOT_HS6K30_4K2592
/* 4608x2592 = (0x1200, 0x0A20) LE = 00 12 20 0A; VQ kept stock */
static const u8 ap_after[11]   = {0,18,32,10, 0,18,32,10, 74,55,91};
#elif PILOT_HS6K30_5K2880
/* 5120x2880 = (0x1400, 0x0B40) LE = 00 14 40 0B; VQ kept stock */
static const u8 ap_after[11]   = {0,20,64,11, 0,20,64,11, 74,55,91};
#elif PILOT_HS6K30_4K2304
/* 4096x2304 = (0x1000, 0x0900) LE = 00 10 00 09; VQ kept stock */
static const u8 ap_after[11]   = {0,16,0,9, 0,16,0,9, 74,55,91};
#elif PILOT_HS6K30_3K1728
/* 3072x1728 = (0x0C00, 0x06C0) LE = 00 0C C0 06; VQ kept stock */
static const u8 ap_after[11]   = {0,12,192,6, 0,12,192,6, 74,55,91};
#elif PILOT_HS6K30_KEEP_VQ
static const u8 ap_after[11]   = {128,22,168,12, 128,22,168,12, 74,55,91};
#else
static const u8 ap_after[11]   = {128,22,168,12, 128,22,168,12, 93,93,93};
#endif

/* --- Layer 2: AV-CAM Tier-3 pool row --- */
#define AVCAM_SLOT_OFF  0x393678ULL
#define AVCAM_SLOT_PHYS (0x401000000ULL + AVCAM_SLOT_OFF)
#define AVCAM_PAGE_PHYS (AVCAM_SLOT_PHYS & ~(PAGE_SIZE - 1ULL))
#define AVCAM_PAGE_OFF  (AVCAM_SLOT_PHYS - AVCAM_PAGE_PHYS)
#define AVCAM_SLOT_LEN  16U

#if !PILOT_HS6K30_CLONE_8K
static const u8 TIER3_STOCK[AVCAM_SLOT_LEN] = {
    0x00, 0x00, 0x00, 0x00,
    0x73, 0x04, 0x00, 0x00,
    0x76, 0x04, 0x00, 0x00,
    0x79, 0x04, 0x00, 0x00,
};
#endif
#ifndef PILOT_HS6K30_TIER3_POOL0
#define PILOT_HS6K30_TIER3_POOL0 0x486
#endif
#ifndef PILOT_HS6K30_TIER3_POOL1
#define PILOT_HS6K30_TIER3_POOL1 0x474
#endif
#ifndef PILOT_HS6K30_TIER3_POOL2
#define PILOT_HS6K30_TIER3_POOL2 0x477
#endif
#ifndef PILOT_HS6K30_ALL_47A
#define PILOT_HS6K30_ALL_47A 0
#endif
#ifndef PILOT_HS6K30_CD_OVERRIDE_POOL
#define PILOT_HS6K30_CD_OVERRIDE_POOL 0
#endif
#if PILOT_HS6K30_PATCH_TIER3 || !PILOT_HS6K30_CLONE_8K
static const u8 TIER2_MAP[AVCAM_SLOT_LEN] = {
#if PILOT_HS6K30_ALL_47A
    0x7a, 0x04, 0x00, 0x00,
    0x7a, 0x04, 0x00, 0x00,
    0x7a, 0x04, 0x00, 0x00,
    0x7a, 0x04, 0x00, 0x00,
#else
    (u8)(PILOT_HS6K30_TIER3_POOL0 & 0xff), (u8)((PILOT_HS6K30_TIER3_POOL0 >> 8) & 0xff), 0x00, 0x00,
    (u8)(PILOT_HS6K30_TIER3_POOL1 & 0xff), (u8)((PILOT_HS6K30_TIER3_POOL1 >> 8) & 0xff), 0x00, 0x00,
    (u8)(PILOT_HS6K30_TIER3_POOL2 & 0xff), (u8)((PILOT_HS6K30_TIER3_POOL2 >> 8) & 0xff), 0x00, 0x00,
    0x7a, 0x04, 0x00, 0x00,
#endif
};
#endif

/* --- Layer 3: CP SyncRCCalc 0x0199EC2C + 0x90 (idx=3) --- */
#define CP_TABLE_BASE 0x0199EC2CULL
#define CP_IDX3_VA    (CP_TABLE_BASE + 0x90ULL) /* 0x0199ECBC, .bss */
#define CP_SLOT_LEN   16U

#ifndef PILOT_HS6K30_CP_8K_POOL
#define PILOT_HS6K30_CP_8K_POOL 0
#endif
#ifndef PILOT_HS6K30_CP_347_POOL
#define PILOT_HS6K30_CP_347_POOL 0
#endif
#ifndef PILOT_HS6K30_CP_43F_POOL
#define PILOT_HS6K30_CP_43F_POOL 0
#endif
#ifndef PILOT_HS6K30_CP_43F_BOTH
#define PILOT_HS6K30_CP_43F_BOTH 0
#endif
#ifndef PILOT_HS6K30_CP_43F_FIRST
#define PILOT_HS6K30_CP_43F_FIRST 0
#endif
#ifndef PILOT_HS6K30_CP_347_BOTH
#define PILOT_HS6K30_CP_347_BOTH 0
#endif
#if PILOT_HS6K30_CP_8K_POOL
#define CP_POOL_94_B0 0x9E
#define CP_POOL_94_B1 0x04
#define CP_POOL_9C_B0 0x3F
#define CP_POOL_9C_B1 0x04
#elif PILOT_HS6K30_CP_43F_BOTH
/* P2-B: 双槽同指 0x43F (48 MiB, boot 唯一已绑定大池) */
#define CP_POOL_94_B0 0x3F
#define CP_POOL_94_B1 0x04
#define CP_POOL_9C_B0 0x3F
#define CP_POOL_9C_B1 0x04
#elif PILOT_HS6K30_CP_43F_FIRST
/* P2-C 归因变体: 仅第一槽 0x43F, 第二槽留 stock 0x2D9 */
#define CP_POOL_94_B0 0x3F
#define CP_POOL_94_B1 0x04
#define CP_POOL_9C_B0 0xD9
#define CP_POOL_9C_B1 0x02
#elif PILOT_HS6K30_CP_43F_POOL
#define CP_POOL_94_B0 0x3E
#define CP_POOL_94_B1 0x04
#define CP_POOL_9C_B0 0x3F
#define CP_POOL_9C_B1 0x04
#elif PILOT_HS6K30_CP_347_POOL
#if PILOT_HS6K30_CP_347_BOTH
#define CP_POOL_94_B0 0x47
#define CP_POOL_94_B1 0x03
#else
#define CP_POOL_94_B0 0x3E
#define CP_POOL_94_B1 0x04
#endif
#define CP_POOL_9C_B0 0x47
#define CP_POOL_9C_B1 0x03
#else
#define CP_POOL_94_B0 0x3E
#define CP_POOL_94_B1 0x04
#define CP_POOL_9C_B0 0xD9
#define CP_POOL_9C_B1 0x02
#endif

static const u8 CP_IDX3_STOCK[CP_SLOT_LEN] = {
    0x00, 0x0F, 0x70, 0x08, /* +0x90: 3840 x 2160 */
    0x3E, 0x04, 0x00, 0x00, /* +0x94: flag 0x43E */
    0x00, 0x0F, 0x70, 0x08, /* +0x98: 3840 x 2160 */
    0xD9, 0x02, 0x00, 0x00, /* +0x9C: flag 0x2D9 */
};
#if PILOT_HS6K30_HAS_CUSTOM
static const u8 CP_IDX3_PATCH[CP_SLOT_LEN] = {
    (u8)(PILOT_HS6K30_CUSTOM_W & 0xff), (u8)((PILOT_HS6K30_CUSTOM_W >> 8) & 0xff),
    (u8)(PILOT_HS6K30_CUSTOM_H & 0xff), (u8)((PILOT_HS6K30_CUSTOM_H >> 8) & 0xff),
    CP_POOL_94_B0, CP_POOL_94_B1, 0x00, 0x00, /* +0x94 */
#if PILOT_HS6K30_CP_ONLY_90
    0x00, 0x0F, 0x70, 0x08, /* +0x98: keep 3840 x 2160 */
#else
    (u8)(PILOT_HS6K30_CUSTOM_W & 0xff), (u8)((PILOT_HS6K30_CUSTOM_W >> 8) & 0xff),
    (u8)(PILOT_HS6K30_CUSTOM_H & 0xff), (u8)((PILOT_HS6K30_CUSTOM_H >> 8) & 0xff),
#endif
    CP_POOL_9C_B0, CP_POOL_9C_B1, 0x00, 0x00, /* +0x9C */
};
#elif PILOT_HS6K30_4K2592
static const u8 CP_IDX3_PATCH[CP_SLOT_LEN] = {
    0x00, 0x12, 0x20, 0x0A, /* +0x90: 4608 x 2592 */
    CP_POOL_94_B0, CP_POOL_94_B1, 0x00, 0x00, /* +0x94 */
    0x00, 0x12, 0x20, 0x0A, /* +0x98: 4608 x 2592 */
    CP_POOL_9C_B0, CP_POOL_9C_B1, 0x00, 0x00, /* +0x9C */
};
#elif PILOT_HS6K30_5K2880
static const u8 CP_IDX3_PATCH[CP_SLOT_LEN] = {
    0x00, 0x14, 0x40, 0x0B, /* +0x90: 5120 x 2880 */
    0x3E, 0x04, 0x00, 0x00, /* +0x94: flag 0x43E */
    0x00, 0x14, 0x40, 0x0B, /* +0x98: 5120 x 2880 */
    0xD9, 0x02, 0x00, 0x00, /* +0x9C: flag 0x2D9 */
};
#elif PILOT_HS6K30_4K2304
static const u8 CP_IDX3_PATCH[CP_SLOT_LEN] = {
    0x00, 0x10, 0x00, 0x09, /* +0x90: 4096 x 2304 */
    0x3E, 0x04, 0x00, 0x00, /* +0x94: flag 0x43E */
    0x00, 0x10, 0x00, 0x09, /* +0x98: 4096 x 2304 */
    0xD9, 0x02, 0x00, 0x00, /* +0x9C: flag 0x2D9 */
};
#elif PILOT_HS6K30_3K1728
static const u8 CP_IDX3_PATCH[CP_SLOT_LEN] = {
    0x00, 0x0C, 0xC0, 0x06, /* +0x90: 3072 x 1728 */
    0x3E, 0x04, 0x00, 0x00, /* +0x94: flag 0x43E */
    0x00, 0x0C, 0xC0, 0x06, /* +0x98: 3072 x 1728 */
    0xD9, 0x02, 0x00, 0x00, /* +0x9C: flag 0x2D9 */
};
#else
static const u8 CP_IDX3_PATCH[CP_SLOT_LEN] = {
    0x80, 0x16, 0xA8, 0x0C, /* +0x90: 5760 x 3240 */
    0x3E, 0x04, 0x00, 0x00, /* +0x94: flag 0x43E */
    0x80, 0x16, 0xA8, 0x0C, /* +0x98: 5760 x 3240 */
    0xD9, 0x02, 0x00, 0x00, /* +0x9C: flag 0x2D9 */
};
#endif

static char maps_buf[131072];
static char out[2048];
static u32 out_n;

/* --- Syscall helpers (freestanding, zero libc dependencies) --- */
__attribute__((noinline)) static i64 sc(i64 nr, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
    register i64 x0 __asm__("x0") = a;
    register i64 x1 __asm__("x1") = b;
    register i64 x2 __asm__("x2") = c;
    register i64 x3 __asm__("x3") = d;
    register i64 x4 __asm__("x4") = e;
    register i64 x5 __asm__("x5") = f;
    register i64 x8 __asm__("x8") = nr;
    __asm__ volatile("svc #0" : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                     : "memory");
    return x0;
}

static i64 sys_openat(const char *p, i64 flags, i64 mode) { return sc(56, AT_FDCWD, (i64)p, flags, mode, 0, 0); }
static i64 sys_close(i64 fd) { return sc(57, fd, 0, 0, 0, 0, 0); }
static i64 sys_read(i64 fd, void *p, i64 n) { return sc(63, fd, (i64)p, n, 0, 0, 0); }
static i64 sys_pread(i64 fd, void *p, i64 n, i64 off) { return sc(67, fd, (i64)p, n, off, 0, 0); }
static i64 sys_mmap(u64 addr, u64 len, i64 prot, i64 flags, i64 fd, u64 off) {
    return sc(222, (i64)addr, (i64)len, prot, flags, fd, (i64)off);
}
static i64 sys_munmap(u64 addr, u64 len) { return sc(215, (i64)addr, (i64)len, 0, 0, 0, 0); }
static i64 sys_mprotect(u64 page, u64 len, i64 prot) { return sc(226, (i64)page, (i64)len, prot, 0, 0, 0); }
static void sys_sleep_ms(u32 ms) {
    i64 ts[2];
    ts[0] = (i64)(ms / 1000u);
    ts[1] = (i64)(ms % 1000u) * 1000000L;
    sc(101, (i64)ts, 0, 0, 0, 0, 0);
}
static int is_err(i64 r) { return r < 0 && r > -4096; }

/* --- Formatting helpers --- */
static void pc(char c) { if (out_n < sizeof(out) - 1) out[out_n++] = c; }
static void ps(const char *s) { while (*s) pc(*s++); }
static void pd(u64 v) {
    char b[24]; u32 n = 0;
    do { b[n++] = (char)('0' + (v % 10U)); v /= 10U; } while (v && n < sizeof(b));
    while (n) pc(b[--n]);
}
static void ph(u64 v) {
    static const char h[] = "0123456789abcdef";
    char b[20]; u32 n = 0;
    ps("0x");
    do { b[n++] = h[v & 0xf]; v >>= 4; } while (v && n < sizeof(b));
    while (n) pc(b[--n]);
}
static void field_str(const char *k, const char *v) { ps(k); ps(v); pc('\n'); }
static void field_dec(const char *k, i64 v) {
    ps(k);
    if (v < 0) { pc('-'); pd((u64)(-v)); } else pd((u64)v);
    pc('\n');
}
static void field_hex(const char *k, u64 v) { ps(k); ph(v); pc('\n'); }
static void field_dims(const char *k, u32 w, u32 h) { ps(k); pd(w); pc('x'); pd(h); pc('\n'); }
static void field_bytes(const char *k, const u8 *v, u32 n) {
    static const char h[] = "0123456789abcdef";
    ps(k);
    for (u32 i = 0; i < n; ++i) { pc(h[v[i] >> 4]); pc(h[v[i] & 15]); }
    pc('\n');
}

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static int contains(const char *s, const char *needle) {
    for (; *s; s++) {
        const char *a = s, *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}
static int bytes_eq(const u8 *a, const u8 *b, u32 n) {
    for (u32 i = 0; i < n; ++i) if (a[i] != b[i]) return 0;
    return 1;
}
static void copy_bytes(u8 *dst, const u8 *src, u32 n) {
    for (u32 i = 0; i < n; ++i) dst[i] = src[i];
}
static u16 le16(const u8 *p) { return (u16)p[0] | ((u16)p[1] << 8); }

/* --- Layer 1: AP libmpr.so --- */
static int parsehex(const char *b, int *p, int end, u64 *out_v) {
    u64 val = 0; int any = 0;
    while (*p < end) {
        char c = b[*p]; int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        val = (val << 4) | (u64)d; (*p)++; any = 1;
    }
    if (!any) return 0;
    *out_v = val; return 1;
}

static int read_all(const char *path, char *buf, int cap) {
    i64 fd = sys_openat(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    int total = 0;
    while (total < cap - 1) {
        i64 n = sys_read(fd, buf + total, cap - 1 - total);
        if (n < 0) { sys_close(fd); return -1; }
        if (n == 0) break;
        total += (int)n;
    }
    sys_close(fd);
    if (total <= 0 || total == cap - 1) return -1;
    buf[total] = 0;
    return total;
}

static int next_map_line(int *pos, int total, int *start, int *end) {
    if (*pos >= total) return 0;
    *start = *pos;
    while (*pos < total && maps_buf[*pos] != '\n') (*pos)++;
    *end = *pos;
    if (*pos < total) (*pos)++;
    return 1;
}

static int map_fields(int start, int end, u64 *lo, u64 *hi, u64 *off, int *prot, int *path_pos) {
    int p = start;
    if (!parsehex(maps_buf, &p, end, lo) || p >= end || maps_buf[p++] != '-') return 0;
    if (!parsehex(maps_buf, &p, end, hi)) return 0;
    while (p < end && (maps_buf[p] == ' ' || maps_buf[p] == '\t')) p++;
    if (p + 4 > end) return 0;
    *prot = (maps_buf[p] == 'r' ? 1 : 0) | (maps_buf[p + 1] == 'w' ? 2 : 0) | (maps_buf[p + 2] == 'x' ? 4 : 0);
    p += 4;
    while (p < end && (maps_buf[p] == ' ' || maps_buf[p] == '\t')) p++;
    if (!parsehex(maps_buf, &p, end, off)) return 0;
    while (p < end && (maps_buf[p] == ' ' || maps_buf[p] == '\t')) p++;
    *path_pos = p;
    return 1;
}

static u64 libmpr_base(void) {
    int n = read_all("/proc/self/maps", maps_buf, (int)sizeof(maps_buf));
    if (n <= 0) return 0;
    int pos = 0, start, end, found = 0;
    u64 base = 0;
    while (next_map_line(&pos, n, &start, &end)) {
        u64 lo, hi, off; int prot, path_pos;
        if (!map_fields(start, end, &lo, &hi, &off, &prot, &path_pos)) continue;
        char save = maps_buf[end]; maps_buf[end] = 0;
        int is_mpr = contains(maps_buf + path_pos, "libmpr.so");
        maps_buf[end] = save;
        if (!is_mpr || off != 0 || !(prot & 1)) continue;
        if (found && base != lo) return 0;
        base = lo; found = 1;
    }
    return found ? base : 0;
}

static int range_in_libmpr(u64 addr, u64 len, int req_prot) {
    int n = read_all("/proc/self/maps", maps_buf, (int)sizeof(maps_buf));
    if (n <= 0) return 0;
    int pos = 0, start, end;
    while (next_map_line(&pos, n, &start, &end)) {
        u64 lo, hi, off; int prot, path_pos;
        if (!map_fields(start, end, &lo, &hi, &off, &prot, &path_pos)) continue;
        char save = maps_buf[end]; maps_buf[end] = 0;
        int is_mpr = contains(maps_buf + path_pos, "libmpr.so");
        maps_buf[end] = save;
        if (!is_mpr || addr < lo || addr >= hi || len > hi - addr) continue;
        if ((prot & req_prot) != req_prot) continue;
        return 1;
    }
    return 0;
}

static int page_prot(u64 page) {
    int n = read_all("/proc/self/maps", maps_buf, (int)sizeof(maps_buf));
    if (n <= 0) return -1;
    int pos = 0, start, end;
    while (next_map_line(&pos, n, &start, &end)) {
        u64 lo, hi, off; int prot, path_pos;
        if (!map_fields(start, end, &lo, &hi, &off, &prot, &path_pos)) continue;
        if (page >= lo && page + PAGE_SIZE <= hi) return prot;
    }
    return -1;
}

static int ap_signature(u64 base) {
    static const u32 sites[10] = {0xbe984,0xbe988,0xbe98c,0xbe990,0xbebf4,0xbebf8,0xbebfc,0xbec00,0xbf230,0xbf238};
    static const u32 words[10] = {0x79400b60,0x7900d260,0x79400f60,0x7900d660,0x79400b60,0x7900d260,0x79400f60,0x7900d660,0x79400b41,0x79400f40};
    if (!range_in_libmpr(base + GOT_VIDEO_COMB, 8, 1)) return 0;
    if (*(volatile u64 *)(base + GOT_VIDEO_COMB) != base + TBL_VA) return 0;
    for (int i = 0; i < 10; i++) {
        if (!range_in_libmpr(base + sites[i], 4, 1)) return 0;
        if (*(volatile u32 *)(base + sites[i]) != words[i]) return 0;
    }
    return 1;
}

static int ap_byte_index(u64 off) {
    for (int i = 0; i < 2; i++)
        if (off >= ap_offsets[i] && off < ap_offsets[i] + 4) return i * 4 + (int)(off - ap_offsets[i]);
    for (int i = 2; i < 5; i++) if (off == ap_offsets[i]) return i + 6;
    return -1;
}

static int ap_patch_state(u64 base) {
    int is_stock = 1, is_patch = 1;
    for (int region = 0; region < 2; region++) {
        u64 start = region ? QUAL_TBL_VA : TBL_VA;
        int len = region ? 64 : 288;
        const u8 *expected = region ? stock_quality : stock_table;
        for (int j = 0; j < len; j++) {
            u8 v = *(volatile u8 *)(base + start + (u64)j);
            int k = ap_byte_index(start + (u64)j);
            if (v != expected[j]) is_stock = 0;
            if (v != (k < 0 ? expected[j] : ap_after[k])) is_patch = 0;
        }
    }
    return is_stock ? 1 : (is_patch ? 2 : 0);
}

static void ap_patch_write(u64 base, int apply) {
    const u8 *p = apply ? ap_after : ap_before;
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 4; j++)
            *(volatile u8 *)(base + ap_offsets[i] + (u64)j) = p[i * 4 + j];
    for (int i = 2; i < 5; i++)
        *(volatile u8 *)(base + ap_offsets[i]) = p[i + 6];
    __asm__ volatile("dsb ish" ::: "memory");
}

struct ap_info {
    u64 base;
    int orig_prot;
    int prot_restored;
    int state_before;
    int state_after;
    const char *status;
};

static int ap_probe_or_apply(int mode_op, struct ap_info *info) {
    info->base = libmpr_base();
    info->orig_prot = -1;
    info->prot_restored = 1;
    info->state_before = 0;
    info->state_after = 0;
    if (!info->base) { info->status = "NO_LIBMPR"; return 0; }
    u64 page = (info->base + TBL_VA) & ~(PAGE_SIZE - 1ULL);
    if (!range_in_libmpr(page, PAGE_SIZE, 1)) { info->status = "PAGE_UNMAPPED"; return 0; }
    info->orig_prot = page_prot(page);
    if (info->orig_prot < 0 || !ap_signature(info->base)) { info->status = "SIGNATURE_FAILED"; return 0; }
    info->state_before = ap_patch_state(info->base);
    info->state_after = info->state_before;
    if (!info->state_before) { info->status = "FOREIGN_STATE"; return 0; }

    if (mode_op == 0) {
        info->status = (info->state_before == 1) ? "STATUS_STOCK" : "STATUS_PATCHED";
        return 1;
    }
    int desired = (mode_op == 1) ? 2 : 1;
    if (info->state_before == desired) {
        info->status = (mode_op == 1) ? "ALREADY_PATCHED" : "ALREADY_STOCK";
        return 1;
    }
    if (sys_mprotect(page, PAGE_SIZE, info->orig_prot | PROT_WRITE) != 0) {
        info->status = "MPROTECT_WRITE_FAILED";
        return 0;
    }
    ap_patch_write(info->base, mode_op == 1);
    info->state_after = ap_patch_state(info->base);
    info->prot_restored = (sys_mprotect(page, PAGE_SIZE, info->orig_prot) == 0);
    if (!info->prot_restored) { info->status = "MPROTECT_RESTORE_FAILED"; return 0; }
    if (info->state_after != desired) { info->status = "VERIFY_FAILED"; return 0; }
    info->status = (mode_op == 1) ? "APPLY_OK" : "UNDO_OK";
    return 1;
}

/* --- Layer 2: AV-CAM Tier 3 pool row --- */
struct avcam_info {
    u8 before_pread[AVCAM_SLOT_LEN];
    u8 before_map[AVCAM_SLOT_LEN];
    u8 after_map[AVCAM_SLOT_LEN];
    u8 after_pread[AVCAM_SLOT_LEN];
    int have_before;
    int have_after;
    int state_before;
    int state_after;
    const char *status;
};

#if PILOT_HS6K30_CLONE_8K
#define AVCAM_BASE_PHYS 0x401000000ULL
#define CC_ROW_LEN      0x90U
#define ES_ROW_LEN      0x100U
#if PILOT_HS6K30_HAS_CUSTOM
#define TARGET_3K_W      ((u32)PILOT_HS6K30_CUSTOM_W)
#define TARGET_3K_H      ((u32)PILOT_HS6K30_CUSTOM_H)
#define TARGET_3K_STRIDE (((u32)PILOT_HS6K30_CUSTOM_W * 3U) / 2U)
#elif PILOT_HS6K30_4K2592
#define TARGET_3K_W      4608U
#define TARGET_3K_H      2592U
#define TARGET_3K_STRIDE 6912U
#elif PILOT_HS6K30_5K2880
#define TARGET_3K_W      5120U
#define TARGET_3K_H      2880U
#define TARGET_3K_STRIDE 7680U
#elif PILOT_HS6K30_4K2304
#define TARGET_3K_W      4096U
#define TARGET_3K_H      2304U
#define TARGET_3K_STRIDE 6144U
#else
#define TARGET_3K_W      3072U
#define TARGET_3K_H      1728U
#define TARGET_3K_STRIDE 4608U
#endif

#ifndef PILOT_HS6K30_USE_2CORE
#define PILOT_HS6K30_USE_2CORE (((TARGET_3K_W) * (TARGET_3K_H)) > 0x870000U)
#endif
#ifndef PILOT_HS6K30_USE_DDR_CD
#define PILOT_HS6K30_USE_DDR_CD 0
#endif
#define TARGET_3K_VSIZE_CORE0 \
    (PILOT_HS6K30_USE_2CORE ? (u16)(((((TARGET_3K_H + 15U) & ~15U) >> 1) + 31U) & ~31U) : 0U)
#define TARGET_3K_CC_38   (PILOT_HS6K30_USE_2CORE ? 2U : 0U)
#define TARGET_3K_CC_3C   (PILOT_HS6K30_USE_2CORE ? (PILOT_HS6K30_USE_DDR_CD ? 8U : 6U) : 1U)
#define TARGET_3K_CC_78   (PILOT_HS6K30_USE_2CORE ? 2U : 0U)
#define TARGET_3K_CC_7A   (PILOT_HS6K30_USE_DDR_CD ? 0U : 1U)
#define TARGET_3K_CC_85   (PILOT_HS6K30_USE_DDR_CD ? 1U : 3U)
#define TARGET_3K_CC_86   (PILOT_HS6K30_USE_DDR_CD ? 4U : 0U)
#define TARGET_3K_ES_97   (PILOT_HS6K30_USE_DDR_CD ? 1U : 3U)
#define TARGET_3K_ES_9E   (PILOT_HS6K30_USE_2CORE ? 0xB7U : 0x96U)

struct avcam_3k_pair {
    u32 src_cc_off, dst_cc_off;
    u32 src_es_off, dst_es_off;
    u32 dst_id;
};

static const struct avcam_3k_pair AVCAM_3K_PAIRS[12] = {
    /* qt=0x1A ch=0 bd=1 (100M 4:2:0 10-bit): 29.97p / 25p / 23.98p */
    {0x3A1878U, 0x3A6C48U, 0x3C4C78U, 0x3CE178U, 100100U},
    {0x3A1BD8U, 0x3A6CD8U, 0x3C5278U, 0x3CE278U, 100101U},
    {0x3A1C68U, 0x3A6D68U, 0x3C5378U, 0x3CE378U, 100200U},
    /* qt=0x22 ch=0 bd=1 (50M 4:2:0 10-bit): 29.97p / 25p / 23.98p */
    {0x3A5AD8U, 0x3A5028U, 0x3CC278U, 0x3CAF78U, 100304U},
    {0x3A5B68U, 0x3A50B8U, 0x3CC378U, 0x3CB078U, 100305U},
    {0x3A5BF8U, 0x3A51D8U, 0x3CC478U, 0x3CB278U, 100404U},
    /* qt=0x24 ch=1 bd=1 (100M 4:2:2 10-bit): 29.97p / 25p / 23.98p */
    {0x3A4458U, 0x3A3768U, 0x3C9A78U, 0x3C8378U, 100202U},
    {0x3A44E8U, 0x3A37F8U, 0x3C9B78U, 0x3C8478U, 100203U},
    {0x3A4578U, 0x3A3918U, 0x3C9C78U, 0x3C8678U, 100205U},
    /* qt=0x29 ch=0 bd=1 (30M 4:2:0 10-bit): 29.97p / 25p / 23.98p */
    {0x3A55C8U, 0x3A2E68U, 0x3CB978U, 0x3C7378U, 100306U},
    {0x3A5658U, 0x3A2EF8U, 0x3CBA78U, 0x3C7478U, 100307U},
    {0x3A56E8U, 0x3A3378U, 0x3CBB78U, 0x3C7C78U, 100405U},
};

static u16 rd_u16(const u8 *b, u32 off) { return (u16)b[off] | ((u16)b[off + 1] << 8); }
static u32 rd_u32(const u8 *b, u32 off) {
    return (u32)b[off] | ((u32)b[off + 1] << 8) | ((u32)b[off + 2] << 16) | ((u32)b[off + 3] << 24);
}
static void wr_u16(u8 *b, u32 off, u16 v) { b[off] = (u8)v; b[off + 1] = (u8)(v >> 8); }
static void wr_u32(u8 *b, u32 off, u32 v) {
    b[off] = (u8)v; b[off + 1] = (u8)(v >> 8); b[off + 2] = (u8)(v >> 16); b[off + 3] = (u8)(v >> 24);
}

static int avcam_write_phys(i64 fd, u64 phys, const u8 *buf, u32 len, u8 *rb) {
    u64 page = phys & ~(PAGE_SIZE - 1ULL);
    u64 off  = phys - page;
    u64 map_len = (off + len > PAGE_SIZE) ? (2ULL * PAGE_SIZE) : PAGE_SIZE;
    i64 map_raw = sys_mmap(0, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, page);
    if (is_err(map_raw)) return 0;
    volatile u8 *dst = (volatile u8 *)map_raw + off;
    for (u32 i = 0; i < len; ++i) dst[i] = buf[i];
    __asm__ volatile("dsb sy" ::: "memory");
    for (u32 i = 0; i < len; i += 64U) {
        __asm__ volatile("dc civac, %0" :: "r"(dst + i) : "memory");
    }
    __asm__ volatile("dc civac, %0\ndsb sy" :: "r"(dst + len - 1U) : "memory");
    sys_munmap((u64)map_raw, map_len);
    if (sys_pread(fd, rb, len, (i64)phys) != (i64)len) return 0;
    return bytes_eq(rb, buf, len);
}

static int avcam_probe_or_apply(int mode_op, struct avcam_info *info) {
    info->have_before = 0;
    info->have_after = 0;
    info->state_before = 0;
    info->state_after = 0;
    i64 fd = sys_openat(MEM_PATH, (mode_op == 0) ? O_RDONLY : O_RDWR, 0);
    if (fd < 0) { info->status = "OPEN_FAILED"; return 0; }

    u8 cc_buf[CC_ROW_LEN], es_buf[ES_ROW_LEN], rb[ES_ROW_LEN];
    int all_stock = 1, all_patch = 1;
    for (u32 i = 0; i < 12U; ++i) {
        const struct avcam_3k_pair *p = &AVCAM_3K_PAIRS[i];
        if (sys_pread(fd, cc_buf, CC_ROW_LEN, (i64)(AVCAM_BASE_PHYS + p->dst_cc_off)) != CC_ROW_LEN ||
            sys_pread(fd, es_buf, ES_ROW_LEN, (i64)(AVCAM_BASE_PHYS + p->dst_es_off)) != ES_ROW_LEN) {
            sys_close(fd);
            info->status = "PREAD_FAILED";
            return 0;
        }
        if (i == 0) {
            copy_bytes(info->before_pread, cc_buf + 0x78U, AVCAM_SLOT_LEN);
            copy_bytes(info->before_map, cc_buf + 0x78U, AVCAM_SLOT_LEN);
            info->have_before = 1;
        }
        if (rd_u32(cc_buf, 0x28U) != p->dst_id || rd_u32(es_buf, 0x20U) != p->dst_id) {
            sys_close(fd);
            info->status = "FOREIGN_STATE";
            return 0;
        }
        u16 cw = rd_u16(cc_buf, 0x7EU), ch = rd_u16(cc_buf, 0x80U);
        u16 ew = rd_u16(es_buf, 0x04U), eh = rd_u16(es_buf, 0x06U);
        u16 ew8a = rd_u16(es_buf, 0x8AU), eh8c = rd_u16(es_buf, 0x8CU);
        if (!(cw == 7680U && ch == 4320U && ew == 7680U && eh == 4320U)) all_stock = 0;
        if (!(cw == TARGET_3K_W && ch == TARGET_3K_H &&
              ew == TARGET_3K_W && eh == TARGET_3K_H &&
              ew8a == TARGET_3K_W && eh8c == TARGET_3K_H &&
              rd_u32(cc_buf, 0x38U) == TARGET_3K_CC_38 &&
              rd_u32(cc_buf, 0x3CU) == TARGET_3K_CC_3C &&
              cc_buf[0x78U] == (u8)TARGET_3K_CC_78 &&
              cc_buf[0x7AU] == (u8)TARGET_3K_CC_7A &&
              cc_buf[0x85U] == (u8)TARGET_3K_CC_85 &&
              cc_buf[0x86U] == (u8)TARGET_3K_CC_86 &&
              rd_u16(es_buf, 0x56U) == TARGET_3K_VSIZE_CORE0 &&
              es_buf[0x97U] == (u8)TARGET_3K_ES_97 &&
              es_buf[0x9EU] == (u8)TARGET_3K_ES_9E))
            all_patch = 0;
    }
    if (!all_stock && !all_patch) {
        sys_close(fd);
        info->status = "FOREIGN_STATE";
        return 0;
    }
    info->state_before = all_stock ? 1 : 2;
    info->state_after = info->state_before;
    copy_bytes(info->after_map, info->before_map, AVCAM_SLOT_LEN);
    copy_bytes(info->after_pread, info->before_pread, AVCAM_SLOT_LEN);
    info->have_after = 1;

    if (mode_op == 0) {
        sys_close(fd);
        info->status = all_stock ? "STATUS_STOCK" : "STATUS_PATCHED";
        return 1;
    }
    if (mode_op != 1) {
        sys_close(fd);
        info->status = "UNDO_UNSUPPORTED";
        return 0;
    }
    if (all_patch) {
        sys_close(fd);
        info->status = "ALREADY_PATCHED";
        return 1;
    }
    for (u32 i = 0; i < 12U; ++i) {
        const struct avcam_3k_pair *p = &AVCAM_3K_PAIRS[i];
        if (sys_pread(fd, cc_buf, CC_ROW_LEN, (i64)(AVCAM_BASE_PHYS + p->src_cc_off)) != CC_ROW_LEN ||
            sys_pread(fd, es_buf, ES_ROW_LEN, (i64)(AVCAM_BASE_PHYS + p->src_es_off)) != ES_ROW_LEN) {
            sys_close(fd);
            info->status = "SRC_PREAD_FAILED";
            return 0;
        }
        wr_u32(cc_buf, 0x28U, p->dst_id);
        wr_u32(cc_buf, 0x38U, TARGET_3K_CC_38);
        wr_u32(cc_buf, 0x3CU, TARGET_3K_CC_3C);
        cc_buf[0x78U] = (u8)TARGET_3K_CC_78;
        cc_buf[0x7AU] = (u8)TARGET_3K_CC_7A;
        cc_buf[0x85U] = (u8)TARGET_3K_CC_85;
        cc_buf[0x86U] = (u8)TARGET_3K_CC_86;
        wr_u16(cc_buf, 0x7EU, (u16)TARGET_3K_W);
        wr_u16(cc_buf, 0x80U, (u16)TARGET_3K_H);

        wr_u16(es_buf, 0x04U, (u16)TARGET_3K_W);
        wr_u16(es_buf, 0x06U, (u16)TARGET_3K_H);
        wr_u32(es_buf, 0x20U, p->dst_id);
        wr_u32(es_buf, 0x34U, TARGET_3K_W);
        wr_u32(es_buf, 0x38U, TARGET_3K_H);
        wr_u32(es_buf, 0x4CU, TARGET_3K_STRIDE);
        wr_u16(es_buf, 0x56U, TARGET_3K_VSIZE_CORE0);
        wr_u16(es_buf, 0x8AU, (u16)TARGET_3K_W);
        wr_u16(es_buf, 0x8CU, (u16)TARGET_3K_H);
        es_buf[0x97U] = (u8)TARGET_3K_ES_97;
        es_buf[0x9EU] = (u8)TARGET_3K_ES_9E;

        if (!avcam_write_phys(fd, AVCAM_BASE_PHYS + p->dst_cc_off, cc_buf, CC_ROW_LEN, rb) ||
            !avcam_write_phys(fd, AVCAM_BASE_PHYS + p->dst_es_off, es_buf, ES_ROW_LEN, rb)) {
            sys_close(fd);
            info->status = "READBACK_MISMATCH";
            return 0;
        }
        if (i == 0) {
            copy_bytes(info->after_map, cc_buf + 0x78U, AVCAM_SLOT_LEN);
            if (sys_pread(fd, rb, CC_ROW_LEN, (i64)(AVCAM_BASE_PHYS + p->dst_cc_off)) == CC_ROW_LEN)
                copy_bytes(info->after_pread, rb + 0x78U, AVCAM_SLOT_LEN);
        }
    }
#if PILOT_HS6K30_PATCH_TIER3
    if (!avcam_write_phys(fd, AVCAM_SLOT_PHYS, TIER2_MAP, AVCAM_SLOT_LEN, rb)) {
        sys_close(fd);
        info->status = "TIER3_READBACK_MISMATCH";
        return 0;
    }
#endif
#if PILOT_HS6K30_CD_OVERRIDE_POOL
    {
        static const u8 CD_OVR_STOCK[12] = {
            0x70, 0x04, 0x00, 0x00,
            0x71, 0x04, 0x00, 0x00,
            0x6F, 0x04, 0x00, 0x00,
        };
        static const u8 CD_OVR_PATCH[12] = {
            0x70, 0x04, 0x00, 0x00,
            0x71, 0x04, 0x00, 0x00,
            (u8)(PILOT_HS6K30_CD_OVERRIDE_POOL & 0xff), (u8)((PILOT_HS6K30_CD_OVERRIDE_POOL >> 8) & 0xff), 0x00, 0x00,
        };
        u64 ovr_phys = AVCAM_BASE_PHYS + 0x32C5D0ULL;
        if (sys_pread(fd, rb, 12U, (i64)ovr_phys) != 12 ||
            (!bytes_eq(rb, CD_OVR_STOCK, 12U) && !bytes_eq(rb, CD_OVR_PATCH, 12U))) {
            sys_close(fd);
            info->status = "CD_OVR_MISMATCH";
            return 0;
        }
        if (!avcam_write_phys(fd, ovr_phys, CD_OVR_PATCH, 12U, rb)) {
            sys_close(fd);
            info->status = "CD_OVR_READBACK_MISMATCH";
            return 0;
        }
    }
#endif
    sys_close(fd);
    info->state_after = 2;
    info->status = "APPLY_OK";
    return 1;
}
#else
static int avcam_classify(const u8 *row) {
    if (bytes_eq(row, TIER3_STOCK, AVCAM_SLOT_LEN)) return 1;
    if (bytes_eq(row, TIER2_MAP, AVCAM_SLOT_LEN)) return 2;
    return 0;
}

static int avcam_probe_or_apply(int mode_op, struct avcam_info *info) {
    info->have_before = 0;
    info->have_after = 0;
    info->state_before = 0;
    info->state_after = 0;
    i64 fd = sys_openat(MEM_PATH, (mode_op == 0) ? O_RDONLY : O_RDWR, 0);
    if (fd < 0) { info->status = "OPEN_FAILED"; return 0; }

    i64 pr = sys_pread(fd, info->before_pread, AVCAM_SLOT_LEN, (i64)AVCAM_SLOT_PHYS);
    i64 map_raw = sys_mmap(0, PAGE_SIZE, PROT_READ | ((mode_op == 0) ? 0 : PROT_WRITE), MAP_SHARED, fd, AVCAM_PAGE_PHYS);
    if (pr != AVCAM_SLOT_LEN || is_err(map_raw)) {
        if (!is_err(map_raw)) sys_munmap((u64)map_raw, PAGE_SIZE);
        sys_close(fd);
        info->status = is_err(map_raw) ? "MMAP_FAILED" : "PREAD_FAILED";
        return 0;
    }
    volatile u8 *slot = (volatile u8 *)map_raw + AVCAM_PAGE_OFF;
    for (u32 i = 0; i < AVCAM_SLOT_LEN; ++i) info->before_map[i] = slot[i];
    info->have_before = 1;

    if (!bytes_eq(info->before_pread, info->before_map, AVCAM_SLOT_LEN)) {
        sys_munmap((u64)map_raw, PAGE_SIZE);
        sys_close(fd);
        info->status = "READ_MISMATCH";
        return 0;
    }
    info->state_before = avcam_classify(info->before_map);
    info->state_after = info->state_before;
    copy_bytes(info->after_map, info->before_map, AVCAM_SLOT_LEN);
    copy_bytes(info->after_pread, info->before_pread, AVCAM_SLOT_LEN);
    info->have_after = 1;

    if (!info->state_before) {
        sys_munmap((u64)map_raw, PAGE_SIZE);
        sys_close(fd);
        info->status = "FOREIGN_STATE";
        return 0;
    }
    if (mode_op == 0) {
        sys_munmap((u64)map_raw, PAGE_SIZE);
        sys_close(fd);
        info->status = (info->state_before == 1) ? "STATUS_STOCK" : "STATUS_PATCHED";
        return 1;
    }
    int desired = (mode_op == 1) ? 2 : 1;
    if (info->state_before == desired) {
        sys_munmap((u64)map_raw, PAGE_SIZE);
        sys_close(fd);
        info->status = (mode_op == 1) ? "ALREADY_PATCHED" : "ALREADY_STOCK";
        return 1;
    }
    const u8 *target = (mode_op == 1) ? TIER2_MAP : TIER3_STOCK;
    for (u32 i = 0; i < AVCAM_SLOT_LEN; ++i) slot[i] = target[i];
    __asm__ volatile("dsb sy\ndc civac, %0\ndc civac, %1\ndsb sy"
                     :: "r"(slot), "r"(slot + AVCAM_SLOT_LEN - 1) : "memory");
    for (u32 i = 0; i < AVCAM_SLOT_LEN; ++i) info->after_map[i] = slot[i];
    i64 ar = sys_pread(fd, info->after_pread, AVCAM_SLOT_LEN, (i64)AVCAM_SLOT_PHYS);
    sys_munmap((u64)map_raw, PAGE_SIZE);
    sys_close(fd);
    if (ar != AVCAM_SLOT_LEN || !bytes_eq(info->after_map, target, AVCAM_SLOT_LEN) || !bytes_eq(info->after_pread, target, AVCAM_SLOT_LEN)) {
        info->status = "READBACK_MISMATCH";
        return 0;
    }
    info->state_after = desired;
    info->status = (mode_op == 1) ? "APPLY_OK" : "UNDO_OK";
    return 1;
}
#endif

/* --- Layer 3: CP SyncRCCalc 0x0199EC2C + 0x90 via PilotApi->cp_mem --- */
struct cp_info {
    PilotStatus rd_ret;
    PilotStatus patch_ret;
    PilotStatus rb_ret;
    u8 before[CP_SLOT_LEN];
    u8 after[CP_SLOT_LEN];
    int have_before;
    int have_after;
    int state_before;
    int state_after;
    const char *status;
};

static int cp_classify(const u8 *row) {
    if (bytes_eq(row, CP_IDX3_STOCK, CP_SLOT_LEN)) return 1;
    if (bytes_eq(row, CP_IDX3_PATCH, CP_SLOT_LEN)) return 2;
    return 0;
}

#define CP_PCIDMA_DEV      "/dev/pcidma"
#define CP_PCIDMA_TRANSFER 0x40185001u
#define CP_PCIDMA_RD       0u
#define CP_PCIDMA_WR       1u
#define CP_RO_END          0x01000000ULL

struct cp_pcidma_param {
    u8 pcie_ch;
    u8 dmac_ch;
    u8 dir;
    u8 pad;
    u32 size;
    u64 src;
    u64 dst;
};

struct cp_dma_shm {
    volatile u32 done;
    volatile i32 rc;
};

static void cp_dcache_clean_inval(const void *addr, u32 len) {
    u64 p = (u64)addr & ~63ULL;
    u64 end = (u64)addr + (u64)len;
    __asm__ volatile("dsb sy" ::: "memory");
    while (p < end) {
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
        p += 64ULL;
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static i32 cp_dma_xfer(u8 dir, u64 cp_addr, u8 *host_buf, u32 len) {
    if (len == 0 || len > 64u) return -22;
    if (dir == CP_PCIDMA_WR && cp_addr < CP_RO_END) return -34;

    u8 *page = (u8 *)mmap(0, 4096, PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return -12;
    for (u32 i = 0; i < 4096; i++) page[i] = 0;
    (void)mlock(page, 4096);

    int pg = open("/proc/self/pagemap", O_RDONLY);
    if (pg < 0) {
        munmap(page, 4096);
        return -5;
    }
    u64 entry = 0;
    off_t off = (off_t)(((u64)page >> 12) * sizeof(u64));
    ssize_t pr = pread(pg, &entry, sizeof(entry), off);
    close(pg);
    if (pr != (ssize_t)sizeof(entry) || !(entry & (1ULL << 63)) || !(entry & ((1ULL << 55) - 1))) {
        munmap(page, 4096);
        return -14;
    }
    u64 phys = ((entry & ((1ULL << 55) - 1)) << 12) | ((u64)page & 0xfffULL);

    if (dir == CP_PCIDMA_WR)
        copy_bytes(page, host_buf, len);
    cp_dcache_clean_inval(page, 64);

    struct cp_dma_shm *shm = (struct cp_dma_shm *)(page + 2048);
    shm->done = 0;
    shm->rc = -1;

    struct cp_pcidma_param param;
    param.pcie_ch = 0;
    param.dmac_ch = 7;
    param.dir = dir;
    param.pad = 0;
    param.size = len;
    param.src = (dir == CP_PCIDMA_RD) ? cp_addr : phys;
    param.dst = (dir == CP_PCIDMA_RD) ? phys : cp_addr;

    pid_t pid = fork();
    if (pid == 0) {
        int fd = open(CP_PCIDMA_DEV, O_RDWR);
        if (fd < 0) {
            shm->rc = -(i32)(errno ? errno : 2);
            shm->done = 1;
            _exit(1);
        }
        int ir = ioctl(fd, CP_PCIDMA_TRANSFER, &param);
        int e = errno;
        close(fd);
        shm->rc = (ir < 0) ? -(i32)(e ? e : 5) : 0;
        shm->done = 1;
        _exit(0);
    }
    if (pid < 0) {
        munmap(page, 4096);
        return -11;
    }

    int status = 0;
    for (u32 ms = 0; ms < 5000; ms += 50) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) break;
        if (shm->done) break;
        sys_sleep_ms(50);
    }
    if (!shm->done) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        munmap(page, 4096);
        return -110;
    }
    i32 rc = shm->rc;
    if (rc == 0 && dir == CP_PCIDMA_RD) {
        cp_dcache_clean_inval(page, 64);
        copy_bytes(host_buf, page, len);
    }
    munmap(page, 4096);
    return rc;
}

static int cp_probe_or_apply(const PilotApi *api, int mode_op, struct cp_info *info) {
    (void)api;
    info->rd_ret = 0;
    info->patch_ret = 0;
    info->rb_ret = 0;
    info->have_before = 0;
    info->have_after = 0;
    info->state_before = 0;
    info->state_after = 0;

    info->rd_ret = cp_dma_xfer(CP_PCIDMA_RD, CP_IDX3_VA, info->before, CP_SLOT_LEN);
    if (info->rd_ret != 0) {
        info->status = "CP_READ_FAILED";
        return 0;
    }
    copy_bytes(info->after, info->before, CP_SLOT_LEN);
    info->have_before = 1;
    info->have_after = 1;
    info->state_before = cp_classify(info->before);
    info->state_after = info->state_before;

    if (!info->state_before) {
        info->status = "FOREIGN_STATE";
        return 0;
    }
    if (mode_op == 0) {
        info->status = (info->state_before == 1) ? "STATUS_STOCK" : "STATUS_PATCHED";
        return 1;
    }
    int desired = (mode_op == 1) ? 2 : 1;
    if (info->state_before == desired) {
        info->status = (mode_op == 1) ? "ALREADY_PATCHED" : "ALREADY_STOCK";
        return 1;
    }

    const u8 *target = (mode_op == 1) ? CP_IDX3_PATCH : CP_IDX3_STOCK;
    u8 wr_buf[CP_SLOT_LEN];
    copy_bytes(wr_buf, target, CP_SLOT_LEN);
    info->patch_ret = cp_dma_xfer(CP_PCIDMA_WR, CP_IDX3_VA, wr_buf, CP_SLOT_LEN);
    if (info->patch_ret != 0) {
        info->status = "CP_PATCH_FAILED";
        return 0;
    }

    info->rb_ret = cp_dma_xfer(CP_PCIDMA_RD, CP_IDX3_VA, info->after, CP_SLOT_LEN);
    if (info->rb_ret != 0 || !bytes_eq(info->after, target, CP_SLOT_LEN)) {
        info->status = "READBACK_MISMATCH";
        return 0;
    }
#if PILOT_HS6K30_3K_OPENGATE
    {
        static const u64 CP_ROOT_PTR_VA = 0x017FAB08ULL;
        static const u64 CP_VT_BASE_VA  = 0x01991800ULL;
        static const u64 CP_TRAMP_VA    = 0x01991A00ULL;
        u8 tmp[256], rb_vt[256];
        u32 obj_va = 0, orig_vt = 0;
        if (cp_dma_xfer(CP_PCIDMA_RD, CP_ROOT_PTR_VA, (u8 *)&obj_va, 4) != 0 || obj_va == 0) {
            info->status = "CP_ROOT_NOT_READY";
            return 0;
        }
        if (cp_dma_xfer(CP_PCIDMA_RD, (u64)obj_va, (u8 *)&orig_vt, 4) != 0 || orig_vt == 0) {
            info->status = "CP_VT_READ_FAILED";
            return 0;
        }
        if ((u64)orig_vt != CP_VT_BASE_VA) {
            for (u32 o = 0; o < 256U; o += 16U) {
                if (cp_dma_xfer(CP_PCIDMA_RD, (u64)orig_vt + o, tmp + o, 16U) != 0) {
                    info->status = "CP_VT_COPY_FAILED";
                    return 0;
                }
            }
            static const u32 slot_offs[4] = {0x28U, 0x2CU, 0xD4U, 0xDCU};
            static const u8 regs[4] = {2, 3, 1, 1};
            u8 tr[128];
            for (u32 k = 0; k < 128U; ++k) tr[k] = 0;
            for (u32 k = 0; k < 4U; ++k) {
                u32 so = slot_offs[k];
                u32 orig_fn = (u32)tmp[so] | ((u32)tmp[so + 1] << 8) | ((u32)tmp[so + 2] << 16) | ((u32)tmp[so + 3] << 24);
                u8 *p = tr + k * 32U;
                u8 rn = regs[k];
                p[0] = (u8)(0xB0U | rn); p[1] = 0xF5U; p[2] = 0x02U; p[3] = 0x6FU; /* cmp.w rN, #0x820 */
                p[4] = 0x08U; p[5] = 0xBFU;                                         /* it eq */
                p[6] = 0x45U; p[7] = 0xF2U; p[8] = 0x02U; p[9] = rn;                /* movweq rN, #0x5002 */
                p[10] = 0x00U; p[11] = 0xBFU;                                       /* nop */
                p[12] = 0xDFU; p[13] = 0xF8U; p[14] = 0x00U; p[15] = 0xF0U;         /* ldr.w pc, [pc, #0] */
                p[16] = (u8)orig_fn; p[17] = (u8)(orig_fn >> 8); p[18] = (u8)(orig_fn >> 16); p[19] = (u8)(orig_fn >> 24);
                u32 tv = (u32)(CP_TRAMP_VA + k * 32U) | 1U;
                tmp[so] = (u8)tv; tmp[so + 1] = (u8)(tv >> 8); tmp[so + 2] = (u8)(tv >> 16); tmp[so + 3] = (u8)(tv >> 24);
            }
            for (u32 o = 0; o < 128U; o += 16U) {
                if (cp_dma_xfer(CP_PCIDMA_WR, CP_TRAMP_VA + o, tr + o, 16U) != 0) {
                    info->status = "CP_TRAMP_WRITE_FAILED";
                    return 0;
                }
            }
            for (u32 o = 0; o < 256U; o += 16U) {
                if (cp_dma_xfer(CP_PCIDMA_WR, CP_VT_BASE_VA + o, tmp + o, 16U) != 0 ||
                    cp_dma_xfer(CP_PCIDMA_RD, CP_VT_BASE_VA + o, rb_vt + o, 16U) != 0) {
                    info->status = "CP_TRAMP_WRITE_FAILED";
                    return 0;
                }
            }
            if (!bytes_eq(rb_vt, tmp, 256)) {
                info->status = "CP_TRAMP_WRITE_FAILED";
                return 0;
            }
            u32 new_vt = (u32)CP_VT_BASE_VA, rb_ptr = 0;
            if (cp_dma_xfer(CP_PCIDMA_WR, (u64)obj_va, (u8 *)&new_vt, 4) != 0 ||
                cp_dma_xfer(CP_PCIDMA_RD, (u64)obj_va, (u8 *)&rb_ptr, 4) != 0 ||
                rb_ptr != new_vt) {
                info->status = "CP_VT_PTR_FAILED";
                return 0;
            }
        }
    }
#endif
    info->state_after = desired;
    info->status = (mode_op == 1) ? "APPLY_OK" : "UNDO_OK";
    return 1;
}

#ifndef PILOT_HS6K30_MODE
#define PILOT_HS6K30_MODE "apply"
#endif
#ifndef PILOT_HS6K30_CP_ONLY
#define PILOT_HS6K30_CP_ONLY 0
#endif

static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;
static const PilotApi *g_api = 0;
static int g_started = 0;

static void *hs6k30_worker(void *unused) {
    (void)unused;
    const PilotApi *api = g_api;
    const char *mode = PILOT_HS6K30_MODE;

    int is_status = str_eq(mode, "status");
    int is_apply  = str_eq(mode, "apply");
    int is_undo   = str_eq(mode, "undo") || str_eq(mode, "restore");

    struct ap_info ap = {0};
    struct avcam_info av = {0};
    struct cp_info cp = {0};
    ap.status = "SKIPPED";
    av.status = "SKIPPED";
    cp.status = "SKIPPED";

    const char *overall = "BAD_MODE";
    int wait_ticks = 0;

    if (api->log && api->log->print)
        (void)api->log->print("hs6k30_all_v1: worker_start");

    if (is_status || is_apply || is_undo) {
        int mop = is_apply ? 1 : 2;
        int desired = is_apply ? 2 : 1;
        int ok_ap = 0, ok_av = 0, ok_cp = 0;

        if (is_status || PILOT_HS6K30_CP_ONLY) {
            for (wait_ticks = 0; wait_ticks < 15; wait_ticks++) {
                ok_ap = ap_probe_or_apply(0, &ap);
                ok_av = avcam_probe_or_apply(0, &av);
                ok_cp = cp_probe_or_apply(api, 0, &cp);
                if (ok_ap && ok_av && ok_cp)
                    break;
                sys_sleep_ms(1000);
            }
            if (!ok_ap || !ok_av || !ok_cp) {
                overall = !ok_ap ? "AP_PRECHECK_FAILED" : (!ok_av ? "AVCAM_PRECHECK_FAILED" : "CP_PRECHECK_FAILED");
            } else if (is_status) {
                if (ap.state_before == 1 && av.state_before == 1 && cp.state_before == 1)
                    overall = "STATUS_STOCK";
                else if (ap.state_before == 2 && av.state_before == 2 && cp.state_before == 2)
                    overall = "STATUS_PATCHED";
                else
                    overall = "STATUS_MIXED";
            } else {
                ap.status = "SKIPPED_CP_ONLY";
                av.status = "SKIPPED_CP_ONLY";
                ok_cp = cp_probe_or_apply(api, mop, &cp);
                overall = ok_cp ? (is_apply ? "APPLY_CP_ONLY_OK" : "UNDO_CP_ONLY_OK") : "CP_WRITE_FAILED";
            }
        } else if (PILOT_HS6K30_AVCAM_ONLY) {
            ap.status = "SKIPPED_AVCAM_ONLY";
            cp.status = "SKIPPED_AVCAM_ONLY";
            ok_ap = 1;
            ok_cp = 1;
            ok_av = avcam_probe_or_apply(mop, &av);
            overall = ok_av ? (is_apply ? "APPLY_OK" : "UNDO_OK") : "AVCAM_WRITE_FAILED";
        } else {
            /* Apply AP + AV-CAM immediately at t=6s before MPR EnterRec/EncOpen runs (~t=8s),
               then poll & apply CP (rolling back AP/AV-CAM if CP fails). */
            ok_ap = ap_probe_or_apply(mop, &ap);
            ok_av = ok_ap ? avcam_probe_or_apply(mop, &av) : 0;
            if (!ok_ap || !ok_av) {
                if (ok_ap && ap.state_before != desired) {
                    struct ap_info dummy_ap;
                    ap_probe_or_apply(is_apply ? 2 : 1, &dummy_ap);
                }
                overall = !ok_ap ? "AP_WRITE_FAILED" : "AVCAM_WRITE_FAILED";
            } else {
#if PILOT_HS6K30_SKIP_CP
                cp.status = "SKIPPED";
                ok_cp = 1;
                overall = is_apply ? "APPLY_OK" : "UNDO_OK";
#else
                for (wait_ticks = 0; wait_ticks < 15; wait_ticks++) {
                    ok_cp = cp_probe_or_apply(api, 0, &cp);
                    if (ok_cp) break;
                    sys_sleep_ms(1000);
                }
                if (ok_cp)
                    ok_cp = cp_probe_or_apply(api, mop, &cp);
                if (ok_cp) {
                    overall = is_apply ? "APPLY_OK" : "UNDO_OK";
                } else {
                    int rb_op = is_apply ? 2 : 1;
                    struct ap_info dummy_ap;
                    struct avcam_info dummy_av;
                    if (av.state_before != desired) avcam_probe_or_apply(rb_op, &dummy_av);
                    if (ap.state_before != desired) ap_probe_or_apply(rb_op, &dummy_ap);
                    overall = "CP_WRITE_FAILED";
                }
#endif
            }
        }
    }

    out_n = 0;
    ps("A6700_PILOT_HS6K30_ALL_V1\n");
    field_str("MODE=", mode);
    field_str("RESULT=", overall);
    field_dec("WAIT_TICKS=", wait_ticks);

    field_str("AP_STATUS=", ap.status);
    field_hex("AP_BASE=", ap.base);
    field_dec("AP_STATE_BEFORE=", ap.state_before);
    field_dec("AP_STATE_AFTER=", ap.state_after);
    field_dec("AP_PROT_RESTORED=", ap.prot_restored);
    if (ap.base && ap.state_before) {
        u64 r5 = ap.base + ap_offsets[0];
        u64 r7 = ap.base + ap_offsets[1];
        field_dims("AP_ROW5=", *(volatile u16 *)r5, *(volatile u16 *)(r5 + 2));
        field_dims("AP_ROW7=", *(volatile u16 *)r7, *(volatile u16 *)(r7 + 2));
        field_hex("AP_VQ22=", *(volatile u8 *)(ap.base + ap_offsets[2]));
        field_hex("AP_VQ29=", *(volatile u8 *)(ap.base + ap_offsets[3]));
        field_hex("AP_VQ2B=", *(volatile u8 *)(ap.base + ap_offsets[4]));
    }

    field_str("AVCAM_STATUS=", av.status);
    field_dec("AVCAM_STATE_BEFORE=", av.state_before);
    field_dec("AVCAM_STATE_AFTER=", av.state_after);
#if PILOT_HS6K30_CLONE_8K
    field_dec("AVCAM_USE_2CORE=", PILOT_HS6K30_USE_2CORE ? 1 : 0);
    field_hex("AVCAM_VSIZE_CORE0=", TARGET_3K_VSIZE_CORE0);
    field_dims("AVCAM_ES_8A_DIMS=", TARGET_3K_W, TARGET_3K_H);
    field_hex("AVCAM_HEVC_LEVEL=", TARGET_3K_ES_9E);
#endif
    if (av.have_before) field_bytes("AVCAM_BEFORE_HEX=", av.before_map, AVCAM_SLOT_LEN);
    if (av.have_after)  {
        field_bytes("AVCAM_AFTER_HEX=", av.after_map, AVCAM_SLOT_LEN);
        field_bytes("AVCAM_AFTER_PREAD_HEX=", av.after_pread, AVCAM_SLOT_LEN);
    }

    field_str("CP_STATUS=", cp.status);
    field_hex("CP_TARGET_VA=", CP_IDX3_VA);
#if PILOT_HS6K30_CP_8K_POOL
    field_str("CP_POOL_VARIANT=", "8k_pair");
#elif PILOT_HS6K30_CP_43F_BOTH
    field_str("CP_POOL_VARIANT=", "43f_both");
#elif PILOT_HS6K30_CP_43F_FIRST
    field_str("CP_POOL_VARIANT=", "43f_first");
#elif PILOT_HS6K30_CP_43F_POOL
    field_str("CP_POOL_VARIANT=", "43f_second");
#elif PILOT_HS6K30_CP_347_POOL && PILOT_HS6K30_CP_347_BOTH
    field_str("CP_POOL_VARIANT=", "347_both");
#elif PILOT_HS6K30_CP_347_POOL
    field_str("CP_POOL_VARIANT=", "347_second");
#else
    field_str("CP_POOL_VARIANT=", "stock");
#endif
    field_dec("CP_STATE_BEFORE=", cp.state_before);
    field_dec("CP_STATE_AFTER=", cp.state_after);
    field_dec("CP_RD_RET=", cp.rd_ret);
    field_dec("CP_PATCH_RET=", cp.patch_ret);
    field_dec("CP_RB_RET=", cp.rb_ret);
    if (cp.have_before) {
        field_bytes("CP_BEFORE_HEX=", cp.before, CP_SLOT_LEN);
        field_dims("CP_IDX3_BEFORE_90=", le16(cp.before + 0), le16(cp.before + 2));
        field_dims("CP_IDX3_BEFORE_98=", le16(cp.before + 8), le16(cp.before + 10));
    }
    if (cp.have_after) {
        field_bytes("CP_AFTER_HEX=", cp.after, CP_SLOT_LEN);
        field_dims("CP_IDX3_AFTER_90=", le16(cp.after + 0), le16(cp.after + 2));
        field_dims("CP_IDX3_AFTER_98=", le16(cp.after + 8), le16(cp.after + 10));
    }
    ps("END=1\n");

    if (api->mlog && api->mlog->print) {
        u32 start = 0;
        for (u32 i = 0; i <= out_n; i++) {
            if (i == out_n || out[i] == '\n') {
                if (i > start) {
                    char saved = out[i];
                    out[i] = 0;
                    (void)api->mlog->print(api->mlog, out + start);
                    out[i] = saved;
                }
                start = i + 1;
            }
        }
    }
    if (api->log && api->log->print) {
        out_n = 0;
        ps("hs6k30_all_v1: RESULT="); ps(overall);
        ps(" ap="); ps(ap.status);
        ps(" av="); ps(av.status);
        ps(" cp="); ps(cp.status);
        out[out_n] = 0;
        (void)api->log->print(out);
    }

    return 0;
}

__attribute__((visibility("default")))
PilotStatus pilot_module_init(const PilotApi *api) {
    if (!api || api->abi_version != PILOT_ABI_VERSION || api->struct_size < sizeof(PilotApi))
        return PILOT_STATUS_INVALID_ARGUMENT;

    pthread_mutex_lock(&g_init_lock);
    if (g_started) {
        pthread_mutex_unlock(&g_init_lock);
        return PILOT_STATUS_OK;
    }
    g_api = api;
    g_started = 1;
    pthread_mutex_unlock(&g_init_lock);

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&tid, &attr, hs6k30_worker, 0);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        if (api->log && api->log->print)
            (void)api->log->print("hs6k30_all_v1: pthread_create failed");
        return PILOT_STATUS_SYSTEM;
    }
    return PILOT_STATUS_OK;
}
