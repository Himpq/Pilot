/* A6700 Unified Video Modes Pilot Module
 *   (Stock / 2K OpenGate / 6K HS / 1080p 6K OS / 4K OpenGate / 5.76K OpenGate)
 *
 * Combines:
 *   - 2K OpenGate (1920x1280 3:2 on HD 24-60p)
 *   - 6K HS (5760x3240 on XAVC HS 24p/25p/30p/50p)
 *   - 1080p 6K OS (5952x3348 full readout on HD 24-60p)
 *   - 4K OpenGate (3840x2560 3:2 on 24p/25p) — Credits: GitHub @resaviewsgit, Instagram @resaviews
 *   - 5.76K OpenGate (5760x3840 3:2 on 24p/25p) — Credits: GitHub @resaviewsgit, Instagram @resaviews
 * into a single mutually-exclusive module:
 *   - Single Pilot Menu enum ("Video Mode")
 *   - Natural 100% mutual exclusion (never collides on CP idx3/idx10 or 3:2 VDF)
 *   - Single config.txt ("mode=0|1|2|3|4|5"), single worker thread (zero CP DMA race)
 *   - Native 0x260 Processing Dialog during hot-switch
 *   - Dynamic native Movie File Format menu text overrides via pilot_menu_ui
 *   - Automatic appFw pipeline reload: seq 0xCA (sequence_set_movie_format)
 *     + seq 0x32F (sequence_dummy_request_display_output_update), with 1.8s
 *     boot-settle wait so booting directly into Video mode refreshes cleanly.
 */

#define _GNU_SOURCE
#include "pilot_mod_util.h"
#include "pilot_ui.h"
#include "cp_2k_og_tables.h"
#include "cp_1080p_os_tables.h"
#include "al6k_tables.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MODULE_SO_NAME    "pilot_video_modes.so"
#define CONFIG_FILE_NAME  "config.txt"

enum {
    VMODE_STOCK  = 0,
    VMODE_OG2K   = 1,
    VMODE_6K     = 2,
    VMODE_1080OS = 3,
    VMODE_OG4K   = 4,
    VMODE_OG57K  = 5,
    VMODE_COUNT  = 6
};

static const char *const kModeNames[VMODE_COUNT] = {
    "Stock",
    "2K OpenGate",
    "6K 420 (24-50p)",
    "1080p (6K OS)",
    "4K OpenGate (24-25p)",
    "5.76K OpenGate (24-25p)"
};

/* --- Shared hardware bases --- */
#define AVCAM_BASE_PHYS   0x401000000ULL

/* --- 6K constants --- */
#define TARGET_6K_W       5760u
#define TARGET_6K_H       3240u
#define TARGET_6K_STRIDE  8640u
#define TARGET_6K_VCORE0  1632u
#define MPR_ROW5_OFF      0x27E60CULL /* XAVC HS 4K 4:2:0 10-bit */
#define MPR_ROW7_OFF      0x27E63CULL /* XAVC HS 4K 4:2:2 10-bit */
#define AVCAM_TIER3_OFF   0x393678ULL
#define CC_ROW_LEN        0x90u
#define ES_ROW_LEN        0x100u
#define AVCAM_6K_COUNT    17u

#define CP_IDX3_VA        0x0199ECBCULL /* 16B: 4K UHD output slot */
#define CP_IDX5_VA0       0x0199ECDCULL /* 4B:  HD output slot 0 */
#define CP_IDX5_VA1       0x0199ECE4ULL /* 4B:  HD output slot 1 */
#define CP_SCALER_UNITS   0x0199EEB0ULL /* 1B:  0 -> 3 (4 scaler units) */
#define CP_BLK_IDX_TBL    0x02579D00ULL
#define CP_BLK_HDR_ARR    0x023A3D00ULL
#define CP_UNIT_ROW_446   0x025B71D8ULL
#define CP_UNIT_ROW_498   0x025B72B0ULL
#define CP_5F2_NEW_SIZE   0x02180000u   /* 33.50 MiB x 3 = 100.5 MiB <= 102 MiB (parent 0x39) */
#define CP_57F_NEW_SIZE   0x02200000u   /* 34.00 MiB x 2 = 68.0 MiB <= 71 MiB (parent 0x6fa) */

/* --- appFw sequence reload offsets (ILCE-6700 2.00) --- */
#define APPFW_GET_SEQ_MGR_OFF       0x328B434ULL
#define APPFW_SEQ_SET_MOVIE_FMT_OFF 0x33164ACULL
#define APPFW_SEQ_DISP_UPDATE_OFF   0x33CDE20ULL
#define APPFW_SEQ_IS_RUNNING_OFF    0x32C7694ULL
#define APPFW_SEQ_ID_MOVIE_FMT      0xCAu
#define APPFW_SEQ_ID_DISP_UPDATE    0x32Fu

/* --- Sony UI Movie File Format RIDs (string_*_func_j.uxc, ILCE-6700 2.00) --- */
#define RID_XAVC_HS_4K              0x0200856Fu
#define RID_XAVC_S_HD               0x02008455u
#define RID_XAVC_SI_HD              0x0200898Au

static pthread_mutex_t g_init_lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_state_cond = PTHREAD_COND_INITIALIZER;

static const PilotApi *g_api = 0;
static int g_started      = 0;
static int g_cp_ready     = 0;
static int g_desired_mode = VMODE_STOCK;
static int g_applied_mode = VMODE_STOCK;

/* Backups for byte-exact hot restore */
static u8  g_avc_orig_lvl[374];
static int g_avc_lvl_saved = 0;

static int g_avcam6k_saved = 0;
static u8  g_avcam6k_orig_cc[AVCAM_6K_COUNT][CC_ROW_LEN];
static u8  g_avcam6k_orig_es[AVCAM_6K_COUNT][ES_ROW_LEN];

static int g_cp6k_saved = 0;
static u8  g_cp6k_orig_su = 0;
static u32 g_cp6k_orig_kp446 = 0;
static u32 g_cp6k_kp498 = 0;
static int g_timing_pal = -1;
static u32 g_cp6k_rec46_va = 0;
static u32 g_cp6k_rec8a_va = 0;
static u8  g_cp6k_orig_rec46[60];
static u8  g_cp6k_orig_rec8a[60];

struct avcam_pair {
    u32 src_cc_off, dst_cc_off;
    u32 src_es_off, dst_es_off;
    u32 dst_id;
};

static const struct avcam_pair AVCAM_6K_PAIRS[AVCAM_6K_COUNT] = {
    /* 100M 4:2:0 10-bit (qt=0x1A): 29.97p / 25p / 23.98p */
    {0x3A1878u, 0x3A6C48u, 0x3C4C78u, 0x3CE178u, 100100u},
    {0x3A1BD8u, 0x3A6CD8u, 0x3C5278u, 0x3CE278u, 100101u},
    {0x3A1C68u, 0x3A6D68u, 0x3C5378u, 0x3CE378u, 100200u},
    /* 50M 4:2:0 10-bit (qt=0x22): 29.97p / 25p / 23.98p */
    {0x3A5AD8u, 0x3A5028u, 0x3CC278u, 0x3CAF78u, 100304u},
    {0x3A5B68u, 0x3A50B8u, 0x3CC378u, 0x3CB078u, 100305u},
    {0x3A5BF8u, 0x3A51D8u, 0x3CC478u, 0x3CB278u, 100404u},
    /* 100M 4:2:2 10-bit (qt=0x24): 29.97p / 25p / 23.98p */
    {0x3A4458u, 0x3A3768u, 0x3C9A78u, 0x3C8378u, 100202u},
    {0x3A44E8u, 0x3A37F8u, 0x3C9B78u, 0x3C8478u, 100203u},
    {0x3A4578u, 0x3A3918u, 0x3C9C78u, 0x3C8678u, 100205u},
    /* 30M 4:2:0 10-bit (qt=0x29): 29.97p / 25p / 23.98p */
    {0x3A55C8u, 0x3A2E68u, 0x3CB978u, 0x3C7378u, 100306u},
    {0x3A5658u, 0x3A2EF8u, 0x3CBA78u, 0x3C7478u, 100307u},
    {0x3A56E8u, 0x3A3378u, 0x3CBB78u, 0x3C7C78u, 100405u},
    /* 50p PAL (5 pairs, hardware-verified) */
    {0x3A30A8u, 0x3A2CB8u, 0x3C7778u, 0x3C7078u, 100501u},
    {0x3A1AB8u, 0x3A2D48u, 0x3C5078u, 0x3C7178u, 100503u},
    {0x3A2A78u, 0x3A2DD8u, 0x3C6C78u, 0x3C7278u, 100601u},
    {0x3A7938u, 0x3A2F88u, 0x3CF878u, 0x3C7578u, 100403u},
    {0x3A43C8u, 0x3A3888u, 0x3C9978u, 0x3C8578u, 100204u},
};

/* --- Config persistence --- */
static int load_config_mode(const PilotApi *api) {
    if (!api || !api->file || !api->file->read)
        return VMODE_STOCK;
    char buf[128];
    memset(buf, 0, sizeof(buf));
    if (api->file->read(api->file, CONFIG_FILE_NAME, buf, sizeof(buf)) != PILOT_STATUS_OK)
        return VMODE_STOCK;
    const char *p = strstr(buf, "mode=");
    if (!p) return VMODE_STOCK;
    int m = p[5] - '0';
    return (m >= 0 && m < VMODE_COUNT) ? m : VMODE_STOCK;
}

static void save_config_mode(const PilotApi *api, int mode) {
    if (!api || !api->file || !api->file->write)
        return;
    char buf[32];
    snprintf(buf, sizeof(buf), "mode=%d\n", mode);
    (void)api->file->write(api->file, CONFIG_FILE_NAME, buf);
}

static void show_toast(const char *msg, u32 ms) {
    if (pilot_ui_show_text(msg, ms) != PILOT_STATUS_OK)
        pm_toast_show("vmod", 0, 36, 480, 32, msg, ms);
}

/* Wait for appFw boot / wake init to settle BEFORE touching live CP DMA so the
 * screen never sits in a mismatched DMA-vs-ISP debayer state while waiting. */
static void wait_appfw_settle(const PilotApi *api, u32 settle_ms) {
    u64 appfw = pm_find_so_base("appFw.so");
    if (appfw) {
        typedef void *(*get_seq_mgr_fn)(void);
        typedef int   (*is_running_fn)(void *mgr, u32 seq_id);
        get_seq_mgr_fn get_mgr = (get_seq_mgr_fn)(uintptr_t)(appfw + APPFW_GET_SEQ_MGR_OFF);
        is_running_fn  is_run  = (is_running_fn)(uintptr_t)(appfw + APPFW_SEQ_IS_RUNNING_OFF);
        void *mgr = get_mgr();
        if (mgr) {
            for (u32 w = 0; w < 60u && is_run(mgr, APPFW_SEQ_ID_MOVIE_FMT); ++w)
                usleep(50000);
        }
    }
    if (settle_ms > 0u)
        usleep((useconds_t)(settle_ms * 1000u));
    (void)api;
}

/* --- Native appFw pipeline reload (0xCA + 0x32F) --- */
static void reload_movie_pipeline(const PilotApi *api, int is_boot) {
    u64 appfw = pm_find_so_base("appFw.so");
    if (!appfw) {
        pm_emit_log(api, "VMOD_RELOAD skip=NO_APPFW");
        return;
    }

    typedef void *(*get_seq_mgr_fn)(void);
    typedef void  (*start_seq_fn)(void *mgr, u32 seq_id, void *arg);
    typedef void  (*start_disp_fn)(void *mgr, u32 seq_id, void *arg, u32 flag);
    typedef int   (*is_running_fn)(void *mgr, u32 seq_id);

    get_seq_mgr_fn get_mgr = (get_seq_mgr_fn)(uintptr_t)(appfw + APPFW_GET_SEQ_MGR_OFF);
    start_seq_fn   start_s = (start_seq_fn)(uintptr_t)(appfw + APPFW_SEQ_SET_MOVIE_FMT_OFF);
    start_disp_fn  start_d = (start_disp_fn)(uintptr_t)(appfw + APPFW_SEQ_DISP_UPDATE_OFF);
    is_running_fn  is_run  = (is_running_fn)(uintptr_t)(appfw + APPFW_SEQ_IS_RUNNING_OFF);

    void *mgr = get_mgr();
    if (!mgr) {
        pm_emit_log(api, "VMOD_RELOAD skip=NO_SEQ_MGR");
        return;
    }

    /* Fire 0xCA immediately after live CP DMA patch (0ms gap -> no debayer glitch).
     * On boot/wake, if appFw was still busy and dropped 0xCA (ca_ms < 100ms),
     * retry once after 400ms so VDF surface always rebuilds cleanly (no bottom 1/4 block). */
    long ca_ms = 0;
    int tries = 0;
    int max_tries = is_boot ? 2 : 1;
    for (tries = 1; tries <= max_tries; ++tries) {
        struct timespec c0, c1;
        clock_gettime(CLOCK_MONOTONIC, &c0);
        start_s(mgr, APPFW_SEQ_ID_MOVIE_FMT, 0);
        usleep(50000);
        for (u32 w = 0; w < 50u && is_run(mgr, APPFW_SEQ_ID_MOVIE_FMT); ++w)
            usleep(50000);
        clock_gettime(CLOCK_MONOTONIC, &c1);
        ca_ms = (c1.tv_sec - c0.tv_sec) * 1000L + (c1.tv_nsec - c0.tv_nsec) / 1000000L;
        if (ca_ms >= 100L || tries >= max_tries)
            break;
        usleep(400000);
    }

    struct timespec d0, d1;
    clock_gettime(CLOCK_MONOTONIC, &d0);
    start_d(mgr, APPFW_SEQ_ID_DISP_UPDATE, 0, 1u);
    usleep(50000);
    for (u32 w = 0; w < 30u && is_run(mgr, APPFW_SEQ_ID_DISP_UPDATE); ++w)
        usleep(50000);
    clock_gettime(CLOCK_MONOTONIC, &d1);
    long disp_ms = (d1.tv_sec - d0.tv_sec) * 1000L + (d1.tv_nsec - d0.tv_nsec) / 1000000L;

    char line[160];
    snprintf(line, sizeof(line), "VMOD_RELOAD seq=0xca+0x32f boot=%d tries=%d ca_ms=%ld disp_ms=%ld run=%d/%d",
             is_boot, tries, ca_ms, disp_ms,
             is_run(mgr, APPFW_SEQ_ID_MOVIE_FMT) ? 1 : 0,
             is_run(mgr, APPFW_SEQ_ID_DISP_UPDATE) ? 1 : 0);
    pm_emit_log(api, line);
}

/* =========================================================================
 *  PART 1: 2K OpenGate (1920x1280 3:2 on HD 24-60p)
 * ========================================================================= */
static const char *apply_og2k_ap(int enable) {
    u64 base = pm_find_so_base("libmpr.so");
    if (!base) return "LIBMPR_NOT_FOUND";
    static const u64 offs[4] = {0x27e5acULL, 0x27e5dcULL, 0x27e624ULL, 0x27e684ULL};
    static const u8 stock[4] = {0x80, 0x07, 0x38, 0x04};
    static const u8 patch[4] = {0x80, 0x07, 0x00, 0x05};
    const char *rc = pm_ap_patch_ro_multi(base, offs, 4u,
                                          enable ? stock : patch,
                                          enable ? patch : stock, 4u);
    if (strcmp(rc, "APPLY_OK") != 0 && strcmp(rc, "ALREADY_PATCHED") != 0)
        return rc;

    /* Playback CompareResolution (0x15e4f4): 1920-wide height eq -> hs (>=1080) */
    static const u8 cmp_stock[4] = {0xe0, 0x17, 0x9f, 0x1a};
    static const u8 cmp_patch[4] = {0xe0, 0x37, 0x9f, 0x1a};
    const u8 *exp = enable ? cmp_stock : cmp_patch;
    const u8 *rep = enable ? cmp_patch : cmp_stock;
    u8 *dst = (u8 *)(uintptr_t)(base + 0x15e4f4ULL);
    if (memcmp(dst, rep, 4u) != 0) {
        if (memcmp(dst, exp, 4u) != 0) return "MPR_PB_MISMATCH";
        u64 page = (base + 0x15e4f4ULL) & ~(PM_PAGE_SIZE - 1ULL);
        if (mprotect((void *)(uintptr_t)page, (size_t)PM_PAGE_SIZE,
                     PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
            return "MPR_PB_MPROTECT_FAIL";
        memcpy(dst, rep, 4u);
        __builtin___clear_cache((char *)dst, (char *)(dst + 4u));
        (void)mprotect((void *)(uintptr_t)page, (size_t)PM_PAGE_SIZE,
                       PROT_READ | PROT_EXEC);
        if (memcmp(dst, rep, 4u) != 0) return "MPR_PB_VERIFY_FAIL";
    }
    return rc;
}

static int patch_avcam_phys_idempotent(const PilotApi *api, int fd, u64 off,
                                       const u8 *exp, const u8 *rep, size_t len) {
    u64 phys = AVCAM_BASE_PHYS + off;
    u8 cur[32], rb[32];
    memset(cur, 0, sizeof(cur));
    if (len == 0u || len > sizeof(cur)) return 0;
    if (pread(fd, cur, len, (off_t)phys) != (ssize_t)len) {
        if (api) {
            char line[96];
            snprintf(line, sizeof(line), "VMOD_PB_ERR off=0x%llx stage=pread_fail", (unsigned long long)off);
            pm_emit_log(api, line);
        }
        return 0;
    }
    if (memcmp(cur, rep, len) == 0) return 1;
    if (memcmp(cur, exp, len) != 0) {
        if (api) {
            char line[128];
            snprintf(line, sizeof(line),
                     "VMOD_PB_ERR off=0x%llx stage=mismatch cur=%02x%02x%02x%02x exp=%02x%02x%02x%02x",
                     (unsigned long long)off,
                     cur[0], cur[1], cur[2], cur[3],
                     exp[0], exp[1], exp[2], exp[3]);
            pm_emit_log(api, line);
        }
        return 0;
    }
    u64 page = phys & ~0xFFFULL;
    u64 p_off = phys - page;
    size_t map_len = (p_off + len > 0x1000ULL) ? 0x2000u : 0x1000u;
    void *m = mmap(0, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)page);
    if (m == MAP_FAILED) return 0;
    u8 *dst = (u8 *)m + p_off;
    memcpy(dst, rep, len);
    pm_dcache_clean_inval(dst, len);
    munmap(m, map_len);
    if (pread(fd, rb, len, (off_t)phys) != (ssize_t)len) return 0;
    return memcmp(rb, rep, len) == 0;
}

static const char *apply_og2k_avcam(const PilotApi *api, int enable) {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) return "DEVMEM_OPEN_FAIL";

    const u64 map_off = 0x393000ULL;
    const size_t map_len = 0x52000u;
    void *m = mmap(0, map_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, (off_t)(AVCAM_BASE_PHYS + map_off));
    if (m == MAP_FAILED) { close(fd); return "MMAP_FAIL"; }
    u8 *base = (u8 *)m;

    const u16 from_h = enable ? 1080u : 1280u;
    const u16 to_h   = enable ? 1280u : 1080u;

    u32 cc_count = 0;
    for (u32 i = 0; i < 689u; ++i) {
        u8 *r = base + (0x3936E8u - (u32)map_off) + i * 0x90u;
        u16 w = pm_rd_u16(r, 0x7Eu), h = pm_rd_u16(r, 0x80u);
        if (w == 1920u && (h == 1080u || h == 1280u)) {
            if (h == from_h) pm_wr_u16(r, 0x80u, to_h);
            cc_count++;
        }
    }

    u32 avc_count = 0;
    for (u32 idx = 0; idx < 374u; ++idx) {
        u32 off = 0x3ABA78u + idx * 0x100u;
        u8 *r = base + (off - (u32)map_off);
        if (!g_avc_lvl_saved)
            g_avc_orig_lvl[idx] = r[0xA7u];
        u16 w = pm_rd_u16(r, 0x04u), h = pm_rd_u16(r, 0x06u);
        if (w == 1920u && (h == 1080u || h == 1280u)) {
            if (h == from_h) {
                pm_wr_u16(r, 0x06u, to_h);
                pm_wr_u32(r, 0x38u, (u32)to_h);
                pm_wr_u16(r, 0x90u, to_h);
                {
                    u16 from_mb_half = enable ? 0x0210u : 0x0280u;
                    u16 to_mb_half   = enable ? 0x0280u : 0x0210u;
                    if (pm_rd_u16(r, 0x56u) == from_mb_half)
                        pm_wr_u16(r, 0x56u, to_mb_half);
                }
                if (enable) {
                    if (r[0xA7u] < 52u) r[0xA7u] = 52u;
                } else if (g_avc_lvl_saved) {
                    r[0xA7u] = g_avc_orig_lvl[idx];
                }
            }
            avc_count++;
        }
    }
    g_avc_lvl_saved = 1;

    for (u32 off = 0x3C3078u; off < 0x3E4778u; off += 0x100u) {
        u8 *r = base + (off - (u32)map_off);
        u16 w = pm_rd_u16(r, 0x04u), h = pm_rd_u16(r, 0x06u);
        if (w == 1920u && (h == 1080u || h == 1280u)) {
            if (h == from_h) {
                pm_wr_u16(r, 0x06u, to_h);
                pm_wr_u32(r, 0x38u, (u32)to_h);
                pm_wr_u16(r, 0x8Cu, to_h);
            }
        }
    }

    pm_dcache_clean_inval(base, map_len);
    munmap(m, map_len);

    /* Playback fixes for 1920x1280: pbformat (0x115de8), pbaspect (0x23e510), avdec HD (0xc00378) */
    static const u8 pbf_stock[4] = {0xe1, 0x00, 0x00, 0x54};
    static const u8 pbf_patch[4] = {0xe3, 0x00, 0x00, 0x54};
    static const u8 pba_stock[28] = {
        0xbf, 0xe2, 0x10, 0x71, 0xe0, 0x02, 0x00, 0x54,
        0xbf, 0xc2, 0x21, 0x71, 0xe0, 0x03, 0x00, 0x54,
        0x08, 0x1c, 0x82, 0x52, 0xbf, 0x02, 0x08, 0x6b,
        0x01, 0x06, 0x00, 0x54
    };
    static const u8 pba_patch[28] = {
        0xbf, 0xc2, 0x21, 0x71, 0x20, 0x04, 0x00, 0x54,
        0x08, 0x1c, 0x82, 0x52, 0xbf, 0x02, 0x08, 0x6b,
        0x60, 0x00, 0x00, 0x54, 0xbf, 0xe2, 0x10, 0x71,
        0x42, 0x02, 0x00, 0x54
    };
    static const u8 dec_stock[4] = {0x80, 0x07, 0x40, 0x04};
    static const u8 dec_patch[4] = {0x80, 0x07, 0x00, 0x05};

    int pb_ok = 1;
    pb_ok &= patch_avcam_phys_idempotent(api, fd, 0x115de8ULL,
                                         enable ? pbf_stock : pbf_patch,
                                         enable ? pbf_patch : pbf_stock, 4u);
    pb_ok &= patch_avcam_phys_idempotent(api, fd, 0x23e510ULL,
                                         enable ? pba_stock : pba_patch,
                                         enable ? pba_patch : pba_stock, 28u);
    pb_ok &= patch_avcam_phys_idempotent(api, fd, 0xc00378ULL,
                                         enable ? dec_stock : dec_patch,
                                         enable ? dec_patch : dec_stock, 4u);

    close(fd);
    if (cc_count < 100u || avc_count < 100u) return "ROW_COUNT_LOW";
    if (!pb_ok) return "PB_PATCH_FAIL";
    return "APPLY_OK";
}

static const char *apply_cp_dma_table(const PilotApi *api,
                                      const struct cp_2k_patch *tbl, size_t n_dma,
                                      int enable, u32 *applied, u32 *skipped) {
    for (size_t i = 0; i < n_dma; ++i) {
        u8 cur[40], rb[40];
        u8 ln = tbl[i].len;
        const u8 *exp = enable ? tbl[i].stock : tbl[i].patch;
        const u8 *rep = enable ? tbl[i].patch : tbl[i].stock;

        if (api->cp_mem->read(tbl[i].va, cur, ln) != PILOT_STATUS_OK) {
            char err[96];
            snprintf(err, sizeof(err), "VMOD_DMA_ERR idx=%u va=0x%08x step=rd", (unsigned)i, tbl[i].va);
            pm_emit_log(api, err);
            return "CP_DMA_RD_FAIL";
        }
        if (memcmp(cur, rep, ln) == 0) { (*skipped)++; continue; }
        if (memcmp(cur, exp, ln) != 0) {
            char err[128];
            snprintf(err, sizeof(err), "VMOD_DMA_ERR idx=%u va=0x%08x step=cmp cur=%02x%02x%02x%02x",
                     (unsigned)i, tbl[i].va, cur[0], ln > 1u ? cur[1] : 0u,
                     ln > 2u ? cur[2] : 0u, ln > 3u ? cur[3] : 0u);
            pm_emit_log(api, err);
            return "CP_DMA_MISMATCH";
        }
        if (api->cp_mem->write(tbl[i].va, rep, ln) != PILOT_STATUS_OK) return "CP_DMA_WR_FAIL";
        if (api->cp_mem->read(tbl[i].va, rb, ln) != PILOT_STATUS_OK || memcmp(rb, rep, ln) != 0)
            return "CP_DMA_RB_FAIL";
        (*applied)++;
    }
    return "APPLY_OK";
}

static const char *apply_cp_uipc_table(const PilotApi *api,
                                       const struct cp_2k_patch *tbl, size_t n_uipc,
                                       int enable, u32 *applied, u32 *skipped) {
    for (size_t k = 0; k < n_uipc; ++k) {
        size_t i = enable ? k : (n_uipc - 1u - k);
        const u8 *exp = enable ? tbl[i].stock : tbl[i].patch;
        const u8 *rep = enable ? tbl[i].patch : tbl[i].stock;
        PilotStatus rc = pm_cp_patch_idempotent(api, tbl[i].va, exp, rep, tbl[i].len,
                                                applied, skipped);
        if (rc != PILOT_STATUS_OK) {
            u8 cur[4] = {0};
            (void)api->cp_mem->read(tbl[i].va, cur, tbl[i].len < 4u ? tbl[i].len : 4u);
            char err[128];
            snprintf(err, sizeof(err),
                     "VMOD_UIPC_ERR idx=%u va=0x%08x len=%u en=%d rc=%d cur=%02x%02x%02x%02x",
                     (unsigned)i, tbl[i].va, (unsigned)tbl[i].len, enable, (int)rc,
                     cur[0], cur[1], cur[2], cur[3]);
            pm_emit_log(api, err);
            return "CP_UIPC_FAIL";
        }
    }
    return "APPLY_OK";
}

static const char *apply_og2k_cp_dma(const PilotApi *api, int enable, u32 *applied, u32 *skipped) {
    return apply_cp_dma_table(api, CP_2K_DMA, sizeof(CP_2K_DMA) / sizeof(CP_2K_DMA[0]),
                              enable, applied, skipped);
}

static const char *apply_og2k_cp_uipc(const PilotApi *api, int enable, u32 *applied, u32 *skipped) {
    return apply_cp_uipc_table(api, CP_2K_UIPC, sizeof(CP_2K_UIPC) / sizeof(CP_2K_UIPC[0]),
                               enable, applied, skipped);
}

/* =========================================================================
 *  PART 3: 6K -> 1080p Oversampling (5952x3348 full readout on HD 24-60p)
 * ========================================================================= */
static int run_1080os(const PilotApi *api, int enable) {
    u32 w = 0, sk = 0;
    /* Patch cold UIPC tables FIRST, live DRVREC_idx10 DMA LAST (right before 0xCA)
     * to eliminate mid-stream debayer glitches. */
    const char *u = apply_cp_uipc_table(api, CP_1080OS_UIPC,
                                        sizeof(CP_1080OS_UIPC) / sizeof(CP_1080OS_UIPC[0]),
                                        enable, &w, &sk);
    const char *d = apply_cp_dma_table(api, CP_1080OS_DMA,
                                       sizeof(CP_1080OS_DMA) / sizeof(CP_1080OS_DMA[0]),
                                       enable, &w, &sk);
    char line[192];
    snprintf(line, sizeof(line), "VMOD_1080OS en=%d dma=%s uipc=%s w=%u sk=%u",
             enable, d, u, w, sk);
    pm_emit_log(api, line);
    return (strcmp(d, "APPLY_OK") == 0) && (strcmp(u, "APPLY_OK") == 0);
}

static const char *apply_og2k_cp_idx5(const PilotApi *api, int enable, u32 *applied, u32 *skipped) {
    static const u8 hd_stock[4] = {0x80, 0x07, 0x38, 0x04};
    static const u8 hd_patch[4] = {0x80, 0x07, 0x00, 0x05};
    static const u32 hd_vas[2]  = {(u32)CP_IDX5_VA0, (u32)CP_IDX5_VA1};
    const u8 *exp = enable ? hd_stock : hd_patch;
    const u8 *rep = enable ? hd_patch : hd_stock;
    for (size_t i = 0; i < 2u; ++i) {
        u8 cur[4];
        if (api->cp_mem->read(hd_vas[i], cur, 4u) == PILOT_STATUS_OK &&
            (memcmp(cur, hd_stock, 4u) == 0 || memcmp(cur, hd_patch, 4u) == 0)) {
            if (pm_cp_patch_idempotent(api, hd_vas[i], exp, rep, 4u, applied, skipped) != PILOT_STATUS_OK)
                return "CP_IDX5_FAIL";
        }
    }
    return "APPLY_OK";
}

static int run_og2k(const PilotApi *api, int enable) {
    u32 w = 0, sk = 0;
    const char *ap = apply_og2k_ap(enable);
    const char *av = apply_og2k_avcam(api, enable);
    const char *cp = "APPLY_OK";
    if (!api || !api->cp_mem || !api->cp_mem->read || !api->cp_mem->write || !api->cp_mem->patch) {
        cp = "CP_API_MISSING";
    } else if (enable) {
        /* Patch cold UIPC tables & idx5 FIRST, live DRVREC_idx10 DMA LAST right before 0xCA
         * so LiveView never runs with mismatched DMA readout vs ISP demosaic geometry. */
        cp = apply_og2k_cp_uipc(api, 1, &w, &sk);
        if (strcmp(cp, "APPLY_OK") == 0) cp = apply_og2k_cp_idx5(api, 1, &w, &sk);
        if (strcmp(cp, "APPLY_OK") == 0) cp = apply_og2k_cp_dma(api, 1, &w, &sk);
    } else {
        cp = apply_og2k_cp_idx5(api, 0, &w, &sk);
        if (strcmp(cp, "APPLY_OK") == 0) cp = apply_og2k_cp_uipc(api, 0, &w, &sk);
        if (strcmp(cp, "APPLY_OK") == 0) cp = apply_og2k_cp_dma(api, 0, &w, &sk);
    }
    char line[192];
    snprintf(line, sizeof(line), "VMOD_OG2K en=%d ap=%s av=%s cp=%s w=%u skip=%u",
             enable, ap, av, cp, w, sk);
    pm_emit_log(api, line);
    int ok_ap = (strcmp(ap, "APPLY_OK") == 0 || strcmp(ap, "ALREADY_PATCHED") == 0);
    return ok_ap && (strcmp(av, "APPLY_OK") == 0) && (strcmp(cp, "APPLY_OK") == 0);
}

/* =========================================================================
 *  PART 2: 6K XAVC HS 4:2:0 10-bit (5760x3240, 24p/25p/30p/50p)
 *  Note: Only patches MPR_ROW5_OFF (4:2:0 10-bit); MPR_ROW7_OFF (4:2:2 10-bit)
 *  stays stock 4K because 6K 4:2:2 (~44.5 MiB/frame) exceeds CP 0x57F parent
 *  pool (71 MiB for 2 slots).
 * ========================================================================= */
static const char *apply_6k_ap(int enable) {
    u64 base = pm_find_so_base("libmpr.so");
    if (!base) return "LIBMPR_NOT_FOUND";
    static const u64 offs[1]  = {MPR_ROW5_OFF};
    static const u8 dim_4k[4] = {0x00, 0x0F, 0x70, 0x08};
    static const u8 dim_6k[4] = {0x80, 0x16, 0xA8, 0x0C};
    return pm_ap_patch_ro_multi(base, offs, 1u,
                                enable ? dim_4k : dim_6k,
                                enable ? dim_6k : dim_4k, 4u);
}

static int avcam_write_phys(int fd, u64 phys, const u8 *buf, u32 len) {
    u64 page = phys & ~0xFFFULL;
    u64 off  = phys - page;
    size_t map_len = (off + len > 0x1000ULL) ? 0x2000u : 0x1000u;
    void *m = mmap(0, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)page);
    if (m == MAP_FAILED) return 0;
    u8 *dst = (u8 *)m + off;
    memcpy(dst, buf, len);
    pm_dcache_clean_inval(dst, len);
    munmap(m, map_len);
    u8 rb[ES_ROW_LEN];
    if (pread(fd, rb, len, (off_t)phys) != (ssize_t)len) return 0;
    return memcmp(rb, buf, len) == 0;
}

static const char *apply_6k_avcam(int enable) {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) return "DEVMEM_OPEN_FAIL";
    u8 cc_buf[CC_ROW_LEN], es_buf[ES_ROW_LEN];

    if (!g_avcam6k_saved) {
        for (u32 i = 0; i < AVCAM_6K_COUNT; ++i) {
            const struct avcam_pair *p = &AVCAM_6K_PAIRS[i];
            if (pread(fd, g_avcam6k_orig_cc[i], CC_ROW_LEN, (off_t)(AVCAM_BASE_PHYS + p->dst_cc_off)) != CC_ROW_LEN ||
                pread(fd, g_avcam6k_orig_es[i], ES_ROW_LEN, (off_t)(AVCAM_BASE_PHYS + p->dst_es_off)) != ES_ROW_LEN) {
                close(fd); return "AVCAM_SAVE_FAIL";
            }
            if (pm_rd_u32(g_avcam6k_orig_cc[i], 0x28u) != p->dst_id ||
                pm_rd_u32(g_avcam6k_orig_es[i], 0x20u) != p->dst_id) {
                close(fd); return "AVCAM_ID_MISMATCH";
            }
        }
        g_avcam6k_saved = 1;
    }

    for (u32 i = 0; i < AVCAM_6K_COUNT; ++i) {
        const struct avcam_pair *p = &AVCAM_6K_PAIRS[i];
        if (enable) {
            if (pread(fd, cc_buf, CC_ROW_LEN, (off_t)(AVCAM_BASE_PHYS + p->src_cc_off)) != CC_ROW_LEN ||
                pread(fd, es_buf, ES_ROW_LEN, (off_t)(AVCAM_BASE_PHYS + p->src_es_off)) != ES_ROW_LEN) {
                close(fd); return "AVCAM_SRC_RD_FAIL";
            }
            pm_wr_u32(cc_buf, 0x28u, p->dst_id);
            pm_wr_u32(cc_buf, 0x38u, 2u);
            pm_wr_u32(cc_buf, 0x3Cu, 8u);
            cc_buf[0x78u] = 2u; cc_buf[0x7Au] = 0u; cc_buf[0x85u] = 1u; cc_buf[0x86u] = 4u;
            pm_wr_u16(cc_buf, 0x7Eu, (u16)TARGET_6K_W);
            pm_wr_u16(cc_buf, 0x80u, (u16)TARGET_6K_H);

            pm_wr_u16(es_buf, 0x04u, (u16)TARGET_6K_W);
            pm_wr_u16(es_buf, 0x06u, (u16)TARGET_6K_H);
            pm_wr_u32(es_buf, 0x20u, p->dst_id);
            pm_wr_u32(es_buf, 0x34u, TARGET_6K_W);
            pm_wr_u32(es_buf, 0x38u, TARGET_6K_H);
            pm_wr_u32(es_buf, 0x4Cu, TARGET_6K_STRIDE);
            pm_wr_u16(es_buf, 0x56u, (u16)TARGET_6K_VCORE0);
            pm_wr_u16(es_buf, 0x8Au, (u16)TARGET_6K_W);
            pm_wr_u16(es_buf, 0x8Cu, (u16)TARGET_6K_H);
            es_buf[0x97u] = 1u; es_buf[0x9Eu] = 0xB7u;

            if (!avcam_write_phys(fd, AVCAM_BASE_PHYS + p->dst_cc_off, cc_buf, CC_ROW_LEN) ||
                !avcam_write_phys(fd, AVCAM_BASE_PHYS + p->dst_es_off, es_buf, ES_ROW_LEN)) {
                close(fd); return "AVCAM_WR_FAIL";
            }
        } else {
            if (!avcam_write_phys(fd, AVCAM_BASE_PHYS + p->dst_cc_off, g_avcam6k_orig_cc[i], CC_ROW_LEN) ||
                !avcam_write_phys(fd, AVCAM_BASE_PHYS + p->dst_es_off, g_avcam6k_orig_es[i], ES_ROW_LEN)) {
                close(fd); return "AVCAM_RESTORE_FAIL";
            }
        }
    }

    static const u8 tier3_stock[16] = {
        0x00,0x00,0x00,0x00, 0x73,0x04,0x00,0x00, 0x76,0x04,0x00,0x00, 0x79,0x04,0x00,0x00
    };
    static const u8 tier2_map[16] = {
        0x86,0x04,0x00,0x00, 0x74,0x04,0x00,0x00, 0x77,0x04,0x00,0x00, 0x7A,0x04,0x00,0x00
    };
    if (!avcam_write_phys(fd, AVCAM_BASE_PHYS + AVCAM_TIER3_OFF,
                          enable ? tier2_map : tier3_stock, 16u)) {
        close(fd); return "AVCAM_TIER3_FAIL";
    }
    close(fd);
    return "APPLY_OK";
}

static void make_cp_rec(u8 rec[20], u32 align_shift, u32 ext_id, u32 sz, u32 fl) {
    pm_wr_u32(rec, 0u,  0u);
    pm_wr_u32(rec, 4u,  align_shift);
    pm_wr_u32(rec, 8u,  ext_id);
    pm_wr_u32(rec, 12u, sz);
    pm_wr_u32(rec, 16u, fl);
}

static u32 cp_lookup_blk(const PilotApi *api, u32 blk_id) {
    u8 b2[2], b4[4];
    if (api->cp_mem->read(CP_BLK_IDX_TBL + (u64)blk_id * 2ULL, b2, 2u) != PILOT_STATUS_OK) return 0;
    u16 bidx = pm_rd_u16(b2, 0u);
    if (bidx >= 0x400u) return 0;
    if (api->cp_mem->read(CP_BLK_HDR_ARR + (u64)bidx * 0x50ULL, b4, 4u) != PILOT_STATUS_OK) return 0;
    return pm_rd_u32(b4, 0u);
}

static int cp_wr_verify(const PilotApi *api, u64 va, const u8 *buf, size_t len) {
    u8 rb[64];
    if (len > sizeof(rb)) return 0;
    if (api->cp_mem->write(va, buf, len) != PILOT_STATUS_OK) return 0;
    if (api->cp_mem->read(va, rb, len) != PILOT_STATUS_OK) return 0;
    return memcmp(rb, buf, len) == 0;
}

static const char *apply_6k_cp(const PilotApi *api, int enable) {
    if (!api || !api->cp_mem || !api->cp_mem->read || !api->cp_mem->write)
        return "CP_API_MISSING";

    if (!g_cp6k_saved) {
        u8 su = 0xff, r446[8], r498[8];
        if (api->cp_mem->read(CP_SCALER_UNITS, &su, 1u) != PILOT_STATUS_OK || (su != 0u && su != 3u))
            return "CP_SU_RD_FAIL";
        if (api->cp_mem->read(CP_UNIT_ROW_446, r446, 8u) != PILOT_STATUS_OK ||
            api->cp_mem->read(CP_UNIT_ROW_498, r498, 8u) != PILOT_STATUS_OK)
            return "CP_ROW_RD_FAIL";
        u32 id446 = pm_rd_u32(r446, 0u), kp446 = pm_rd_u32(r446, 4u);
        u32 id498 = pm_rd_u32(r498, 0u), kp498 = pm_rd_u32(r498, 4u);
        if ((id446 != 0x446u && id446 != 0x43Fu) || id498 != 0x498u || kp498 < 0x025B8000u)
            return "CP_ROW_FOREIGN";
        u32 r46 = cp_lookup_blk(api, 0x46u), r8a = cp_lookup_blk(api, 0x8Au);
        if (r46 < 0x02570000u || r46 >= 0x02600000u || r8a < 0x02570000u || r8a >= 0x02600000u)
            return "CP_BLK_RANGE_FAIL";
        if (api->cp_mem->read(r46, g_cp6k_orig_rec46, 60u) != PILOT_STATUS_OK ||
            api->cp_mem->read(r8a, g_cp6k_orig_rec8a, 60u) != PILOT_STATUS_OK)
            return "CP_REC_RD_FAIL";
        if (pm_rd_u32(g_cp6k_orig_rec46, 8u) != 0x05F2u || pm_rd_u32(g_cp6k_orig_rec8a, 8u) != 0x057Fu)
            return "CP_REC_FOREIGN";
        g_cp6k_orig_su = 0u;
        g_cp6k_orig_kp446 = kp446;
        g_cp6k_kp498 = kp498;
        g_cp6k_rec46_va = r46;
        g_cp6k_rec8a_va = r8a;
        g_cp6k_saved = 1;
    }

    static const u8 idx3_stock[16] = {
        0x00,0x0F,0x70,0x08, 0x3E,0x04,0x00,0x00, 0x00,0x0F,0x70,0x08, 0xD9,0x02,0x00,0x00
    };
    static const u8 idx3_patch[16] = {
        0x80,0x16,0xA8,0x0C, 0x3E,0x04,0x00,0x00, 0x80,0x16,0xA8,0x0C, 0x3F,0x04,0x00,0x00
    };

    u8 su_val = enable ? 3u : g_cp6k_orig_su;
    u8 row446[8];
    pm_wr_u32(row446, 0u, enable ? 0x0000043Fu : 0x00000446u);
    pm_wr_u32(row446, 4u, enable ? g_cp6k_kp498 : g_cp6k_orig_kp446);

    u8 rec46[60], rec8a[60];
    if (enable) {
        for (int i = 0; i < 3; ++i)
            make_cp_rec(rec46 + i * 20, 7u, 0x05F2u, CP_5F2_NEW_SIZE, 0x0000010Fu);
        make_cp_rec(rec8a + 0,  7u, 0x057Fu, CP_57F_NEW_SIZE, 0x0000010Fu);
        make_cp_rec(rec8a + 20, 7u, 0x057Fu, CP_57F_NEW_SIZE, 0x0000010Fu);
        make_cp_rec(rec8a + 40, 4u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x0000010Fu);

        if (!cp_wr_verify(api, CP_SCALER_UNITS, &su_val, 1u)) return "CP_SU_WR_FAIL";
        if (!cp_wr_verify(api, CP_UNIT_ROW_446, row446, 8u))  return "CP_ROW_WR_FAIL";
        if (!cp_wr_verify(api, g_cp6k_rec46_va, rec46, 60u))  return "CP_R46_WR_FAIL";
        if (!cp_wr_verify(api, g_cp6k_rec8a_va, rec8a, 60u))  return "CP_R8A_WR_FAIL";
        if (!cp_wr_verify(api, CP_IDX3_VA, idx3_patch, 16u))  return "CP_IDX3_WR_FAIL";
    } else {
        if (!cp_wr_verify(api, CP_IDX3_VA, idx3_stock, 16u))           return "CP_IDX3_UN_FAIL";
        if (!cp_wr_verify(api, g_cp6k_rec8a_va, g_cp6k_orig_rec8a, 60u)) return "CP_R8A_UN_FAIL";
        if (!cp_wr_verify(api, g_cp6k_rec46_va, g_cp6k_orig_rec46, 60u)) return "CP_R46_UN_FAIL";
        if (!cp_wr_verify(api, CP_UNIT_ROW_446, row446, 8u))           return "CP_ROW_UN_FAIL";
        if (!cp_wr_verify(api, CP_SCALER_UNITS, &su_val, 1u))          return "CP_SU_UN_FAIL";
    }
    return "APPLY_OK";
}

static int run_6k(const PilotApi *api, int enable) {
    const char *ap = apply_6k_ap(enable);
    const char *av = apply_6k_avcam(enable);
    const char *cp = apply_6k_cp(api, enable);
    char line[192];
    snprintf(line, sizeof(line), "VMOD_6K en=%d ap=%s av=%s cp=%s", enable, ap, av, cp);
    pm_emit_log(api, line);
    int ok_ap = (strcmp(ap, "APPLY_OK") == 0 || strcmp(ap, "ALREADY_PATCHED") == 0);
    return ok_ap && (strcmp(av, "APPLY_OK") == 0) && (strcmp(cp, "APPLY_OK") == 0);
}

/* =========================================================================
 *  PART 4: 4K OpenGate (3840x2560) & 5.76K OpenGate (5760x3840)
 *          Credits: GitHub @resaviewsgit, Instagram @resaviews
 * ========================================================================= */
static int detect_timing_pal(const PilotApi *api) {
    if (g_timing_pal >= 0)
        return g_timing_pal;
    u8 b = 0;
    if (!api || !api->cp_mem || !api->cp_mem->read ||
        api->cp_mem->read(0x025DAEACULL + 0xBCULL, &b, 1u) != PILOT_STATUS_OK)
        return 0;
    if (b == 0x66u || b == 0x22u) {
        g_timing_pal = 1;
        return 1;
    }
    if (b == 0x7Fu || b == 0x3Eu) {
        g_timing_pal = 0;
        return 0;
    }
    return 0;
}

static const char *apply_al_libmpr_items(const al6k_ap_patch_t *items, size_t count,
                                         int enable, u32 *applied, u32 *skipped) {
    u64 base = pm_find_so_base("libmpr.so");
    if (!base) return "LIBMPR_NOT_FOUND";
    for (size_t i = 0; i < count; ++i) {
        const al6k_ap_patch_t *it = &items[i];
        const u8 *exp = enable ? it->old_b : it->new_b;
        const u8 *rep = enable ? it->new_b : it->old_b;
        u8 *dst = (u8 *)(uintptr_t)(base + it->addr);
        if (memcmp(dst, rep, it->len) == 0) { (*skipped)++; continue; }
        if (memcmp(dst, exp, it->len) != 0) return "LIBMPR_MISMATCH";
        u64 page = (base + it->addr) & ~(PM_PAGE_SIZE - 1ULL);
        size_t span = (size_t)((((base + it->addr) - page) + it->len + PM_PAGE_SIZE - 1ULL) & ~(PM_PAGE_SIZE - 1ULL));
        int is_code = (it->addr < 0x270000ULL);
        int orig_prot = is_code ? (PROT_READ | PROT_EXEC) : PROT_READ;
        if (mprotect((void *)(uintptr_t)page, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
            return "MPROTECT_RW_FAIL";
        memcpy(dst, rep, it->len);
        __builtin___clear_cache((char *)dst, (char *)(dst + it->len));
        (void)mprotect((void *)(uintptr_t)page, span, orig_prot);
        if (memcmp(dst, rep, it->len) != 0) return "LIBMPR_VERIFY_FAIL";
        (*applied)++;
    }
    return "APPLY_OK";
}

static const char *apply_al_avcam_items(const al6k_ap_patch_t *items, size_t count,
                                        int enable, u32 *applied, u32 *skipped) {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) return "DEVMEM_OPEN_FAIL";
    u8 cur[32], rb[32];
    for (size_t i = 0; i < count; ++i) {
        const al6k_ap_patch_t *it = &items[i];
        const u8 *exp = enable ? it->old_b : it->new_b;
        const u8 *rep = enable ? it->new_b : it->old_b;
        u64 phys = AVCAM_BASE_PHYS + it->addr;
        if (it->len == 0u || it->len > sizeof(cur)) { close(fd); return "AVCAM_LEN_ERR"; }
        if (pread(fd, cur, it->len, (off_t)phys) != (ssize_t)it->len) { close(fd); return "AVCAM_RD_FAIL"; }
        if (memcmp(cur, rep, it->len) == 0) { (*skipped)++; continue; }
        if (memcmp(cur, exp, it->len) != 0) { close(fd); return "AVCAM_MISMATCH"; }
        u64 page = phys & ~0xFFFULL;
        u64 p_off = phys - page;
        size_t map_len = (p_off + it->len > 0x1000ULL) ? 0x2000u : 0x1000u;
        void *m = mmap(0, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)page);
        if (m == MAP_FAILED) { close(fd); return "AVCAM_MMAP_FAIL"; }
        u8 *dst = (u8 *)m + p_off;
        memcpy(dst, rep, it->len);
        pm_dcache_clean_inval(dst, it->len);
        munmap(m, map_len);
        if (pread(fd, rb, it->len, (off_t)phys) != (ssize_t)it->len || memcmp(rb, rep, it->len) != 0) {
            close(fd);
            return "AVCAM_RB_FAIL";
        }
        (*applied)++;
    }
    close(fd);
    return "APPLY_OK";
}

static const char *apply_al_cp_ro_items(const PilotApi *api, const al6k_cp_patch_t *items,
                                        size_t count, int enable, u32 *applied, u32 *skipped) {
    for (size_t k = 0; k < count; ++k) {
        size_t i = enable ? k : (count - 1u - k);
        const al6k_cp_patch_t *it = &items[i];
        const u8 *exp = enable ? it->old_b : it->new_b;
        const u8 *rep = enable ? it->new_b : it->old_b;
        PilotStatus rc = pm_cp_patch_idempotent(api, (u64)it->addr, exp, rep, (size_t)it->len,
                                                applied, skipped);
        if (rc != PILOT_STATUS_OK) {
            u8 cur[4] = {0};
            (void)api->cp_mem->read((u64)it->addr, cur, it->len < 4u ? it->len : 4u);
            char err[128];
            snprintf(err, sizeof(err),
                     "VMOD_UIPC_ERR idx=%u va=0x%08x len=%u en=%d rc=%d cur=%02x%02x%02x%02x",
                     (unsigned)i, it->addr, (unsigned)it->len, enable, (int)rc,
                     cur[0], cur[1], cur[2], cur[3]);
            pm_emit_log(api, err);
            return "CP_UIPC_FAIL";
        }
    }
    return "APPLY_OK";
}

static const char *apply_al_cp_ram_items(const PilotApi *api, const al6k_cp_patch_t *items,
                                         size_t count, int enable, u32 *applied, u32 *skipped) {
    u8 cur[64], rb[64];
    for (size_t k = 0; k < count; ++k) {
        size_t i = enable ? k : (count - 1u - k);
        const al6k_cp_patch_t *it = &items[i];
        const u8 *exp = enable ? it->old_b : it->new_b;
        const u8 *rep = enable ? it->new_b : it->old_b;
        if (it->len == 0u || it->len > sizeof(cur)) return "CP_LEN_ERR";
        if (api->cp_mem->read((u64)it->addr, cur, it->len) != PILOT_STATUS_OK) return "CP_DMA_RD_FAIL";
        if (memcmp(cur, rep, it->len) == 0) { (*skipped)++; continue; }
        if (memcmp(cur, exp, it->len) != 0) {
            char err[128];
            snprintf(err, sizeof(err), "VMOD_DMA_ERR idx=%u va=0x%08x step=cmp cur=%02x%02x%02x%02x",
                     (unsigned)i, it->addr, cur[0], it->len > 1u ? cur[1] : 0u,
                     it->len > 2u ? cur[2] : 0u, it->len > 3u ? cur[3] : 0u);
            pm_emit_log(api, err);
            return "CP_DMA_MISMATCH";
        }
        if (api->cp_mem->write((u64)it->addr, rep, it->len) != PILOT_STATUS_OK) {
            if (api->cp_mem->patch((u64)it->addr, exp, rep, it->len) != PILOT_STATUS_OK)
                return "CP_DMA_WR_FAIL";
        }
        if (api->cp_mem->read((u64)it->addr, rb, it->len) != PILOT_STATUS_OK || memcmp(rb, rep, it->len) != 0)
            return "CP_DMA_RB_FAIL";
        (*applied)++;
    }
    return "APPLY_OK";
}

static int run_al_og(const PilotApi *api, int is_4k, int enable) {
    u32 w_cp = 0, sk_cp = 0, w_mp = 0, sk_mp = 0, w_av = 0, sk_av = 0;
    const char *rc_mp = "SKIP", *rc_av = "SKIP", *rc_lcd = "SKIP";
    const char *rc_sen_ro = "SKIP", *rc_dmm = "SKIP", *rc_sen_ram = "SKIP";
    const char *rc_srec = "SKIP", *rc_cp = "SKIP";

    int pal = detect_timing_pal(api);
    const al6k_cp_patch_t *srec_ptr = pal ? AL6K_SREC_PAL_CP_RAM : AL6K_SREC_NTSC_CP_RAM;
    size_t srec_n = pal ? AL6K_SREC_PAL_CP_RAM_COUNT : AL6K_SREC_NTSC_CP_RAM_COUNT;
    const al6k_cp_patch_t *mode_cp = is_4k ? AL6K_4K_CP_RAM : AL6K_57K_CP_RAM;
    size_t mode_cp_n = is_4k ? AL6K_4K_CP_RAM_COUNT : AL6K_57K_CP_RAM_COUNT;
    const al6k_ap_patch_t *mode_mp = is_4k ? AL6K_4K_LIBMPR : AL6K_57K_LIBMPR;
    size_t mode_mp_n = is_4k ? AL6K_4K_LIBMPR_COUNT : AL6K_57K_LIBMPR_COUNT;
    const al6k_ap_patch_t *mode_av = is_4k ? AL6K_4K_AVCAM : AL6K_57K_AVCAM;
    size_t mode_av_n = is_4k ? AL6K_4K_AVCAM_COUNT : AL6K_57K_AVCAM_COUNT;

    if (enable) {
        /* Cold AP + AV-CAM + CP RO tables first; live DMM/Sensor/idx3 RAM last right before 0xCA */
        rc_mp = apply_al_libmpr_items(mode_mp, mode_mp_n, 1, &w_mp, &sk_mp);
        rc_av = apply_al_avcam_items(mode_av, mode_av_n, 1, &w_av, &sk_av);
        rc_lcd = apply_al_cp_ro_items(api, AL6K_LCD_CP_RO, AL6K_LCD_CP_RO_COUNT, 1, &w_cp, &sk_cp);
        if (strcmp(rc_lcd, "APPLY_OK") == 0)
            rc_sen_ro = apply_al_cp_ro_items(api, AL6K_SENSOR_CP_RO, AL6K_SENSOR_CP_RO_COUNT, 1, &w_cp, &sk_cp);
        if (strcmp(rc_sen_ro, "APPLY_OK") == 0) {
            rc_dmm = apply_al_cp_ram_items(api, AL6K_DMM_CP_RAM, AL6K_DMM_CP_RAM_COUNT, 1, &w_cp, &sk_cp);
            if (strcmp(rc_dmm, "APPLY_OK") == 0)
                rc_sen_ram = apply_al_cp_ram_items(api, AL6K_SENSOR_CP_RAM, AL6K_SENSOR_CP_RAM_COUNT, 1, &w_cp, &sk_cp);
            if (strcmp(rc_sen_ram, "APPLY_OK") == 0)
                rc_srec = apply_al_cp_ram_items(api, srec_ptr, srec_n, 1, &w_cp, &sk_cp);
            if (strcmp(rc_srec, "APPLY_OK") == 0)
                rc_cp = apply_al_cp_ram_items(api, mode_cp, mode_cp_n, 1, &w_cp, &sk_cp);
        }
    } else {
        rc_cp = apply_al_cp_ram_items(api, mode_cp, mode_cp_n, 0, &w_cp, &sk_cp);
        if (strcmp(rc_cp, "APPLY_OK") == 0)
            rc_srec = apply_al_cp_ram_items(api, srec_ptr, srec_n, 0, &w_cp, &sk_cp);
        if (strcmp(rc_srec, "APPLY_OK") == 0)
            rc_sen_ram = apply_al_cp_ram_items(api, AL6K_SENSOR_CP_RAM, AL6K_SENSOR_CP_RAM_COUNT, 0, &w_cp, &sk_cp);
        if (strcmp(rc_sen_ram, "APPLY_OK") == 0)
            rc_dmm = apply_al_cp_ram_items(api, AL6K_DMM_CP_RAM, AL6K_DMM_CP_RAM_COUNT, 0, &w_cp, &sk_cp);
        if (strcmp(rc_dmm, "APPLY_OK") == 0)
            rc_sen_ro = apply_al_cp_ro_items(api, AL6K_SENSOR_CP_RO, AL6K_SENSOR_CP_RO_COUNT, 0, &w_cp, &sk_cp);
        if (strcmp(rc_sen_ro, "APPLY_OK") == 0)
            rc_lcd = apply_al_cp_ro_items(api, AL6K_LCD_CP_RO, AL6K_LCD_CP_RO_COUNT, 0, &w_cp, &sk_cp);
        rc_mp = apply_al_libmpr_items(mode_mp, mode_mp_n, 0, &w_mp, &sk_mp);
        rc_av = apply_al_avcam_items(mode_av, mode_av_n, 0, &w_av, &sk_av);
    }

    char line[256];
    snprintf(line, sizeof(line),
             "%s en=%d pal=%d dmm=%s sro=%s sram=%s srec=%s cp=%s lcd=%s mpr=%s av=%s w=%u/%u/%u sk=%u/%u/%u",
             is_4k ? "VMOD_OG4K" : "VMOD_OG57K", enable, pal,
             rc_dmm, rc_sen_ro, rc_sen_ram, rc_srec, rc_cp, rc_lcd, rc_mp, rc_av,
             w_cp, w_mp, w_av, sk_cp, sk_mp, sk_av);
    pm_emit_log(api, line);

    return (strcmp(rc_dmm, "APPLY_OK") == 0) &&
           (strcmp(rc_sen_ro, "APPLY_OK") == 0) &&
           (strcmp(rc_sen_ram, "APPLY_OK") == 0) &&
           (strcmp(rc_srec, "APPLY_OK") == 0) &&
           (strcmp(rc_cp, "APPLY_OK") == 0) &&
           (strcmp(rc_lcd, "APPLY_OK") == 0) &&
           (strcmp(rc_mp, "APPLY_OK") == 0) &&
           (strcmp(rc_av, "APPLY_OK") == 0);
}

/* =========================================================================
 *  Dynamic Native UI File Format Label Overrides
 * ========================================================================= */
static void update_format_labels(int mode) {
    if (mode == VMODE_6K) {
        (void)pilot_ui_set_rid_override(RID_XAVC_HS_4K, "HS 6K 420 (24-50p)");
        (void)pilot_ui_set_rid_override(RID_XAVC_S_HD,  0);
        (void)pilot_ui_set_rid_override(RID_XAVC_SI_HD, 0);
    } else if (mode == VMODE_OG2K) {
        (void)pilot_ui_set_rid_override(RID_XAVC_HS_4K, 0);
        (void)pilot_ui_set_rid_override(RID_XAVC_S_HD,  "S 2K OG (24-60p)");
        (void)pilot_ui_set_rid_override(RID_XAVC_SI_HD, "S-I 2K OG (24-60p)");
    } else if (mode == VMODE_1080OS) {
        (void)pilot_ui_set_rid_override(RID_XAVC_HS_4K, 0);
        (void)pilot_ui_set_rid_override(RID_XAVC_S_HD,  "S HD 6KOS (24-60p)");
        (void)pilot_ui_set_rid_override(RID_XAVC_SI_HD, "S-I HD 6KOS (24-60p)");
    } else if (mode == VMODE_OG4K) {
        (void)pilot_ui_set_rid_override(RID_XAVC_HS_4K, "HS 4K OG (24-25p)");
        (void)pilot_ui_set_rid_override(RID_XAVC_S_HD,  0);
        (void)pilot_ui_set_rid_override(RID_XAVC_SI_HD, 0);
    } else if (mode == VMODE_OG57K) {
        (void)pilot_ui_set_rid_override(RID_XAVC_HS_4K, "HS 5.76K OG (24-25p)");
        (void)pilot_ui_set_rid_override(RID_XAVC_S_HD,  0);
        (void)pilot_ui_set_rid_override(RID_XAVC_SI_HD, 0);
    } else {
        (void)pilot_ui_set_rid_override(RID_XAVC_HS_4K, 0);
        (void)pilot_ui_set_rid_override(RID_XAVC_S_HD,  0);
        (void)pilot_ui_set_rid_override(RID_XAVC_SI_HD, 0);
    }
}

/* =========================================================================
 *  Mutually-Exclusive State Machine
 *  (Stock <-> 2K OG <-> 6K <-> 1080p 6K OS <-> 4K OG <-> 5.76K OG)
 * ========================================================================= */
static int switch_mode(const PilotApi *api, int from_mode, int to_mode, int is_boot) {
    if (from_mode == to_mode) return 1;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int ok = 1;
    /* 1. Revert previous active mode cleanly first */
    if (from_mode == VMODE_OG2K)
        ok = run_og2k(api, 0);
    else if (from_mode == VMODE_6K)
        ok = run_6k(api, 0);
    else if (from_mode == VMODE_1080OS)
        ok = run_1080os(api, 0);
    else if (from_mode == VMODE_OG4K)
        ok = run_al_og(api, 1, 0);
    else if (from_mode == VMODE_OG57K)
        ok = run_al_og(api, 0, 0);

    /* 2. Apply new target mode */
    if (ok) {
        if (to_mode == VMODE_OG2K)
            ok = run_og2k(api, 1);
        else if (to_mode == VMODE_6K)
            ok = run_6k(api, 1);
        else if (to_mode == VMODE_1080OS)
            ok = run_1080os(api, 1);
        else if (to_mode == VMODE_OG4K)
            ok = run_al_og(api, 1, 1);
        else if (to_mode == VMODE_OG57K)
            ok = run_al_og(api, 0, 1);
    }

    /* 3. Update native File Format menu labels & reload AP + CP pipeline */
    if (ok) {
        update_format_labels(to_mode);
        reload_movie_pipeline(api, is_boot);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;

    char line[160];
    snprintf(line, sizeof(line), "VMOD_SWITCH %s -> %s ok=%d ms=%ld",
             kModeNames[from_mode], kModeNames[to_mode], ok, ms);
    pm_emit_log(api, line);

    char toast[96];
    if (ok)
        snprintf(toast, sizeof(toast), "Video Mode: %s", kModeNames[to_mode]);
    else
        snprintf(toast, sizeof(toast), "Video Mode err (%s)", kModeNames[to_mode]);
    show_toast(toast, 4000u);
    return ok;
}

/* --- Declarative Pilot Menu Option --- */
static void on_vmode_changed(PilotUiOption *opt, void *user) {
    (void)user;
    if (!opt) return;
    int next = opt->value;
    if (next < 0 || next >= VMODE_COUNT) next = VMODE_STOCK;

    save_config_mode(g_api, next);

    char line[112];
    snprintf(line, sizeof(line), "VMOD_MENU_SET desired=%d(%s) (saved config.txt)",
             next, kModeNames[next]);
    pm_emit_log(g_api, line);

    int need_switch = 0;
    pthread_mutex_lock(&g_state_lock);
    if (g_cp_ready && next != g_applied_mode)
        need_switch = 1;
    pthread_mutex_unlock(&g_state_lock);

    if (need_switch) {
        char busy[128];
        snprintf(busy, sizeof(busy), "Switching to %s...\nPlease wait", kModeNames[next]);
        (void)pilot_ui_show_busy(busy);
    }

    pthread_mutex_lock(&g_state_lock);
    g_desired_mode = next;
    pthread_cond_signal(&g_state_cond);
    pthread_mutex_unlock(&g_state_lock);
}

static PilotUiOption g_vmode_options[] = {
    {
        .label = "Video Mode",
        .kind = PILOT_UI_OPT_ENUM,
        .value = VMODE_STOCK,
        .choices = kModeNames,
        .choice_count = VMODE_COUNT,
        .on_change = on_vmode_changed,
    },
};

/* Check whether CP RAM was wiped back to stock (e.g., after soft power-off/on
 * STR wake or Auto Power Off standby wake where AP stays alive while CP reboots).
 * Returns 1 ONLY when CP DMA read succeeds AND bytes explicitly match stock. */
static int check_cp_reset_to_stock(const PilotApi *api, int mode) {
    if (!api || !api->cp_mem || !api->cp_mem->read || mode == VMODE_STOCK)
        return 0;

    if (mode == VMODE_OG2K) {
        u8 b_dma[4] = {0}, b_uipc[4] = {0};
        const struct cp_2k_patch *p_dma  = &CP_2K_DMA[1];  /* DRVREC_idx10_0x054 (2B) */
        const struct cp_2k_patch *p_uipc = &CP_2K_UIPC[0]; /* DIST_ASP_0x4C9684  (1B) */
        if (api->cp_mem->read(p_dma->va, b_dma, p_dma->len) == PILOT_STATUS_OK &&
            memcmp(b_dma, p_dma->stock, p_dma->len) == 0)
            return 1;
        if (api->cp_mem->read(p_uipc->va, b_uipc, p_uipc->len) == PILOT_STATUS_OK &&
            memcmp(b_uipc, p_uipc->stock, p_uipc->len) == 0)
            return 1;
        return 0;
    }

    if (mode == VMODE_1080OS) {
        u8 b_dma[4] = {0}, b_uipc[4] = {0};
        const struct cp_2k_patch *p_dma  = &CP_1080OS_DMA[0];
        const struct cp_2k_patch *p_uipc = &CP_1080OS_UIPC[0];
        if (api->cp_mem->read(p_dma->va, b_dma, p_dma->len) == PILOT_STATUS_OK &&
            memcmp(b_dma, p_dma->stock, p_dma->len) == 0)
            return 1;
        if (api->cp_mem->read(p_uipc->va, b_uipc, p_uipc->len) == PILOT_STATUS_OK &&
            memcmp(b_uipc, p_uipc->stock, p_uipc->len) == 0)
            return 1;
        return 0;
    }

    if (mode == VMODE_6K || mode == VMODE_OG4K || mode == VMODE_OG57K) {
        static const u8 idx3_4k[4] = {0x00, 0x0F, 0x70, 0x08};
        u8 cur[4] = {0};
        if (api->cp_mem->read(CP_IDX3_VA, cur, 4u) == PILOT_STATUS_OK &&
            memcmp(cur, idx3_4k, 4u) == 0)
            return 1;
        return 0;
    }

    return 0;
}

/* --- Single Background Worker --- */
static void *vmode_worker(void *unused) {
    (void)unused;
    const PilotApi *api = g_api;
    char line[128];

    pm_emit_log(api, "VMOD_START unified=1 modes=Stock|2K_OG|6K_HS|1080p_6K_OS|4K_OG|5.76K_OG");

    /* Wait until CP output table (idx3/idx5) is initialized & /dev/pcidma ready */
    PilotStatus prc = pm_cp_wait_nonzero_u16(api, CP_IDX3_VA, 16u, 150u, 100u);
    if (prc != PILOT_STATUS_OK) {
        snprintf(line, sizeof(line), "VMOD_CP_NOT_READY rc=%d", (int)prc);
        pm_emit_log(api, line);
        return 0;
    }

    pthread_mutex_lock(&g_state_lock);
    g_cp_ready = 1;
    int init_mode = g_desired_mode;
    pthread_mutex_unlock(&g_state_lock);

    if (init_mode != VMODE_STOCK) {
        wait_appfw_settle(api, 0u);
        int init_ok = switch_mode(api, VMODE_STOCK, init_mode, 1);
        pthread_mutex_lock(&g_state_lock);
        if (init_ok) {
            g_applied_mode = init_mode;
        } else {
            g_desired_mode = VMODE_STOCK;
            g_vmode_options[0].value = VMODE_STOCK;
            save_config_mode(api, VMODE_STOCK);
        }
        pthread_mutex_unlock(&g_state_lock);
    } else {
        pm_emit_log(api, "VMOD_INIT_STOCK config mode=0");
    }

    for (;;) {
        pthread_mutex_lock(&g_state_lock);
        int do_wake_reapply = 0;
        while (g_desired_mode == g_applied_mode) {
            if (g_applied_mode == VMODE_STOCK) {
                pthread_cond_wait(&g_state_cond, &g_state_lock);
            } else {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 250000000L; /* 250ms wake watchdog */
                if (ts.tv_nsec >= 1000000000L) {
                    ts.tv_sec += 1;
                    ts.tv_nsec -= 1000000000L;
                }
                (void)pthread_cond_timedwait(&g_state_cond, &g_state_lock, &ts);
                if (g_desired_mode == g_applied_mode) {
                    int active = g_applied_mode;
                    pthread_mutex_unlock(&g_state_lock);
                    if (check_cp_reset_to_stock(api, active)) {
                        usleep(50000);
                        if (check_cp_reset_to_stock(api, active)) {
                            do_wake_reapply = 1;
                            pthread_mutex_lock(&g_state_lock);
                            break;
                        }
                    }
                    pthread_mutex_lock(&g_state_lock);
                }
            }
        }
        int cur    = g_applied_mode;
        int target = g_desired_mode;
        pthread_mutex_unlock(&g_state_lock);

        if (do_wake_reapply && target == cur && cur != VMODE_STOCK) {
            (void)pm_cp_wait_nonzero_u16(api, CP_IDX3_VA, 16u, 50u, 50u);
            wait_appfw_settle(api, 0u);
            int ok = 0;
            if (cur == VMODE_OG2K)
                ok = run_og2k(api, 1);
            else if (cur == VMODE_6K)
                ok = run_6k(api, 1);
            else if (cur == VMODE_1080OS)
                ok = run_1080os(api, 1);
            else if (cur == VMODE_OG4K)
                ok = run_al_og(api, 1, 1);
            else if (cur == VMODE_OG57K)
                ok = run_al_og(api, 0, 1);
            if (ok) {
                update_format_labels(cur);
                reload_movie_pipeline(api, 1);
            }
            snprintf(line, sizeof(line), "VMOD_WAKE_REAPPLY mode=%s ok=%d",
                     kModeNames[cur], ok);
            pm_emit_log(api, line);
            continue;
        }

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int sw_ok = switch_mode(api, cur, target, 0);
        pthread_mutex_lock(&g_state_lock);
        if (sw_ok) {
            g_applied_mode = target;
        } else {
            g_desired_mode = cur;
            g_vmode_options[0].value = cur;
            save_config_mode(api, cur);
        }
        pthread_mutex_unlock(&g_state_lock);

        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
        if (ms < 280L)
            usleep((useconds_t)((280L - ms) * 1000L));
        (void)pilot_ui_hide_busy();
    }
    return 0;
}

__attribute__((visibility("default")))
PilotStatus pilot_module_init(const PilotApi *api) {
    if (!api || api->abi_version < PILOT_ABI_VERSION || api->struct_size < sizeof(PilotApi))
        return PILOT_STATUS_INVALID_ARGUMENT;

    pthread_mutex_lock(&g_init_lock);
    if (g_started) {
        pthread_mutex_unlock(&g_init_lock);
        return PILOT_STATUS_OK;
    }
    g_api = api;
    g_started = 1;

    int saved = load_config_mode(api);
    g_desired_mode = saved;
    g_vmode_options[0].value = saved;
    save_config_mode(api, saved);
    update_format_labels(saved);

    /* Apply AP libmpr.so (+ AVCAM) geometry IMMEDIATELY at module init — BEFORE
     * appFw finishes building the initial boot LiveView display layout — matching
     * the warm-wake state where libmpr.so is already 1920x1280 and 0xCA+0x32F
     * enters full 3:2 cleanly without a bottom 1/4 solid color bar. */
    if (saved == VMODE_OG2K) {
        const char *eap = apply_og2k_ap(1);
        const char *eav = apply_og2k_avcam(api, 1);
        char emsg[96];
        snprintf(emsg, sizeof(emsg), "VMOD_EARLY_INIT mode=1 ap=%s av=%s", eap, eav);
        pm_emit_log(api, emsg);
    } else if (saved == VMODE_6K) {
        const char *eap = apply_6k_ap(1);
        char emsg[96];
        snprintf(emsg, sizeof(emsg), "VMOD_EARLY_INIT mode=2 ap=%s", eap);
        pm_emit_log(api, emsg);
    } else if (saved == VMODE_OG4K || saved == VMODE_OG57K) {
        u32 wm = 0, sm = 0, wa = 0, sa = 0;
        int is_4k = (saved == VMODE_OG4K);
        const char *eap = apply_al_libmpr_items(is_4k ? AL6K_4K_LIBMPR : AL6K_57K_LIBMPR,
                                                is_4k ? AL6K_4K_LIBMPR_COUNT : AL6K_57K_LIBMPR_COUNT,
                                                1, &wm, &sm);
        const char *eav = apply_al_avcam_items(is_4k ? AL6K_4K_AVCAM : AL6K_57K_AVCAM,
                                               is_4k ? AL6K_4K_AVCAM_COUNT : AL6K_57K_AVCAM_COUNT,
                                               1, &wa, &sa);
        char emsg[96];
        snprintf(emsg, sizeof(emsg), "VMOD_EARLY_INIT mode=%d ap=%s av=%s", saved, eap, eav);
        pm_emit_log(api, emsg);
    }

    PilotStatus ust = pilot_ui_register_options(
        MODULE_SO_NAME,
        g_vmode_options,
        sizeof(g_vmode_options) / sizeof(g_vmode_options[0]));
    if (ust != PILOT_STATUS_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "VMOD_MENU_REG_FAIL status=%d", (int)ust);
        pm_emit_log(api, msg);
    } else {
        char msg[96];
        snprintf(msg, sizeof(msg), "VMOD_MENU_READY mode=%d(%s)", saved, kModeNames[saved]);
        pm_emit_log(api, msg);
    }
    pthread_mutex_unlock(&g_init_lock);

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&tid, &attr, vmode_worker, 0);
    pthread_attr_destroy(&attr);
    if (rc != 0) return PILOT_STATUS_SYSTEM;
    return PILOT_STATUS_OK;
}
