/* A6700 Multi-Ratio & OpenGate Anamorphic De-Squeeze Pilot Module
 * (Standalone, ILCE-6700 FW 2.00)
 *
 * Activates Sony's built-in FX3/FX30 hardware VDF De-Squeeze pipeline compiled
 * into A6700 CP firmware (cpapp-b.bin 2.00, SHA-256 61b7fe5c...) with full
 * multi-ratio support and dual source-aspect bases (16:9 & 3:2 OpenGate):
 *
 *   Mode                16:9 Base                3:2 OpenGate Base
 *   -------------------------------------------------------------------------
 *   Off                 stock                    stock (preserves 2K OG 3:2)
 *   1.33x               mode 1, 27/64  (2.37:1)  mode 1,  1/2   (2.00:1)
 *   1.3x                mode 1, 45/104 (2.31:1)  mode 1, 20/39  (1.95:1)
 *   1.5x                mode 1,  3/8   (2.67:1)  mode 1,  4/9   (2.25:1)
 *   1.6x                mode 1, 45/128 (2.84:1)  mode 1,  5/12  (2.40:1)
 *   1.8x                mode 1,  5/16  (3.20:1)  mode 1, 10/27  (2.70:1)
 *   2.0x (Crop)         mode 2,  3/8   (2.67:1)  mode 2,  3/8   (2.67:1, 4:3 crop)
 *   2.0x (Full)         mode 1,  9/32  (3.56:1)  mode 1,  1/3   (3.00:1)
 *
 * CP Code Patches (cpapp-b.bin 2.00):
 *   - 0x006BA3B0 (20B): DSQ_CALC dispatch guard — checks aspect (16:9 / 17:9)
 *                       BEFORE mode 2 / mode 1 so BOTH mode 1 and mode 2
 *                       protect Still Photo (3:2 / 4:3 / 1:1) cleanly.
 *   - 0x006BA3CA (16B): Non-16:9/17:9 fallback -> tail-call RATIO_FN (0x1F6170)
 *                       so Still Photo stays 100% stock on LCD, EVF, and HDMI.
 *   - 0x006BA3E8 (6B):  DSQ_CALC (0x6BA392) mode 1 ratio (mov.w sb,#n; movs r6,#d)
 *   - 0x001F659A (4B):  HDMI manual helper (0x1F6564) mode 1 ratio copy 1
 *   - 0x001F662E (4B):  HDMI manual helper (0x1F6564) mode 1 ratio copy 2
 *   - 0x006BA3E0 (6B):  DSQ_CALC (0x6BA392) mode 2 ratio (3/8 or 4/9 for OG)
 *   - 0x001F6586 (4B):  HDMI manual helper (0x1F6564) mode 2 ratio (3/8 or 4/9)
 *   - 0x00128600 (2B):  AF_RECT_PASS  (cpActAfWindow 0x128438: 00 20 -> 01 20)
 *   - 0x00129472 (2B):  TRK_RECT_PASS (cpActTrackingFrame 0x129288: 00 20 -> 01 20)
 *
 * Activation uses Sony's native VDF synchronous IPC command
 * VdfInputCmdSetDeSqueeze (0x2001/0x0327 -> route 0x9059000E) via appFw.so's
 * VDF helpers, with independent LCD / EVF / HDMI switches, CP DISP shadow sync
 * (0x01FF2928..0x01FF2934) for Focus Magnifier, DMA fallback + seq 0x32F nudge,
 * 250ms sleep/wake watchdog, and safe coexistence with pilot_video_modes.so.
 */

#define _GNU_SOURCE
#include "pilot_mod_util.h"
#include "pilot_ui.h"
#include "localization.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define MODULE_SO_NAME       "pilot_desqueeze.so"
#define CONFIG_FILE_NAME     "config.txt"

/* --- Ratio & Source-Aspect Modes --- */
enum {
    DSQ_OFF       = 0,
    DSQ_133X      = 1,
    DSQ_130X      = 2,
    DSQ_150X      = 3,
    DSQ_160X      = 4,
    DSQ_180X      = 5,
    DSQ_200X_CROP = 6,
    DSQ_200X_FULL = 7,
    DSQ_COUNT     = 8
};

enum {
    SRC_16_9   = 0,
    SRC_3_2_OG = 1,
    SRC_COUNT  = 2
};

static const char *const kModeNames[DSQ_COUNT] = {
    "关闭",
    "1.33x",
    "1.3x",
    "1.5x",
    "1.6x",
    "1.8x",
    "2.0x (裁幅)",
    "2.0x (全宽)"
};

static const char *const kModeShort[DSQ_COUNT] = {
    "Off",
    "1.33x",
    "1.3x",
    "1.5x",
    "1.6x",
    "1.8x",
    "2.0x Crop",
    "2.0x Full"
};

static const char *const kSrcNames[SRC_COUNT] = {
    "16:9 (常规)",
    "3:2 (OpenGate)"
};

static const char *const kSrcShort[SRC_COUNT] = {
    "16:9",
    "3:2 OG"
};

struct dsq_ratio_spec {
    u8 vdf_mode; /* 0=off, 1=full width, 2=3/4 horizontal crop (Sony 2.0x) */
    u8 num_169;  /* height/width numerator for 16:9 source */
    u8 den_169;  /* height/width denominator for 16:9 source */
    u8 num_og;   /* height/width numerator for 3:2 OpenGate source */
    u8 den_og;   /* height/width denominator for 3:2 OpenGate source */
};

static const struct dsq_ratio_spec kRatios[DSQ_COUNT] = {
    /* DSQ_OFF       */ { 0u, 45u, 104u, 45u, 104u },
    /* DSQ_133X      */ { 1u, 27u,  64u,  1u,   2u }, /* 2.370:1 / 2.000:1 */
    /* DSQ_130X      */ { 1u, 45u, 104u, 20u,  39u }, /* 2.311:1 / 1.950:1 */
    /* DSQ_150X      */ { 1u,  3u,   8u,  4u,   9u }, /* 2.667:1 / 2.250:1 */
    /* DSQ_160X      */ { 1u, 45u, 128u,  5u,  12u }, /* 2.844:1 / 2.400:1 */
    /* DSQ_180X      */ { 1u,  5u,  16u, 10u,  27u }, /* 3.200:1 / 2.700:1 */
    /* DSQ_200X_CROP: Sony 0x6BC024 crops source to (4/3)*H_src for BOTH 16:9 (1440x1080)
     * and 3:2 OpenGate (1708x1280), so (4:3) * 2.0x = 2.667:1 (3/8) on both bases. */
    /* DSQ_200X_CROP */ { 2u,  3u,   8u,  3u,   8u }, /* 2.667:1 / 2.667:1 */
    /* DSQ_200X_FULL */ { 1u,  9u,  32u,  1u,   3u }, /* 3.556:1 / 3.000:1 */
};

/* --- CP RAM addresses (ILCE-6700 FW 2.00, cpapp-b.bin base 0x00108000) --- */
#define CP_IDX3_VA              0x0199ECBCULL /* 16B: CP ready sentinel */
#define CP_VDF_INPUT_DSQ_VA     0x01205C10ULL /* 4B: {mode, panel, evf, hdmi} */
#define CP_VDF_DEV_LCD_BASE     0x01214658ULL /* Output 1 (Rear Panel) */
#define CP_VDF_DEV_EVF_BASE     0x012066E8ULL /* Output 2 (EVF) */
#define CP_VDF_DEV_HDMI_BASE    0x0120D668ULL /* Output 4 (HDMI) */
#define CP_VDF_DEV_VTBL_BASE    0x00D0563Cu   /* base vtable written by 0x6c19a8 */
#define CP_VDF_DEV_VTBL_LCD     0x00D056A8u   /* runtime LCD derived vtable (HW verified) */
#define CP_VDF_DEV_DIRTY_OFF    0x00FAULL     /* 1B: relayout trigger flag */
#define CP_VDF_DEV_DSQ_OFF      0x6F79ULL     /* 2B: {mode, enable} */
#define CP_DISP_DSQ_SHADOW_VA   0x01FF2928ULL /* 16B: 4x u32 {mode, evf, panel, hdmi} */

#define CP_AF_RECT_LCD_169_VA   0x00E99E68ULL /* 8B: stock = (0, 38, 640, 404) */

/* Dynamic ratio instruction sites */
#define CP_DSQ_M1_MAIN_VA       0x006BA3E8u /* 6B: 4f f0 <num> 09 <den> 26 */
#define CP_DSQ_M1_HDMI1_VA      0x001F659Au /* 4B: <num> 24 <den> 25 */
#define CP_DSQ_M1_HDMI2_VA      0x001F662Eu /* 4B: <num> 24 <den> 25 */
#define CP_DSQ_M2_MAIN_VA       0x006BA3E0u /* 6B: 4f f0 <num> 09 <den> 26 */
#define CP_DSQ_M2_HDMI_VA       0x001F6586u /* 4B: <num> 24 <den> 25 */

/* --- appFw.so offsets (ILCE-6700 FW 2.00) --- */
#define APPFW_REPLY_MBX_CTOR_OFF    0x2D1AED0ULL /* void (u32 mbx[2], u32 kind) */
#define APPFW_REPLY_MBX_DTOR_OFF    0x34ABA6CULL /* void (u32 mbx[2]) */
#define APPFW_VDF_SEND_OFF          0x34CE2BCULL /* int (const void *cmd, u32 len, u32 reply_mbx, u32 route, u32 flag) */
#define APPFW_VDF_RECV_OFF          0x34CE4E8ULL /* int (void *resp, u32 len, u32 reply_mbx) */
#define APPFW_VDF_INPUT_ROUTE       0x9059000Eu

#define APPFW_GET_SEQ_MGR_OFF       0x328B434ULL
#define APPFW_SEQ_DISP_UPDATE_OFF   0x33CDE20ULL
#define APPFW_SEQ_IS_RUNNING_OFF    0x32C7694ULL
#define APPFW_SEQ_ID_DISP_UPDATE    0x32Fu

/* --- Fixed CP code patch descriptors --- */
struct cp_fixed_patch {
    const char *name;
    u32 va;
    u8 len;
    u8 stock[20];
    u8 patch[20];
};

static const struct cp_fixed_patch kFixedPatches[2] = {
    {
        /* 0x6BA3B0 (20B): guard aspect == 2 || aspect == 5 BEFORE mode 2 / mode 1
         * so BOTH mode 1 and mode 2 fall through to 0x6BA3CA for Photo (0/1/3):
         *   cmp r6, #2; beq +2; cmp r6, #5; bne 0x6ba3ca;
         *   cmp r0, #2; beq 0x6ba3e0; cmp r0, #1; bne 0x6ba3ca;
         *   b 0x6ba3e8; nop */
        "DSQ_DISPATCH_GUARD", 0x006BA3B0u, 20u,
        {0x02, 0x28, 0x15, 0xd0, 0x01, 0x28, 0x08, 0xd1,
         0x02, 0x2e, 0x15, 0xd0, 0x05, 0x2e, 0x04, 0xd1,
         0x40, 0xf2, 0xa3, 0x29},
        {0x02, 0x2e, 0x01, 0xd0, 0x05, 0x2e, 0x08, 0xd1,
         0x02, 0x28, 0x11, 0xd0, 0x01, 0x28, 0x04, 0xd1,
         0x12, 0xe0, 0x00, 0xbf}
    },
    {
        /* 0x6BA3CA (16B): non-16:9/17:9 fallback -> mov r1,r4; mov r3,r6;
         * pop.w {r8,sb,fp}; pop.w {r4,r5,r6,r7,lr}; b.w #0x1f6170 */
        "DSQ_PHOTO_FALLBACK", 0x006BA3CAu, 16u,
        {0x00, 0x20, 0xa8, 0xf8, 0x00, 0x00, 0xa8, 0xf8,
         0x02, 0x00, 0x20, 0x0c, 0xa8, 0xf8, 0x04, 0x40},
        {0x21, 0x46, 0x33, 0x46, 0xbd, 0xe8, 0x00, 0x0b,
         0xbd, 0xe8, 0xf0, 0x40, 0x3b, 0xf7, 0xcb, 0xb6}
    },
};

static const struct cp_fixed_patch kAfPassPatches[2] = {
    { "AF_RECT_PASS",  0x00128600u, 2u, {0x00, 0x20}, {0x01, 0x20} },
    { "TRK_RECT_PASS", 0x00129472u, 2u, {0x00, 0x20}, {0x01, 0x20} },
};

static const u8 kAfRectLcd169Stock[8] = {
    0x00, 0x00, 0x26, 0x00, 0x80, 0x02, 0x94, 0x01
};

/* --- Desired / Applied State --- */
struct dsq_state {
    int mode; /* DSQ_OFF .. DSQ_200X_FULL */
    int src;  /* SRC_16_9 or SRC_3_2_OG */
    int lcd;  /* 0/1 */
    int evf;  /* 0/1 */
    int hdmi; /* 0/1 */
};

static pthread_mutex_t g_init_lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_state_cond = PTHREAD_COND_INITIALIZER;

static const PilotApi *g_api = NULL;
static int g_started  = 0;
static int g_cp_ready = 0;

static struct dsq_state g_desired = { DSQ_OFF, SRC_16_9, 1, 1, 0 };
static struct dsq_state g_applied = { DSQ_OFF, SRC_16_9, 1, 1, 0 };

static int state_equal(const struct dsq_state *a, const struct dsq_state *b)
{
    return a->mode == b->mode && a->src == b->src &&
           a->lcd == b->lcd && a->evf == b->evf && a->hdmi == b->hdmi;
}

static void get_target_ratio(const struct dsq_state *st, u8 *out_vdf_mode, u8 *out_num, u8 *out_den)
{
    int m = (st->mode >= 0 && st->mode < DSQ_COUNT) ? st->mode : DSQ_OFF;
    const struct dsq_ratio_spec *r = &kRatios[m];
    *out_vdf_mode = r->vdf_mode;
    if (st->src == SRC_3_2_OG) {
        *out_num = r->num_og;
        *out_den = r->den_og;
    } else {
        *out_num = r->num_169;
        *out_den = r->den_169;
    }
}

static void show_toast(const char *msg, u32 ms)
{
    if (pilot_ui_show_text(msg, ms) != PILOT_STATUS_OK)
        pm_toast_show("dsq", 0, 36, 480, 32, msg, ms);
}

/* --- Config persistence --- */
static int parse_int_key(const char *buf, const char *key, int def_val, int min_v, int max_v)
{
    const char *p = strstr(buf, key);
    if (!p)
        return def_val;
    p += strlen(key);
    int v = (int)strtol(p, NULL, 10);
    if (v < min_v || v > max_v)
        return def_val;
    return v;
}

static void load_config(const PilotApi *api, struct dsq_state *st)
{
    st->mode = DSQ_OFF;
    st->src  = SRC_16_9;
    st->lcd  = 1;
    st->evf  = 1;
    st->hdmi = 0;

    if (!api || !api->file || !api->file->read)
        return;
    char buf[256];
    memset(buf, 0, sizeof(buf));
    if (api->file->read(api->file, CONFIG_FILE_NAME, buf, sizeof(buf)) != PILOT_STATUS_OK)
        return;

    st->mode = parse_int_key(buf, "mode=", DSQ_OFF,  0, DSQ_COUNT - 1);
    st->src  = parse_int_key(buf, "src=",  SRC_16_9, 0, SRC_COUNT - 1);
    st->lcd  = parse_int_key(buf, "lcd=",  1,        0, 1);
    st->evf  = parse_int_key(buf, "evf=",  1,        0, 1);
    st->hdmi = parse_int_key(buf, "hdmi=", 0,        0, 1);
}

static void save_config(const PilotApi *api, const struct dsq_state *st)
{
    if (!api || !api->file || !api->file->write)
        return;
    char buf[128];
    snprintf(buf, sizeof(buf), "mode=%d\nsrc=%d\nlcd=%d\nevf=%d\nhdmi=%d\n",
             st->mode, st->src, st->lcd, st->evf, st->hdmi);
    (void)api->file->write(api->file, CONFIG_FILE_NAME, buf);
}

/* --- CP readiness helper --- */
static int wait_cp_vdf_ready(const PilotApi *api, u32 max_tries, u32 step_ms)
{
    if (pm_cp_wait_nonzero_u16(api, CP_IDX3_VA, 16u, max_tries, step_ms) != PILOT_STATUS_OK)
        return 0;

    for (u32 i = 0; i < 60u; ++i) {
        u8 vbuf[4] = {0};
        if (api->cp_mem->read(CP_VDF_DEV_LCD_BASE, vbuf, 4u) == PILOT_STATUS_OK) {
            u32 vt = pm_rd_u32(vbuf, 0u);
            if (vt == CP_VDF_DEV_VTBL_LCD || vt == CP_VDF_DEV_VTBL_BASE)
                return 1;
        }
        pm_sleep_ms(50u);
    }
    return 1;
}

/* --- Trigger appFw display output update (seq 0x32F, no black flash) --- */
static void trigger_disp_update_32f(const PilotApi *api)
{
    u64 appfw = pm_find_so_base("appFw.so");
    if (!appfw) {
        pm_emit_log(api, "DSQ_32F skip=NO_APPFW");
        return;
    }

    typedef void *(*get_seq_mgr_fn)(void);
    typedef void  (*start_disp_fn)(void *mgr, u32 seq_id, void *arg, u32 flag);
    typedef int   (*is_running_fn)(void *mgr, u32 seq_id);

    get_seq_mgr_fn get_mgr = (get_seq_mgr_fn)(uintptr_t)(appfw + APPFW_GET_SEQ_MGR_OFF);
    start_disp_fn  start_d = (start_disp_fn)(uintptr_t)(appfw + APPFW_SEQ_DISP_UPDATE_OFF);
    is_running_fn  is_run  = (is_running_fn)(uintptr_t)(appfw + APPFW_SEQ_IS_RUNNING_OFF);

    void *mgr = get_mgr();
    if (!mgr) {
        pm_emit_log(api, "DSQ_32F skip=NO_SEQ_MGR");
        return;
    }

    start_d(mgr, APPFW_SEQ_ID_DISP_UPDATE, NULL, 1u);
    pm_sleep_ms(40u);
    for (u32 w = 0; w < 20u && is_run(mgr, APPFW_SEQ_ID_DISP_UPDATE); ++w)
        pm_sleep_ms(40u);
}

/* --- Aligned 4-byte RMW helper for CP data RAM (>= 0x011D8000) --- */
static int cp_dma_write_bytes_aligned(const PilotApi *api, u64 va, const u8 *bytes, size_t len)
{
    if (!api || !api->cp_mem || !api->cp_mem->read || !api->cp_mem->write || len == 0u || len > 8u)
        return 0;

    u64 aligned = va & ~3ULL;
    size_t off = (size_t)(va - aligned);
    size_t total = ((off + len) + 3u) & ~3u;
    if (total > 12u)
        return 0;

    u8 buf[12], rb[12];
    if (api->cp_mem->read(aligned, buf, total) != PILOT_STATUS_OK)
        return 0;
    memcpy(buf + off, bytes, len);
    if (api->cp_mem->write(aligned, buf, total) != PILOT_STATUS_OK)
        return 0;
    if (api->cp_mem->read(aligned, rb, total) != PILOT_STATUS_OK)
        return 0;
    return memcmp(rb + off, bytes, len) == 0;
}

/* --- Sync CP DISP shadow fields (used by VEF Focus Magnifier 0x1B8100/0x1B8370) --- */
static void sync_disp_shadow(const PilotApi *api, u8 mode, u8 evf, u8 panel, u8 hdmi)
{
    u8 buf[16];
    pm_wr_u32(buf, 0u,  (u32)mode);
    pm_wr_u32(buf, 4u,  (u32)evf);
    pm_wr_u32(buf, 8u,  (u32)panel);
    pm_wr_u32(buf, 12u, (u32)hdmi);
    (void)api->cp_mem->write(CP_DISP_DSQ_SHADOW_VA, buf, sizeof(buf));
}

/* --- Send Sony's native VdfInputCmdSetDeSqueeze (0x2001/0x0327 -> 0x9059000E) --- */
static int send_vdf_desqueeze_ipc(const PilotApi *api,
                                  u8 mode, u8 panel, u8 evf, u8 hdmi,
                                  u32 *out_resp_hdr, u32 *out_resp_st)
{
    u64 appfw = pm_find_so_base("appFw.so");
    if (!appfw)
        return -1;

    typedef void (*mbx_ctor_fn)(u32 *mbx, u32 kind);
    typedef void (*mbx_dtor_fn)(u32 *mbx);
    typedef int  (*vdf_send_fn)(const void *cmd, u32 len, u32 reply_mbx, u32 route, u32 flag);
    typedef int  (*vdf_recv_fn)(void *resp, u32 len, u32 reply_mbx);

    mbx_ctor_fn mbx_ctor = (mbx_ctor_fn)(uintptr_t)(appfw + APPFW_REPLY_MBX_CTOR_OFF);
    mbx_dtor_fn mbx_dtor = (mbx_dtor_fn)(uintptr_t)(appfw + APPFW_REPLY_MBX_DTOR_OFF);
    vdf_send_fn vdf_send = (vdf_send_fn)(uintptr_t)(appfw + APPFW_VDF_SEND_OFF);
    vdf_recv_fn vdf_recv = (vdf_recv_fn)(uintptr_t)(appfw + APPFW_VDF_RECV_OFF);

    u32 mbx[4] = {0, 0, 0, 0};
    u8 cmd[12] = {
        0x01u, 0x20u, 0x27u, 0x03u, /* 0x2001, 0x0327 = VdfInputCmdSetDeSqueeze */
        0x0Cu, 0x00u, 0x01u, 0x00u, /* len=12, ver=1 */
        mode, panel, evf, hdmi
    };
    u8 resp[12];
    memset(resp, 0, sizeof(resp));

    mbx_ctor(mbx, 2u);
    if (mbx[0] == 0u) {
        pm_emit_log(api, "DSQ_IPC err=MBX_CTOR_ZERO");
        return -2;
    }

    int s_rc = vdf_send(cmd, (u32)sizeof(cmd), mbx[0], APPFW_VDF_INPUT_ROUTE, 0u);
    int r_rc = -1;
    if (s_rc == 0)
        r_rc = vdf_recv(resp, (u32)sizeof(resp), mbx[0]);

    mbx_dtor(mbx);

    if (out_resp_hdr) *out_resp_hdr = pm_rd_u32(resp, 0u);
    if (out_resp_st)  *out_resp_st  = pm_rd_u32(resp, 8u);

    if (s_rc != 0 || r_rc != 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "DSQ_IPC fail send=%d recv=%d hdr=0x%08x st=0x%08x",
                 s_rc, r_rc, pm_rd_u32(resp, 0u), pm_rd_u32(resp, 8u));
        pm_emit_log(api, msg);
        return -3;
    }
    return 0;
}

/* --- Direct CP DMA fallback if VDF IPC fails --- */
static int send_vdf_desqueeze_dma_fallback(const PilotApi *api,
                                           u8 mode, u8 panel, u8 evf, u8 hdmi)
{
    u8 master[4] = { mode, panel, evf, hdmi };
    u8 p_lcd[2]  = { mode, panel };
    u8 p_evf[2]  = { mode, evf };
    u8 p_hdmi[2] = { mode, hdmi };
    u8 dirty[1]  = { 1u };

    int ok = 1;
    ok &= (api->cp_mem->write(CP_VDF_INPUT_DSQ_VA, master, 4u) == PILOT_STATUS_OK);
    ok &= cp_dma_write_bytes_aligned(api, CP_VDF_DEV_LCD_BASE  + CP_VDF_DEV_DSQ_OFF, p_lcd,  2u);
    ok &= cp_dma_write_bytes_aligned(api, CP_VDF_DEV_EVF_BASE  + CP_VDF_DEV_DSQ_OFF, p_evf,  2u);
    ok &= cp_dma_write_bytes_aligned(api, CP_VDF_DEV_HDMI_BASE + CP_VDF_DEV_DSQ_OFF, p_hdmi, 2u);
    ok &= cp_dma_write_bytes_aligned(api, CP_VDF_DEV_LCD_BASE  + CP_VDF_DEV_DIRTY_OFF, dirty, 1u);
    ok &= cp_dma_write_bytes_aligned(api, CP_VDF_DEV_EVF_BASE  + CP_VDF_DEV_DIRTY_OFF, dirty, 1u);
    ok &= cp_dma_write_bytes_aligned(api, CP_VDF_DEV_HDMI_BASE + CP_VDF_DEV_DIRTY_OFF, dirty, 1u);
    trigger_disp_update_32f(api);
    return ok ? 0 : -1;
}

/* --- Opcode-skeleton-checked dynamic ratio patchers (allow direct ratio->ratio hot-switch) --- */
static int patch_ratio_6b(const PilotApi *api, const char *name, u32 va,
                          u8 num, u8 den, u32 *app, u32 *sk)
{
    /* Skeleton: 4f f0 <num> 09 <den> 26  (mov.w sb, #num; movs r6, #den) */
    u8 cur[6] = {0};
    u8 rep[6] = {0x4fu, 0xf0u, num, 0x09u, den, 0x26u};

    if (api->cp_mem->read(va, cur, 6u) != PILOT_STATUS_OK)
        return 0;
    if (cur[0] != 0x4fu || cur[1] != 0xf0u || cur[3] != 0x09u || cur[5] != 0x26u) {
        char msg[128];
        snprintf(msg, sizeof(msg), "DSQ_SKEL6_ERR site=%s va=0x%08x cur=%02x%02x%02x%02x%02x%02x",
                 name, va, cur[0], cur[1], cur[2], cur[3], cur[4], cur[5]);
        pm_emit_log(api, msg);
        return 0;
    }
    if (memcmp(cur, rep, 6u) == 0) {
        (*sk)++;
        return 1;
    }
    if (api->cp_mem->patch(va, cur, rep, 6u) != PILOT_STATUS_OK)
        return 0;
    (*app)++;
    return 1;
}

static int patch_ratio_4b(const PilotApi *api, const char *name, u32 va,
                          u8 num, u8 den, u32 *app, u32 *sk)
{
    /* Skeleton: <num> 24 <den> 25  (movs r4, #num; movs r5, #den) */
    u8 cur[4] = {0};
    u8 rep[4] = {num, 0x24u, den, 0x25u};

    if (api->cp_mem->read(va, cur, 4u) != PILOT_STATUS_OK)
        return 0;
    if (cur[1] != 0x24u || cur[3] != 0x25u) {
        char msg[128];
        snprintf(msg, sizeof(msg), "DSQ_SKEL4_ERR site=%s va=0x%08x cur=%02x%02x%02x%02x",
                 name, va, cur[0], cur[1], cur[2], cur[3]);
        pm_emit_log(api, msg);
        return 0;
    }
    if (memcmp(cur, rep, 4u) == 0) {
        (*sk)++;
        return 1;
    }
    if (api->cp_mem->patch(va, cur, rep, 4u) != PILOT_STATUS_OK)
        return 0;
    (*app)++;
    return 1;
}

/* --- Apply or revert CP code patches for the target mode & source aspect --- */
static int apply_code_patches(const PilotApi *api, const struct dsq_state *st,
                              u32 *applied_out, u32 *skipped_out)
{
    u32 app = 0, sk = 0;
    int enable = (st->mode != DSQ_OFF);
    u8 vdf_mode = 0, num = 45u, den = 104u;
    get_target_ratio(st, &vdf_mode, &num, &den);

    /* 1. Dynamic ratio sites (mode 1 + mode 2, both DSQ_CALC and HDMI manual helper) */
    u8 m1_num = (enable && vdf_mode == 1u) ? num : 45u;
    u8 m1_den = (enable && vdf_mode == 1u) ? den : 104u;
    u8 m2_num = (enable && vdf_mode == 2u) ? num : 3u;
    u8 m2_den = (enable && vdf_mode == 2u) ? den : 8u;

    if (!patch_ratio_6b(api, "M1_MAIN",  CP_DSQ_M1_MAIN_VA,  m1_num, m1_den, &app, &sk) ||
        !patch_ratio_4b(api, "M1_HDMI1", CP_DSQ_M1_HDMI1_VA, m1_num, m1_den, &app, &sk) ||
        !patch_ratio_4b(api, "M1_HDMI2", CP_DSQ_M1_HDMI2_VA, m1_num, m1_den, &app, &sk) ||
        !patch_ratio_6b(api, "M2_MAIN",  CP_DSQ_M2_MAIN_VA,  m2_num, m2_den, &app, &sk) ||
        !patch_ratio_4b(api, "M2_HDMI",  CP_DSQ_M2_HDMI_VA,  m2_num, m2_den, &app, &sk)) {
        if (applied_out) *applied_out = app;
        if (skipped_out) *skipped_out = sk;
        return 0;
    }

    /* 2. Fixed Photo-protection sites (DSQ_DISPATCH_GUARD + DSQ_PHOTO_FALLBACK) */
    for (size_t k = 0; k < sizeof(kFixedPatches) / sizeof(kFixedPatches[0]); ++k) {
        size_t i = enable ? k : ((sizeof(kFixedPatches) / sizeof(kFixedPatches[0])) - 1u - k);
        const struct cp_fixed_patch *p = &kFixedPatches[i];
        const u8 *exp = enable ? p->stock : p->patch;
        const u8 *rep = enable ? p->patch : p->stock;
        PilotStatus rc = pm_cp_patch_idempotent(api, p->va, exp, rep, p->len, &app, &sk);
        if (rc != PILOT_STATUS_OK) {
            u8 cur[4] = {0};
            (void)api->cp_mem->read(p->va, cur, p->len < 4u ? p->len : 4u);
            char msg[144];
            snprintf(msg, sizeof(msg),
                     "DSQ_CODE_ERR site=%s va=0x%08x en=%d rc=%d cur=%02x%02x%02x%02x",
                     p->name, p->va, enable, (int)rc, cur[0], cur[1], cur[2], cur[3]);
            pm_emit_log(api, msg);
            if (applied_out) *applied_out = app;
            if (skipped_out) *skipped_out = sk;
            return 0;
        }
    }

    /* 3. AF / Tracking rect pass (shared with pilot_video_modes.so 2K OpenGate).
     * On disable, only revert if 0x00E99E68 (AF_RECT_LCD_169) is still stock. */
    int touch_af = 1;
    if (!enable) {
        u8 rect_cur[8] = {0};
        if (api->cp_mem->read(CP_AF_RECT_LCD_169_VA, rect_cur, 8u) == PILOT_STATUS_OK &&
            memcmp(rect_cur, kAfRectLcd169Stock, 8u) != 0) {
            touch_af = 0;
            pm_emit_log(api, "DSQ_AF_KEEP reason=NON_STOCK_AF_RECT_2KOG");
        }
    }

    if (touch_af) {
        for (size_t i = 0; i < sizeof(kAfPassPatches) / sizeof(kAfPassPatches[0]); ++i) {
            const struct cp_fixed_patch *p = &kAfPassPatches[i];
            const u8 *exp = enable ? p->stock : p->patch;
            const u8 *rep = enable ? p->patch : p->stock;
            PilotStatus rc = pm_cp_patch_idempotent(api, p->va, exp, rep, p->len, &app, &sk);
            if (rc != PILOT_STATUS_OK) {
                char msg[128];
                snprintf(msg, sizeof(msg), "DSQ_AF_ERR site=%s va=0x%08x en=%d rc=%d",
                         p->name, p->va, enable, (int)rc);
                pm_emit_log(api, msg);
                if (applied_out) *applied_out = app;
                if (skipped_out) *skipped_out = sk;
                return 0;
            }
        }
    }

    if (applied_out) *applied_out = app;
    if (skipped_out) *skipped_out = sk;
    return 1;
}

/* --- Apply full desired state to CP --- */
static int apply_desqueeze_state(const PilotApi *api, const struct dsq_state *st, const char *tag)
{
    int enable = (st->mode != DSQ_OFF);
    u8 vdf_mode = 0, num = 45u, den = 104u;
    get_target_ratio(st, &vdf_mode, &num, &den);

    u8 lcd_b  = (enable && st->lcd)  ? 1u : 0u;
    u8 evf_b  = (enable && st->evf)  ? 1u : 0u;
    u8 hdmi_b = (enable && st->hdmi) ? 1u : 0u;

    u32 w = 0, sk = 0;
    u32 resp_hdr = 0, resp_st = 0;
    int ipc_rc = 0;
    const char *path = "ipc";

    if (enable) {
        /* 1. Patch CP ratio/guard/fallback/AF code FIRST */
        if (!apply_code_patches(api, st, &w, &sk))
            return 0;

        /* 2. Send native VdfInputCmdSetDeSqueeze IPC (always sets +0xFA dirty) */
        ipc_rc = send_vdf_desqueeze_ipc(api, vdf_mode, lcd_b, evf_b, hdmi_b, &resp_hdr, &resp_st);
        if (ipc_rc != 0) {
            path = "dma_fallback";
            if (send_vdf_desqueeze_dma_fallback(api, vdf_mode, lcd_b, evf_b, hdmi_b) != 0)
                return 0;
        }
        sync_disp_shadow(api, vdf_mode, evf_b, lcd_b, hdmi_b);
    } else {
        /* 1. Turn OFF VDF De-Squeeze FIRST */
        ipc_rc = send_vdf_desqueeze_ipc(api, 0u, 0u, 0u, 0u, &resp_hdr, &resp_st);
        if (ipc_rc != 0) {
            path = "dma_fallback";
            (void)send_vdf_desqueeze_dma_fallback(api, 0u, 0u, 0u, 0u);
        }
        sync_disp_shadow(api, 0u, 0u, 0u, 0u);
        pm_sleep_ms(50u);

        /* 2. Revert CP code patches cleanly back to stock */
        if (!apply_code_patches(api, st, &w, &sk))
            return 0;
    }

    pm_sleep_ms(60u);
    u8 rb_lcd[2] = {0xFF, 0xFF}, rb_evf[2] = {0xFF, 0xFF}, rb_hdmi[2] = {0xFF, 0xFF};
    u8 d_lcd = 0xFF, d_evf = 0xFF, d_hdmi = 0xFF;
    (void)api->cp_mem->read(CP_VDF_DEV_LCD_BASE  + CP_VDF_DEV_DSQ_OFF, rb_lcd,  2u);
    (void)api->cp_mem->read(CP_VDF_DEV_EVF_BASE  + CP_VDF_DEV_DSQ_OFF, rb_evf,  2u);
    (void)api->cp_mem->read(CP_VDF_DEV_HDMI_BASE + CP_VDF_DEV_DSQ_OFF, rb_hdmi, 2u);
    (void)api->cp_mem->read(CP_VDF_DEV_LCD_BASE  + CP_VDF_DEV_DIRTY_OFF, &d_lcd,  1u);
    (void)api->cp_mem->read(CP_VDF_DEV_EVF_BASE  + CP_VDF_DEV_DIRTY_OFF, &d_evf,  1u);
    (void)api->cp_mem->read(CP_VDF_DEV_HDMI_BASE + CP_VDF_DEV_DIRTY_OFF, &d_hdmi, 1u);

    int nudged = 0;
    int need_nudge = enable
        ? ((lcd_b && d_lcd == 1u) || (evf_b && d_evf == 1u) || (hdmi_b && d_hdmi == 1u))
        : (d_lcd == 1u || d_evf == 1u);
    if (ipc_rc == 0 && need_nudge) {
        trigger_disp_update_32f(api);
        nudged = 1;
    }

    char msg[240];
    snprintf(msg, sizeof(msg),
             "DSQ_APPLY tag=%s mode=%s(%s) vdf=%u(%u/%u) out=%u/%u/%u path=%s "
             "w=%u sk=%u ack=0x%08x/%u rb=%u:%u,%u:%u,%u:%u dirty=%u/%u/%u nudge=%d",
             tag ? tag : "?", kModeShort[st->mode], kSrcShort[st->src],
             (unsigned)vdf_mode, (unsigned)num, (unsigned)den,
             (unsigned)lcd_b, (unsigned)evf_b, (unsigned)hdmi_b,
             path, w, sk, resp_hdr, resp_st,
             (unsigned)rb_lcd[0], (unsigned)rb_lcd[1],
             (unsigned)rb_evf[0], (unsigned)rb_evf[1],
             (unsigned)rb_hdmi[0], (unsigned)rb_hdmi[1],
             (unsigned)d_lcd, (unsigned)d_evf, (unsigned)d_hdmi, nudged);
    pm_emit_log(api, msg);
    return 1;
}

/* --- Watchdog check: detects CP sleep/wake reset or shared AF_RECT_PASS revert --- */
static int check_cp_needs_reapply(const PilotApi *api, const struct dsq_state *st)
{
    if (!api || !api->cp_mem || !api->cp_mem->read || st->mode == DSQ_OFF)
        return 0;

    u8 vdf_mode = 0, num = 45u, den = 104u;
    get_target_ratio(st, &vdf_mode, &num, &den);

    /* 1. Check dispatch guard (always non-stock while active, even for stock 1.3x / 2.0x) */
    u8 cur_guard[4] = {0};
    if (api->cp_mem->read(kFixedPatches[0].va, cur_guard, 4u) == PILOT_STATUS_OK &&
        memcmp(cur_guard, kFixedPatches[0].stock, 4u) == 0) {
        return 1;
    }

    /* 2. Check active ratio bytes match desired (num, den) */
    u32 r_va = (vdf_mode == 2u) ? CP_DSQ_M2_MAIN_VA : CP_DSQ_M1_MAIN_VA;
    u8 cur_r[6] = {0};
    if (api->cp_mem->read(r_va, cur_r, 6u) == PILOT_STATUS_OK &&
        (cur_r[2] != num || cur_r[4] != den)) {
        return 1;
    }

    /* 3. AF/TRK pass reverted to 00 20 (e.g. pilot_video_modes exited 2K OG) */
    for (size_t i = 0; i < 2u; ++i) {
        u8 cur_af[2] = {0};
        if (api->cp_mem->read(kAfPassPatches[i].va, cur_af, 2u) == PILOT_STATUS_OK &&
            memcmp(cur_af, kAfPassPatches[i].stock, 2u) == 0) {
            return 1;
        }
    }

    /* 4. VDF device state reset on any enabled output */
    if (st->lcd) {
        u8 cur_lcd[2] = {0};
        if (api->cp_mem->read(CP_VDF_DEV_LCD_BASE + CP_VDF_DEV_DSQ_OFF, cur_lcd, 2u) == PILOT_STATUS_OK &&
            (cur_lcd[0] != vdf_mode || cur_lcd[1] != 1u)) {
            return 1;
        }
    }
    if (st->evf) {
        u8 cur_evf[2] = {0};
        if (api->cp_mem->read(CP_VDF_DEV_EVF_BASE + CP_VDF_DEV_DSQ_OFF, cur_evf, 2u) == PILOT_STATUS_OK &&
            (cur_evf[0] != vdf_mode || cur_evf[1] != 1u)) {
            return 1;
        }
    }
    if (st->hdmi) {
        u8 cur_hdmi[2] = {0};
        if (api->cp_mem->read(CP_VDF_DEV_HDMI_BASE + CP_VDF_DEV_DSQ_OFF, cur_hdmi, 2u) == PILOT_STATUS_OK &&
            (cur_hdmi[0] != vdf_mode || cur_hdmi[1] != 1u)) {
            return 1;
        }
    }

    return 0;
}

/* --- Menu callbacks --- */
static void on_opt_changed(PilotUiOption *opt, void *user)
{
    int which = (int)(intptr_t)user;
    if (!opt)
        return;

    pthread_mutex_lock(&g_state_lock);
    if (which == 0) {
        int m = opt->value;
        g_desired.mode = (m >= 0 && m < DSQ_COUNT) ? m : DSQ_OFF;
    } else if (which == 1) {
        int s = opt->value;
        g_desired.src = (s >= 0 && s < SRC_COUNT) ? s : SRC_16_9;
    } else if (which == 2) {
        g_desired.lcd = opt->value ? 1 : 0;
    } else if (which == 3) {
        g_desired.evf = opt->value ? 1 : 0;
    } else if (which == 4) {
        g_desired.hdmi = opt->value ? 1 : 0;
    }
    struct dsq_state snap = g_desired;
    pthread_cond_signal(&g_state_cond);
    pthread_mutex_unlock(&g_state_lock);

    save_config(g_api, &snap);

    char msg[128];
    snprintf(msg, sizeof(msg), "DSQ_MENU_SET mode=%d(%s) src=%d(%s) lcd=%d evf=%d hdmi=%d",
             snap.mode, kModeShort[snap.mode], snap.src, kSrcShort[snap.src],
             snap.lcd, snap.evf, snap.hdmi);
    pm_emit_log(g_api, msg);
}

static PilotUiOption g_dsq_options[] = {
    {
        .label        = "反挤压倍率",
        .kind         = PILOT_UI_OPT_ENUM,
        .value        = DSQ_OFF,
        .choices      = kModeNames,
        .choice_count = DSQ_COUNT,
        .on_change    = on_opt_changed,
        .user         = (void *)(intptr_t)0,
    },
    {
        .label        = "画幅基准",
        .kind         = PILOT_UI_OPT_ENUM,
        .value        = SRC_16_9,
        .choices      = kSrcNames,
        .choice_count = SRC_COUNT,
        .on_change    = on_opt_changed,
        .user         = (void *)(intptr_t)1,
    },
    {
        .label     = "屏幕 (LCD)",
        .kind      = PILOT_UI_OPT_BOOL,
        .value     = 1,
        .on_change = on_opt_changed,
        .user      = (void *)(intptr_t)2,
    },
    {
        .label     = "取景器 (EVF)",
        .kind      = PILOT_UI_OPT_BOOL,
        .value     = 1,
        .on_change = on_opt_changed,
        .user      = (void *)(intptr_t)3,
    },
    {
        .label     = "外接 (HDMI)",
        .kind      = PILOT_UI_OPT_BOOL,
        .value     = 0,
        .on_change = on_opt_changed,
        .user      = (void *)(intptr_t)4,
    },
};

/* --- Single Background Worker --- */
static void *dsq_worker(void *unused)
{
    (void)unused;
    const PilotApi *api = g_api;
    char msg[160];

    pm_emit_log(api, "DSQ_START ver=2 modes=1.33/1.3/1.5/1.6/1.8/2.0c/2.0f src=16:9|3:2OG");

    if (!wait_cp_vdf_ready(api, 150u, 100u)) {
        pm_emit_log(api, "DSQ_CP_NOT_READY");
        return NULL;
    }

    u8 vt[4] = {0}, l0[2] = {0xFF, 0xFF}, e0[2] = {0xFF, 0xFF}, h0[2] = {0xFF, 0xFF};
    (void)api->cp_mem->read(CP_VDF_DEV_LCD_BASE, vt, 4u);
    (void)api->cp_mem->read(CP_VDF_DEV_LCD_BASE  + CP_VDF_DEV_DSQ_OFF, l0, 2u);
    (void)api->cp_mem->read(CP_VDF_DEV_EVF_BASE  + CP_VDF_DEV_DSQ_OFF, e0, 2u);
    (void)api->cp_mem->read(CP_VDF_DEV_HDMI_BASE + CP_VDF_DEV_DSQ_OFF, h0, 2u);
    snprintf(msg, sizeof(msg),
             "DSQ_BOOT_PROBE lcd_vtbl=0x%08x init_dsq=%u:%u,%u:%u,%u:%u",
             pm_rd_u32(vt, 0u),
             (unsigned)l0[0], (unsigned)l0[1],
             (unsigned)e0[0], (unsigned)e0[1],
             (unsigned)h0[0], (unsigned)h0[1]);
    pm_emit_log(api, msg);

    pthread_mutex_lock(&g_state_lock);
    g_cp_ready = 1;
    struct dsq_state init_st = g_desired;
    pthread_mutex_unlock(&g_state_lock);

    if (init_st.mode != DSQ_OFF) {
        if (apply_desqueeze_state(api, &init_st, "boot")) {
            pthread_mutex_lock(&g_state_lock);
            g_applied = init_st;
            pthread_mutex_unlock(&g_state_lock);
        }
    } else {
        pthread_mutex_lock(&g_state_lock);
        g_applied = init_st;
        pthread_mutex_unlock(&g_state_lock);
    }

    for (;;) {
        pthread_mutex_lock(&g_state_lock);
        int do_reapply = 0;
        while (state_equal(&g_desired, &g_applied)) {
            if (g_applied.mode == DSQ_OFF) {
                pthread_cond_wait(&g_state_cond, &g_state_lock);
            } else {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 250000000L; /* 250ms wake/conflict watchdog */
                if (ts.tv_nsec >= 1000000000L) {
                    ts.tv_sec += 1;
                    ts.tv_nsec -= 1000000000L;
                }
                (void)pthread_cond_timedwait(&g_state_cond, &g_state_lock, &ts);
                if (state_equal(&g_desired, &g_applied) && g_applied.mode != DSQ_OFF) {
                    struct dsq_state active = g_applied;
                    pthread_mutex_unlock(&g_state_lock);
                    if (check_cp_needs_reapply(api, &active)) {
                        pm_sleep_ms(50u);
                        if (check_cp_needs_reapply(api, &active)) {
                            do_reapply = 1;
                            pthread_mutex_lock(&g_state_lock);
                            break;
                        }
                    }
                    pthread_mutex_lock(&g_state_lock);
                }
            }
        }
        struct dsq_state prev   = g_applied;
        struct dsq_state target = g_desired;
        pthread_mutex_unlock(&g_state_lock);

        if (do_reapply && target.mode != DSQ_OFF) {
            (void)wait_cp_vdf_ready(api, 50u, 50u);
            int ok = apply_desqueeze_state(api, &target, "watchdog");
            snprintf(msg, sizeof(msg), "DSQ_WATCHDOG_REAPPLY ok=%d", ok);
            pm_emit_log(api, msg);
            continue;
        }

        /* Toggling src/LCD/EVF/HDMI while De-Squeeze is Off only updates config */
        if (prev.mode == DSQ_OFF && target.mode == DSQ_OFF) {
            pthread_mutex_lock(&g_state_lock);
            g_applied = target;
            pthread_mutex_unlock(&g_state_lock);
            continue;
        }

        int ok = apply_desqueeze_state(api, &target, "menu");
        pthread_mutex_lock(&g_state_lock);
        g_applied = target;
        pthread_mutex_unlock(&g_state_lock);

        const DesqueezeStrings *ui = dsq_ui_str();
        char toast[80];
        if (ok) {
            if (target.mode != DSQ_OFF)
                snprintf(toast, sizeof(toast), ui->toast_on_fmt,
                         kModeShort[target.mode], kSrcShort[target.src]);
            else
                snprintf(toast, sizeof(toast), "%s", ui->toast_off);
            show_toast(toast, 2500u);
        } else {
            show_toast(ui->toast_err, 3000u);
        }
    }
    return NULL;
}

__attribute__((visibility("default")))
PilotStatus pilot_module_init(const PilotApi *api)
{
    if (!api || api->abi_version < PILOT_ABI_VERSION ||
        api->struct_size < sizeof(PilotApi))
        return PILOT_STATUS_INVALID_ARGUMENT;
    if (!api->cp_mem || !api->cp_mem->read || !api->cp_mem->write || !api->cp_mem->patch)
        return PILOT_STATUS_UNSUPPORTED;

    pthread_mutex_lock(&g_init_lock);
    if (g_started) {
        pthread_mutex_unlock(&g_init_lock);
        return PILOT_STATUS_OK;
    }
    g_api = api;
    g_started = 1;

    const DesqueezeStrings *ui = dsq_ui_str();
    load_config(api, &g_desired);
    g_dsq_options[0].label   = ui->opt_mode;
    g_dsq_options[0].choices = ui->mode_names;
    g_dsq_options[0].value   = g_desired.mode;
    g_dsq_options[1].label   = ui->opt_src;
    g_dsq_options[1].choices = ui->src_names;
    g_dsq_options[1].value   = g_desired.src;
    g_dsq_options[2].label   = ui->opt_lcd;
    g_dsq_options[2].value   = g_desired.lcd;
    g_dsq_options[3].label   = ui->opt_evf;
    g_dsq_options[3].value   = g_desired.evf;
    g_dsq_options[4].label   = ui->opt_hdmi;
    g_dsq_options[4].value   = g_desired.hdmi;
    save_config(api, &g_desired);

    PilotStatus ust = pilot_ui_register_options(
        MODULE_SO_NAME,
        g_dsq_options,
        sizeof(g_dsq_options) / sizeof(g_dsq_options[0]));
    char msg[128];
    if (ust != PILOT_STATUS_OK) {
        snprintf(msg, sizeof(msg), "DSQ_MENU_REG_FAIL status=%d", (int)ust);
    } else {
        snprintf(msg, sizeof(msg), "DSQ_MENU_READY mode=%d(%s) src=%d lcd=%d evf=%d hdmi=%d",
                 g_desired.mode, kModeShort[g_desired.mode], g_desired.src,
                 g_desired.lcd, g_desired.evf, g_desired.hdmi);
    }
    pm_emit_log(api, msg);
    pthread_mutex_unlock(&g_init_lock);

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&tid, &attr, dsq_worker, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0)
        return PILOT_STATUS_SYSTEM;
    return PILOT_STATUS_OK;
}
