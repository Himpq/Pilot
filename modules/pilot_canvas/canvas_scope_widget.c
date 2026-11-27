/* Live View Pro Scope Widget (built on top of the reusable PilotPanel container):
 *   - Sleek 268x136 dark-glass monitor card with bold scale=2 header, IRE
 *     scale (100 / 50 / 0), and unified smooth filled-gradient curve style:
 *       PARADE : X = frame L->R, 3 side-by-side R | G | B filled-gradient waves
 *       LUMA   : X = frame L->R, full-width White filled-gradient Luma wave
 *                (average curve + faint highlight peak envelope)
 *       RGBW   : X = frame L->R, full-width R / G / B overlaid filled waves
 *       HIST   : X = 0..100 IRE, merged 3-color R / G / B frequency histogram
 *       OFF    : Hidden (via Pilot Menu)
 *   - Calibrated Rec.709 (BT.709) & S-Log3 OETF curves + RGGB OPD + OB floor
 *   - Controlled via tap-to-cycle, drag-to-move, or Pilot Menu options */

#include "pilot_canvas_internal.h"
#include "pilot_ui.h"
#include "localization.h"

#define SCOPE_PANEL_W 336
#define SCOPE_PANEL_H 170
#define SCOPE_DEF_X   364
#define SCOPE_DEF_Y   276

static const int k_scope_w[3] = { 196, 268, 336 };
static const int k_scope_h[3] = { 100, 136, 170 };
static const char *const k_size_choices[3] = { "小", "中", "大" };

typedef enum {
    SCOPE_MODE_RGB_PARADE = 0,
    SCOPE_MODE_WAVEFORM   = 1,
    SCOPE_MODE_RGB_WAVE   = 2,
    SCOPE_MODE_RGB_CURVE  = 3,
    SCOPE_MODE_OFF        = 4,
    SCOPE_MODE_COUNT      = 5
} PilotScopeMode;

static const char *const k_mode_titles[SCOPE_MODE_COUNT] = {
    "PARADE", "LUMA", "RGBW", "HIST", "OFF"
};

static const char *const k_mode_choices[SCOPE_MODE_COUNT] = {
    "RGB 分量", "亮度波形", "RGB 波形", "RGB 直方图", "关闭"
};

static const char *const k_gamma_choices[SCOPE_GAMMA_COUNT] = {
    "Rec.709", "S-Log3"
};

static const char *const k_wb_choices[2] = {
    "关", "自动"
};

enum {
    FC_MODE_OFF  = 0,
    FC_MODE_KEY  = 1,
    FC_MODE_ARRI = 2,
    FC_MODE_COUNT = 3
};

static const char *const k_fc_choices[FC_MODE_COUNT] = {
    "关闭", "关键曝光区", "ARRI 全彩"
};

static const char *const k_disp_choices[2] = {
    "常驻显示", "独立 DISP 页"
};

static const char *const k_lock_choices[2] = {
    "关", "锁定"
};

static const char *const k_focal_choices[FOCAL_MODE_COUNT] = {
    "关闭", "变焦弹出 2秒", "始终显示"
};

static PilotPanel *g_fc_panel;
static PilotPanel *g_scope_panel;
static pthread_mutex_t g_scope_lock = PTHREAD_MUTEX_INITIALIZER;
static PilotOpdFrame g_scope_frame;
static int g_scope_mode   = SCOPE_MODE_RGB_WAVE;
static int g_scope_size   = 1; /* 0=Small(196x100), 1=Medium(268x136), 2=Large(336x170) */
static int g_scope_gamma  = SCOPE_GAMMA_REC709;
static int g_scope_wb    = 1;
static int g_fc_mode     = FC_MODE_OFF;
static int g_disp_sep    = 1; /* 1 = independent DISP page, 0 = Always On in LV */
static int g_scope_locked = 0; /* 1 = Scope fixed position (no dragging) */
static char g_scope_tail[20];
static char g_scope_meta[32] = "709";

static void scope_rebuild_meta_locked(void)
{
    const char *gtag = (g_scope_gamma == SCOPE_GAMMA_SLOG3) ? "SL3" : "709";
    if (g_scope_tail[0])
        snprintf(g_scope_meta, sizeof(g_scope_meta), "%s %s", gtag, g_scope_tail);
    else
        snprintf(g_scope_meta, sizeof(g_scope_meta), "%s", gtag);
}

/* Draw sleek dark card header (scale=2 bold text), horizontal divider,
 * Y-axis 100/50/0 IRE labels (scale=2), and fine graticule lines. */
static void scope_draw_chrome(PilotCanvas *c, const char *title, const char *meta,
                              int card_w, int card_h, int txt_scale,
                              int pad_l, int pad_t, int uw, int uh,
                              int v_divs, int show_ire_labels)
{
    pilot_canvas_fill_rect_v1(c, 0, 0, card_w, card_h,
                              0xB806080Au, PILOT_CANVAS_BLEND_NONE);
    pilot_canvas_fill_rect_v1(c, 0, 0, card_w, 1, 0x70505860u, PILOT_CANVAS_BLEND_NONE);
    pilot_canvas_fill_rect_v1(c, 0, card_h - 1, card_w, 1, 0x70505860u, PILOT_CANVAS_BLEND_NONE);
    pilot_canvas_fill_rect_v1(c, 0, 0, 1, card_h, 0x70505860u, PILOT_CANVAS_BLEND_NONE);
    pilot_canvas_fill_rect_v1(c, card_w - 1, 0, 1, card_h, 0x70505860u, PILOT_CANVAS_BLEND_NONE);

    int hdr_x = (txt_scale == 1) ? 6 : 8;
    int hdr_y = 4;
    int div_y = (txt_scale == 1) ? 14 : 22;
    int adv   = 6 * txt_scale;
    pilot_canvas_draw_osd_text_v1(c, hdr_x, hdr_y, txt_scale, 0xFFFFFFFFu, 0x00000000u, title);
    int meta_len = (int)strlen(meta);
    int meta_x = card_w - hdr_x - meta_len * adv;
    int min_meta_x = hdr_x + (int)strlen(title) * adv + 6;
    if (meta_x < min_meta_x) meta_x = min_meta_x;
    pilot_canvas_draw_osd_text_v1(c, meta_x, hdr_y, txt_scale, 0xD8C0D0E0u, 0x00000000u, meta);
    pilot_canvas_fill_rect_v1(c, hdr_x, div_y, card_w - hdr_x * 2, 1, 0x78606870u, PILOT_CANVAS_BLEND_NONE);

    /* 5 horizontal grid lines (100%, 75%, 50%, 25%, 0%) */
    for (int i = 0; i <= 4; ++i) {
        int y = pad_t + (uh * i) / 4;
        u32 clr = (i == 2) ? 0x90808890u : 0x58485058u;
        pilot_canvas_fill_rect_v1(c, pad_l, y, uw, 1, clr, PILOT_CANVAS_BLEND_NONE);
    }

    /* Vertical grid lines */
    pilot_canvas_fill_rect_v1(c, pad_l, pad_t, 1, uh + 1, 0x78606870u, PILOT_CANVAS_BLEND_NONE);
    pilot_canvas_fill_rect_v1(c, pad_l + uw - 1, pad_t, 1, uh + 1, 0x78606870u, PILOT_CANVAS_BLEND_NONE);
    if (v_divs >= 2) {
        for (int g = 1; g < v_divs; ++g) {
            int gx = pad_l + (uw * g) / v_divs;
            pilot_canvas_fill_rect_v1(c, gx, pad_t, 1, uh, 0x80687078u, PILOT_CANVAS_BLEND_NONE);
        }
    } else {
        for (int g = 1; g < 6; ++g) {
            int gx = pad_l + (uw * g) / 6;
            pilot_canvas_fill_rect_v1(c, gx, pad_t, 1, uh, 0x40485058u, PILOT_CANVAS_BLEND_NONE);
        }
    }

    if (show_ire_labels) {
        if (txt_scale == 1) {
            pilot_canvas_draw_osd_text_v1(c, 3,  pad_t - 2,          1, 0xD8C0C8D0u, 0x00000000u, "100");
            pilot_canvas_draw_osd_text_v1(c, 8,  pad_t + uh / 2 - 3, 1, 0xD8C0C8D0u, 0x00000000u, "50");
            pilot_canvas_draw_osd_text_v1(c, 14, pad_t + uh - 6,     1, 0xD8C0C8D0u, 0x00000000u, "0");
        } else {
            pilot_canvas_draw_osd_text_v1(c, 4,  pad_t - 4,          2, 0xD8C0C8D0u, 0x00000000u, "100");
            pilot_canvas_draw_osd_text_v1(c, 14, pad_t + uh / 2 - 7, 2, 0xD8C0C8D0u, 0x00000000u, "50");
            pilot_canvas_draw_osd_text_v1(c, 26, pad_t + uh - 12,    2, 0xD8C0C8D0u, 0x00000000u, "0");
        }
    }
}

/* Render a smooth filled-gradient area curve with bright 2px top neon stroke.
 * Supports up to 48 input samples (32-bin histograms or 44-col spatial profiles). */
static void scope_draw_filled_curve(PilotCanvas *c, const u32 *vals, int nvals,
                                    u32 maxv, int ncols,
                                    u32 fill_top, u32 fill_bot, u32 stroke,
                                    int pad_l, int pad_t, int uw, int uh)
{
    if (nvals < 2 || nvals > 48 || ncols < 2)
        return;
    if (maxv == 0u) maxv = 1u;

    /* 2-pass binomial smoothing for sleek organic wave shape */
    u32 sm[48], tmp[48];
    for (int i = 0; i < nvals; ++i) {
        u32 l = (i > 0) ? vals[i - 1] : vals[i];
        u32 m = vals[i];
        u32 r = (i + 1 < nvals) ? vals[i + 1] : vals[i];
        tmp[i] = (l + 2u * m + r + 2u) >> 2;
    }
    for (int i = 0; i < nvals; ++i) {
        u32 l = (i > 0) ? tmp[i - 1] : tmp[i];
        u32 m = tmp[i];
        u32 r = (i + 1 < nvals) ? tmp[i + 1] : tmp[i];
        sm[i] = (l + 2u * m + r + 2u) >> 2;
    }

    int prev_y = -1;
    for (int s = 0; s < ncols; ++s) {
        int pos_q8 = (s * (nvals - 1) * 256) / (ncols - 1);
        int idx = pos_q8 >> 8;
        int frac = pos_q8 & 0xFF;
        u32 v0 = sm[idx];
        u32 v1 = (idx + 1 < nvals) ? sm[idx + 1] : v0;
        u32 val = v0 + (u32)(((int32_t)v1 - (int32_t)v0) * frac / 256);

        int h = (int)((u64)val * (u64)uh / (u64)maxv);
        if (h > uh) h = uh;
        int x0 = pad_l + (s * uw) / ncols;
        int x1 = pad_l + ((s + 1) * uw) / ncols;
        int w = (x1 > x0) ? (x1 - x0) : 1;
        int y = pad_t + uh - h;

        /* 2-band translucent vertical gradient fill from curve down to 0 */
        if (fill_top || fill_bot) {
            if (h >= 4) {
                int h_top = h / 2;
                int h_bot = h - h_top;
                pilot_canvas_fill_rect_v1(c, x0, y, w, h_top, fill_top, PILOT_CANVAS_BLEND_NONE);
                pilot_canvas_fill_rect_v1(c, x0, y + h_top, w, h_bot, fill_bot, PILOT_CANVAS_BLEND_NONE);
            } else if (h > 0) {
                pilot_canvas_fill_rect_v1(c, x0, y, w, h, fill_bot, PILOT_CANVAS_BLEND_NONE);
            }
        }

        /* Crisp 2px bright neon top contour */
        if (stroke) {
            int sy = y;
            int sh = 2;
            if (prev_y >= 0) {
                int dy = y - prev_y;
                if (dy > 2) { sy = prev_y; sh = dy + 2; }
                else if (dy < -2) { sy = y; sh = -dy + 2; }
            }
            if (sy < pad_t) sy = pad_t;
            if (sy + sh > pad_t + uh) sh = pad_t + uh - sy;
            if (sh > 0)
                pilot_canvas_fill_rect_v1(c, x0, sy, w, sh, stroke, PILOT_CANVAS_BLEND_NONE);
        }
        prev_y = y;
    }
}

/* Compute per-column top-4 average across active vertical rows. */
static void compute_col_peak_profile(const u8 src[OPD_MAX_ROWS][OPD_MAX_COLS],
                                     u32 rows, u32 cols, u32 out[OPD_MAX_COLS])
{
    for (u32 col = 0; col < cols; ++col) {
        u32 t0 = 0u, t1 = 0u, t2 = 0u, t3 = 0u;
        for (u32 row = 0; row < rows; ++row) {
            u32 v = src[row][col];
            if (v >= t0) {
                t3 = t2; t2 = t1; t1 = t0; t0 = v;
            } else if (v >= t1) {
                t3 = t2; t2 = t1; t1 = v;
            } else if (v >= t2) {
                t3 = t2; t2 = v;
            } else if (v > t3) {
                t3 = v;
            }
        }
        out[col] = (t0 + t1 + t2 + t3 + 2u) >> 2;
    }
}

static void scope_on_render(PilotPanel *panel, PilotCanvas *c)
{
    if (!c)
        return;

    PilotOpdFrame frame;
    int mode, sz;
    char meta[32];
    pthread_mutex_lock(&g_scope_lock);
    frame = g_scope_frame;
    mode  = g_scope_mode;
    sz    = g_scope_size;
    snprintf(meta, sizeof(meta), "%s", g_scope_meta);
    pthread_mutex_unlock(&g_scope_lock);

    pilot_canvas_begin_batch_v1(c);

    if (mode == SCOPE_MODE_OFF) {
        pilot_canvas_end_batch_v1(c);
        return;
    }

    if (sz < 0) sz = 0;
    if (sz > 2) sz = 2;
    int card_w = k_scope_w[sz];
    int card_h = k_scope_h[sz];
    int txt_scale = (sz == 0) ? 1 : 2;
    if (panel)
        canvas_panel_set_hit_size(panel, card_w, card_h);

    int show_ire = (mode != SCOPE_MODE_RGB_CURVE);
    int pad_l = show_ire ? ((txt_scale == 1) ? 24 : 44) : ((txt_scale == 1) ? 6 : 10);
    int pad_t = (txt_scale == 1) ? 18 : 28;
    int pad_r = (txt_scale == 1) ? 6 : 8;
    int pad_b = show_ire ? ((txt_scale == 1) ? 6 : 8) : ((txt_scale == 1) ? 13 : 20);
    int uw = card_w - pad_l - pad_r;
    int uh = card_h - pad_t - pad_b;

    const char *title = k_mode_titles[mode % SCOPE_MODE_COUNT];
    int v_divs = (mode == SCOPE_MODE_RGB_PARADE) ? 3 : 0;
    scope_draw_chrome(c, title, meta, card_w, card_h, txt_scale,
                      pad_l, pad_t, uw, uh, v_divs, show_ire);

    if (!frame.valid) {
        pilot_canvas_end_batch_v1(c);
        return;
    }

    u32 fcols = (frame.cols >= 2u && frame.cols <= OPD_MAX_COLS) ? frame.cols : 44u;
    u32 frows = (frame.rows >= 2u && frame.rows <= OPD_MAX_ROWS) ? frame.rows : 26u;

    if (mode == SCOPE_MODE_RGB_PARADE) {
        /* 3 side-by-side filled-gradient curves: Red | Green | Blue (X = L->R, Y = IRE) */
        u32 pr[OPD_MAX_COLS], pg[OPD_MAX_COLS], pb[OPD_MAX_COLS];
        compute_col_peak_profile(frame.r, frows, fcols, pr);
        compute_col_peak_profile(frame.g, frows, fcols, pg);
        compute_col_peak_profile(frame.b, frows, fcols, pb);

        const u32 *ch_vals[3] = { pr, pg, pb };
        const u32 f_top[3] = { 0x70E02838u, 0x7028D858u, 0x702078F0u };
        const u32 f_bot[3] = { 0x24781018u, 0x24107028u, 0x24103888u };
        const u32 strk[3]  = { 0xFFFF4050u, 0xFF38FF68u, 0xFF38A0FFu };
        for (int ch = 0; ch < 3; ++ch) {
            int sx0 = pad_l + (uw * ch) / 3 + 2;
            int sx1 = pad_l + (uw * (ch + 1)) / 3 - 2;
            int sw = sx1 - sx0;
            if (sw < 12) sw = 12;
            scope_draw_filled_curve(c, ch_vals[ch], (int)fcols, 255u, 22,
                                    f_top[ch], f_bot[ch], strk[ch],
                                    sx0, pad_t, sw, uh);
        }
    } else if (mode == SCOPE_MODE_WAVEFORM) {
        /* Single clean full-width White filled-gradient Luma Wave (X = L->R, Y = IRE) */
        u32 py[OPD_MAX_COLS];
        compute_col_peak_profile(frame.y, frows, fcols, py);
        scope_draw_filled_curve(c, py, (int)fcols, 255u, 56,
                                0x78E0E8F0u, 0x28788088u, 0xFFFFFFFFu,
                                pad_l, pad_t, uw, uh);
    } else if (mode == SCOPE_MODE_RGB_WAVE) {
        /* Full-width R / G / B overlaid filled-gradient waves (X = L->R, Y = IRE) */
        u32 pr[OPD_MAX_COLS], pg[OPD_MAX_COLS], pb[OPD_MAX_COLS];
        compute_col_peak_profile(frame.r, frows, fcols, pr);
        compute_col_peak_profile(frame.g, frows, fcols, pg);
        compute_col_peak_profile(frame.b, frows, fcols, pb);
        scope_draw_filled_curve(c, pb, (int)fcols, 255u, 56,
                                0x482078F0u, 0x18103888u, 0xFF38A0FFu,
                                pad_l, pad_t, uw, uh);
        scope_draw_filled_curve(c, pg, (int)fcols, 255u, 56,
                                0x4428C858u, 0x18106828u, 0xFF38FF68u,
                                pad_l, pad_t, uw, uh);
        scope_draw_filled_curve(c, pr, (int)fcols, 255u, 56,
                                0x44D82838u, 0x18701018u, 0xFFFF4050u,
                                pad_l, pad_t, uw, uh);
    } else {
        /* Merged full-height 3-color RGB Histogram (X = 0..100 IRE) */
        u32 hr[32] = {0}, hg[32] = {0}, hb[32] = {0};
        for (u32 row = 0; row < frows; ++row) {
            for (u32 col = 0; col < fcols; ++col) {
                hr[frame.r[row][col] >> 3]++;
                hg[frame.g[row][col] >> 3]++;
                hb[frame.b[row][col] >> 3]++;
            }
        }
        /* Match scope_draw_filled_curve's 2-pass smoothing BEFORE finding maxv so
         * sharp clipping spikes never squash the histogram to half height. */
        u32 maxv = 1u;
        const u32 *raws[3] = { hr, hg, hb };
        for (int ch = 0; ch < 3; ++ch) {
            u32 t1[32], t2[32];
            for (int i = 0; i < 32; ++i) {
                u32 l = (i > 0) ? raws[ch][i - 1] : raws[ch][i];
                u32 m = raws[ch][i];
                u32 r = (i < 31) ? raws[ch][i + 1] : raws[ch][i];
                t1[i] = (l + 2u * m + r + 2u) >> 2;
            }
            for (int i = 0; i < 32; ++i) {
                u32 l = (i > 0) ? t1[i - 1] : t1[i];
                u32 m = t1[i];
                u32 r = (i < 31) ? t1[i + 1] : t1[i];
                t2[i] = (l + 2u * m + r + 2u) >> 2;
                if (t2[i] > maxv) maxv = t2[i];
            }
        }
        scope_draw_filled_curve(c, hb, 32, maxv, 56, 0x482078F0u, 0x18103888u, 0xFF38A0FFu,
                                pad_l, pad_t, uw, uh);
        scope_draw_filled_curve(c, hg, 32, maxv, 56, 0x4428C858u, 0x18106828u, 0xFF38FF68u,
                                pad_l, pad_t, uw, uh);
        scope_draw_filled_curve(c, hr, 32, maxv, 56, 0x44D82838u, 0x18701018u, 0xFFFF4050u,
                                pad_l, pad_t, uw, uh);

        int by = pad_t + uh + ((txt_scale == 1) ? 3 : 3);
        pilot_canvas_draw_osd_text_v1(c, pad_l, by, txt_scale, 0xC8B0B8C0u, 0x00000000u, "0");
        pilot_canvas_draw_osd_text_v1(c, pad_l + uw / 2 - (txt_scale == 1 ? 6 : 11),
                                      by, txt_scale, 0xC8B0B8C0u, 0x00000000u, "50");
        pilot_canvas_draw_osd_text_v1(c, pad_l + uw - (txt_scale == 1 ? 17 : 34),
                                      by, txt_scale, 0xC8B0B8C0u, 0x00000000u, "100");
    }

    pilot_canvas_end_batch_v1(c);
}

/* ---- False Color full-screen overlay (shares live OPD grid) ------------- */
static u32 fc_color_for_y(u8 y, int fc_mode)
{
    /* y is 0..255 (= 0..100 IRE in active Rec.709 or S-Log3 curve) */
    if (y <= 8u)   return 0x8C9020E8u; /* 0..3 IRE   : Purple (crushed black) */
    if (y <= 26u)  return 0x741868F0u; /* 3..10 IRE  : Blue (deep shadow) */
    if (y >= 242u) return 0x9CFF2020u; /* 95..100 IRE: Red (clipping) */
    if (y >= 214u) return 0x84FFD820u; /* 84..94 IRE : Yellow (highlight warn) */
    if (y >= 97u && y <= 112u)
                   return 0x7C20D840u; /* 38..44 IRE : Green (18% middle gray) */
    if (y >= 133u && y <= 148u)
                   return 0x7CFF68B0u; /* 52..58 IRE : Pink (skin tone) */

    if (fc_mode != FC_MODE_ARRI)
        return 0u; /* Key Zones: leave normal midtones 100% transparent */

    /* Full ARRI: subtle semi-transparent bands for remaining midtones */
    if (y <= 51u)  return 0x6C20A8E0u; /* 10..20 IRE : Cyan */
    if (y < 97u)   return 0x54404850u; /* 20..38 IRE : Dark Gray */
    if (y < 133u)  return 0x50788088u; /* 44..52 IRE : Mid Gray */
    return 0x50B0B8C0u;                /* 58..84 IRE : Light Gray */
}

static void fc_on_render(PilotPanel *panel, PilotCanvas *c)
{
    (void)panel;
    if (!c)
        return;

    PilotOpdFrame frame;
    int fc_mode;
    pthread_mutex_lock(&g_scope_lock);
    frame   = g_scope_frame;
    fc_mode = g_fc_mode;
    pthread_mutex_unlock(&g_scope_lock);

    pilot_canvas_begin_batch_v1(c);
    if (fc_mode == FC_MODE_OFF || !frame.valid) {
        pilot_canvas_end_batch_v1(c);
        return;
    }

    u32 fcols = (frame.cols >= 2u && frame.cols <= OPD_MAX_COLS) ? frame.cols : 44u;
    u32 frows = (frame.rows >= 2u && frame.rows <= OPD_MAX_ROWS) ? frame.rows : 26u;

    /* Align overlay to active LiveView image rect (3:2 full vs 16:9 letterboxed) */
    int ox = 0;
    int oy = (fcols >= 48u) ? 38 : 0;
    int ow = (int)c->w;
    int oh = (fcols >= 48u) ? 404 : (int)c->h;

    /* Horizontal RLE-merged semi-transparent false-color blocks */
    for (u32 r = 0u; r < frows; ++r) {
        int ry0 = oy + (int)((r * (u32)oh) / frows);
        int ry1 = oy + (int)(((r + 1u) * (u32)oh) / frows);
        int rh  = ry1 - ry0;
        if (rh < 1) rh = 1;

        u32 col_idx = 0u;
        while (col_idx < fcols) {
            u32 clr = fc_color_for_y(frame.y[r][col_idx], fc_mode);
            u32 run_end = col_idx + 1u;
            while (run_end < fcols &&
                   fc_color_for_y(frame.y[r][run_end], fc_mode) == clr)
                ++run_end;
            if (clr != 0u) {
                int rx0 = ox + (int)((col_idx * (u32)ow) / fcols);
                int rx1 = ox + (int)((run_end * (u32)ow) / fcols);
                int rw  = rx1 - rx0;
                if (rw < 1) rw = 1;
                pilot_canvas_fill_rect_v1(c, rx0, ry0, rw, rh, clr, PILOT_CANVAS_BLEND_NONE);
            }
            col_idx = run_end;
        }
    }

    /* Compact top-center IRE color legend pill (6 key zones) */
    static const u32 leg_clr[6] = {
        0xFF9020E8u, 0xFF1868F0u, 0xFF20D840u,
        0xFFFF68B0u, 0xFFFFD820u, 0xFFFF2020u
    };
    static const char *const leg_txt[6] = { "0", "10", "41", "55", "90", "99" };
    int lx = 188, ly = 6, lw = 264, lh = 20;
    pilot_canvas_fill_rect_v1(c, lx, ly, lw, lh, 0xB806080Au, PILOT_CANVAS_BLEND_NONE);
    for (int i = 0; i < 6; ++i) {
        int sx = lx + 6 + i * 43;
        pilot_canvas_fill_rect_v1(c, sx, ly + 4, 12, 12, leg_clr[i], PILOT_CANVAS_BLEND_NONE);
        pilot_canvas_draw_osd_text_v1(c, sx + 15, ly + 6, 1, 0xFFE8F0F8u, 0x00000000u, leg_txt[i]);
    }

    pilot_canvas_end_batch_v1(c);
}

/* --- Categorized Multi-Level Pilot Menu Options (Sub-Sub-Sub-Menus) --- */
static void on_scope_mode_opt(PilotUiOption *opt, void *user);
static void on_scope_size_opt(PilotUiOption *opt, void *user);
static void on_scope_gamma_opt(PilotUiOption *opt, void *user);
static void on_scope_wb_opt(PilotUiOption *opt, void *user);
static void on_scope_lock_opt(PilotUiOption *opt, void *user);
static void on_scope_fc_opt(PilotUiOption *opt, void *user);
static void on_scope_focal_mode_opt(PilotUiOption *opt, void *user);
static void on_scope_focal_lock_opt(PilotUiOption *opt, void *user);
static void on_scope_status_opt(PilotUiOption *opt, void *user);
static void on_scope_status_size_opt(PilotUiOption *opt, void *user);
static void on_scope_status_lock_opt(PilotUiOption *opt, void *user);
static void on_scope_disp_opt(PilotUiOption *opt, void *user);

static const char *const k_onoff_choices[2] = { "关闭", "开启" };

static PilotUiOption g_sub_scope_opts[] = {
    {
        .label        = "示波器模式",
        .kind         = PILOT_UI_OPT_ENUM,
        .value        = SCOPE_MODE_RGB_WAVE,
        .choices      = k_mode_choices,
        .choice_count = SCOPE_MODE_COUNT,
        .on_change    = on_scope_mode_opt,
    },
    {
        .label        = "尺寸",
        .kind         = PILOT_UI_OPT_ENUM,
        .value        = 1,
        .choices      = k_size_choices,
        .choice_count = 3,
        .on_change    = on_scope_size_opt,
    },
    {
        .label        = "伽马曲线",
        .kind         = PILOT_UI_OPT_ENUM,
        .value        = SCOPE_GAMMA_REC709,
        .choices      = k_gamma_choices,
        .choice_count = SCOPE_GAMMA_COUNT,
        .on_change    = on_scope_gamma_opt,
    },
    {
        .label        = "白平衡校准",
        .kind         = PILOT_UI_OPT_ENUM,
        .value        = 1,
        .choices      = k_wb_choices,
        .choice_count = 2,
        .on_change    = on_scope_wb_opt,
    },
    {
        .label        = "锁定位置",
        .kind         = PILOT_UI_OPT_ENUM,
        .value        = 0,
        .choices      = k_lock_choices,
        .choice_count = 2,
        .on_change    = on_scope_lock_opt,
    },
};

static PilotUiOption g_sub_fc_opts[] = {
    {
        .label        = "伪色模式",
        .kind         = PILOT_UI_OPT_ENUM,
        .value        = FC_MODE_OFF,
        .choices      = k_fc_choices,
        .choice_count = FC_MODE_COUNT,
        .on_change    = on_scope_fc_opt,
    },
};

static PilotUiOption g_sub_focal_opts[] = {
    {
        .label        = "显示模式",
        .kind         = PILOT_UI_OPT_ENUM,
        .value        = FOCAL_MODE_OFF,
        .choices      = k_focal_choices,
        .choice_count = FOCAL_MODE_COUNT,
        .on_change    = on_scope_focal_mode_opt,
    },
    {
        .label        = "锁定位置",
        .kind         = PILOT_UI_OPT_ENUM,
        .value        = 0,
        .choices      = k_lock_choices,
        .choice_count = 2,
        .on_change    = on_scope_focal_lock_opt,
    },
};

static PilotUiOption g_sub_status_opts[] = {
    {
        .label        = "显示状态条",
        .kind         = PILOT_UI_OPT_ENUM,
        .value        = 0,
        .choices      = k_onoff_choices,
        .choice_count = 2,
        .on_change    = on_scope_status_opt,
    },
    {
        .label        = "尺寸",
        .kind         = PILOT_UI_OPT_ENUM,
        .value        = 0,
        .choices      = k_size_choices,
        .choice_count = 3,
        .on_change    = on_scope_status_size_opt,
    },
    {
        .label        = "锁定位置",
        .kind         = PILOT_UI_OPT_ENUM,
        .value        = 0,
        .choices      = k_lock_choices,
        .choice_count = 2,
        .on_change    = on_scope_status_lock_opt,
    },
};

static PilotUiOption g_sub_global_opts[] = {
    {
        .label        = "显示方式",
        .kind         = PILOT_UI_OPT_ENUM,
        .value        = 1,
        .choices      = k_disp_choices,
        .choice_count = 2,
        .on_change    = on_scope_disp_opt,
    },
};

static PilotUiOption g_scope_options[] = {
    {
        .label       = "示波器",
        .kind        = PILOT_UI_OPT_SUBMENU,
        .sub_options = g_sub_scope_opts,
        .sub_count   = sizeof(g_sub_scope_opts) / sizeof(g_sub_scope_opts[0]),
    },
    {
        .label       = "伪色",
        .kind        = PILOT_UI_OPT_SUBMENU,
        .sub_options = g_sub_fc_opts,
        .sub_count   = sizeof(g_sub_fc_opts) / sizeof(g_sub_fc_opts[0]),
    },
    {
        .label       = "焦段条",
        .kind        = PILOT_UI_OPT_SUBMENU,
        .sub_options = g_sub_focal_opts,
        .sub_count   = sizeof(g_sub_focal_opts) / sizeof(g_sub_focal_opts[0]),
    },
    {
        .label       = "状态条",
        .kind        = PILOT_UI_OPT_SUBMENU,
        .sub_options = g_sub_status_opts,
        .sub_count   = sizeof(g_sub_status_opts) / sizeof(g_sub_status_opts[0]),
    },
    {
        .label       = "全局显示",
        .kind        = PILOT_UI_OPT_SUBMENU,
        .sub_options = g_sub_global_opts,
        .sub_count   = sizeof(g_sub_global_opts) / sizeof(g_sub_global_opts[0]),
    },
};

static void on_scope_mode_opt(PilotUiOption *opt, void *user)
{
    (void)user;
    if (!opt) return;
    int m = opt->value;
    if (m < 0 || m >= SCOPE_MODE_COUNT) m = SCOPE_MODE_RGB_WAVE;
    pthread_mutex_lock(&g_scope_lock);
    g_scope_mode = m;
    scope_rebuild_meta_locked();
    pthread_mutex_unlock(&g_scope_lock);
    canvas_config_mark_dirty();
    if (g_scope_panel)
        canvas_panel_invalidate(g_scope_panel, 0);
}

static void on_scope_size_opt(PilotUiOption *opt, void *user)
{
    (void)user;
    if (!opt) return;
    int sz = opt->value;
    if (sz < 0) sz = 0;
    if (sz > 2) sz = 2;
    pthread_mutex_lock(&g_scope_lock);
    g_scope_size = sz;
    pthread_mutex_unlock(&g_scope_lock);
    canvas_config_mark_dirty();
    if (g_scope_panel)
        canvas_panel_invalidate(g_scope_panel, 0);
}

static void on_scope_fc_opt(PilotUiOption *opt, void *user)
{
    (void)user;
    if (!opt) return;
    int fc = opt->value;
    if (fc < 0 || fc >= FC_MODE_COUNT) fc = FC_MODE_OFF;
    pthread_mutex_lock(&g_scope_lock);
    g_fc_mode = fc;
    pthread_mutex_unlock(&g_scope_lock);
    canvas_config_mark_dirty();
    if (g_fc_panel)
        canvas_panel_invalidate(g_fc_panel, 0);
}

static void on_scope_gamma_opt(PilotUiOption *opt, void *user)
{
    (void)user;
    if (!opt) return;
    int g = opt->value;
    if (g < 0 || g >= SCOPE_GAMMA_COUNT) g = SCOPE_GAMMA_REC709;
    pthread_mutex_lock(&g_scope_lock);
    g_scope_gamma = g;
    scope_rebuild_meta_locked();
    pthread_mutex_unlock(&g_scope_lock);
    canvas_config_mark_dirty();
    if (g_scope_panel)
        canvas_panel_invalidate(g_scope_panel, 0);
    if (g_fc_panel)
        canvas_panel_invalidate(g_fc_panel, 0);
}

static void on_scope_wb_opt(PilotUiOption *opt, void *user)
{
    (void)user;
    if (!opt) return;
    int wb = opt->value ? 1 : 0;
    pthread_mutex_lock(&g_scope_lock);
    g_scope_wb = wb;
    pthread_mutex_unlock(&g_scope_lock);
    canvas_config_mark_dirty();
}

static void on_scope_disp_opt(PilotUiOption *opt, void *user)
{
    (void)user;
    if (!opt) return;
    int sep = opt->value ? 1 : 0;
    pthread_mutex_lock(&g_scope_lock);
    g_disp_sep = sep;
    pthread_mutex_unlock(&g_scope_lock);
    canvas_set_always_on(!sep);
    canvas_config_mark_dirty();
}

static void on_scope_lock_opt(PilotUiOption *opt, void *user)
{
    (void)user;
    if (!opt) return;
    int lk = opt->value ? 1 : 0;
    pthread_mutex_lock(&g_scope_lock);
    g_scope_locked = lk;
    pthread_mutex_unlock(&g_scope_lock);
    if (g_scope_panel)
        canvas_panel_set_locked(g_scope_panel, lk);
    canvas_config_mark_dirty();
}

static void on_scope_focal_mode_opt(PilotUiOption *opt, void *user)
{
    (void)user;
    if (!opt) return;
    int m = opt->value;
    if (m < 0 || m >= FOCAL_MODE_COUNT) m = FOCAL_MODE_OFF;
    canvas_focal_widget_set_mode(m, 1);
    canvas_config_mark_dirty();
}

static void on_scope_focal_lock_opt(PilotUiOption *opt, void *user)
{
    (void)user;
    if (!opt) return;
    canvas_focal_widget_set_locked(opt->value ? 1 : 0);
    canvas_config_mark_dirty();
}

static void on_scope_status_opt(PilotUiOption *opt, void *user)
{
    (void)user;
    if (!opt) return;
    canvas_status_widget_set_enabled(opt->value ? 1 : 0, 1);
    canvas_config_mark_dirty();
}

static void on_scope_status_size_opt(PilotUiOption *opt, void *user)
{
    (void)user;
    if (!opt) return;
    canvas_status_widget_set_size(opt->value, 1);
    canvas_config_mark_dirty();
}

static void on_scope_status_lock_opt(PilotUiOption *opt, void *user)
{
    (void)user;
    if (!opt) return;
    canvas_status_widget_set_locked(opt->value ? 1 : 0);
    canvas_config_mark_dirty();
}

static void scope_on_tap(PilotPanel *panel)
{
    pthread_mutex_lock(&g_scope_lock);
    /* Tap cycles all 4 visible modes: Parade -> White Wave -> RGB Wave -> RGB Hist */
    g_scope_mode = (g_scope_mode + 1) % 4;
    g_sub_scope_opts[0].value = g_scope_mode;
    scope_rebuild_meta_locked();
    int mode = g_scope_mode;
    pthread_mutex_unlock(&g_scope_lock);

    canvas_config_mark_dirty();
    canvas_panel_invalidate(panel, 1);
    mlogf("CANVAS_SCOPE_TAP mode=%d", mode);
}

static void scope_on_cfg_load(PilotPanel *panel, const char *key, const char *val)
{
    (void)panel;
    int v = (int)strtol(val, NULL, 10);
    int apply_always      = -1;
    int apply_scope_lock  = -1;
    int apply_focal_mode  = -1;
    int apply_focal_lock  = -1;
    int apply_status_en   = -1;
    int apply_status_sz   = -1;
    int apply_status_lock = -1;
    pthread_mutex_lock(&g_scope_lock);
    if (strcmp(key, "mode") == 0 && v >= 0 && v < SCOPE_MODE_COUNT) {
        g_scope_mode = v;
        g_sub_scope_opts[0].value = v;
    } else if (strcmp(key, "scope_size") == 0 && v >= 0 && v <= 2) {
        g_scope_size = v;
        g_sub_scope_opts[1].value = v;
    } else if (strcmp(key, "fc") == 0 && v >= 0 && v < FC_MODE_COUNT) {
        g_fc_mode = v;
        g_sub_fc_opts[0].value = v;
    } else if (strcmp(key, "focal") == 0 && v >= 0 && v < FOCAL_MODE_COUNT) {
        apply_focal_mode = v;
        g_sub_focal_opts[0].value = v;
    } else if (strcmp(key, "focal_lock") == 0) {
        apply_focal_lock = v ? 1 : 0;
        g_sub_focal_opts[1].value = apply_focal_lock;
    } else if (strcmp(key, "status") == 0) {
        apply_status_en = v ? 1 : 0;
        g_sub_status_opts[0].value = apply_status_en;
    } else if (strcmp(key, "status_size") == 0 && v >= 0 && v <= 2) {
        apply_status_sz = v;
        g_sub_status_opts[1].value = v;
    } else if (strcmp(key, "status_lock") == 0) {
        apply_status_lock = v ? 1 : 0;
        g_sub_status_opts[2].value = apply_status_lock;
    } else if (strcmp(key, "gamma") == 0 && v >= 0 && v < SCOPE_GAMMA_COUNT) {
        g_scope_gamma = v;
        g_sub_scope_opts[2].value = v;
    } else if (strcmp(key, "wb") == 0) {
        g_scope_wb = v ? 1 : 0;
        g_sub_scope_opts[3].value = g_scope_wb;
    } else if (strcmp(key, "disp_sep") == 0) {
        g_disp_sep = v ? 1 : 0;
        g_sub_global_opts[0].value = g_disp_sep;
        apply_always = !g_disp_sep;
    } else if (strcmp(key, "lock") == 0 || strcmp(key, "scope_lock") == 0) {
        g_scope_locked = v ? 1 : 0;
        g_sub_scope_opts[4].value = g_scope_locked;
        apply_scope_lock = g_scope_locked;
    }
    scope_rebuild_meta_locked();
    pthread_mutex_unlock(&g_scope_lock);
    if (apply_focal_mode >= 0)
        canvas_focal_widget_set_mode(apply_focal_mode, 0);
    if (apply_focal_lock >= 0)
        canvas_focal_widget_set_locked(apply_focal_lock);
    if (apply_status_en >= 0)
        canvas_status_widget_set_enabled(apply_status_en, 0);
    if (apply_status_sz >= 0)
        canvas_status_widget_set_size(apply_status_sz, 0);
    if (apply_status_lock >= 0)
        canvas_status_widget_set_locked(apply_status_lock);
    if (apply_always >= 0)
        canvas_set_always_on(apply_always);
    if (apply_scope_lock >= 0 && g_scope_panel)
        canvas_panel_set_locked(g_scope_panel, apply_scope_lock);
}

static int scope_on_cfg_save(PilotPanel *panel, char *buf, size_t cap)
{
    (void)panel;
    pthread_mutex_lock(&g_scope_lock);
    int m  = g_scope_mode;
    int ssz = g_scope_size;
    int fc = g_fc_mode;
    int g  = g_scope_gamma;
    int w  = g_scope_wb;
    int ds = g_disp_sep;
    int sl = g_scope_locked;
    pthread_mutex_unlock(&g_scope_lock);
    int fm  = canvas_focal_widget_get_mode();
    int fl  = canvas_focal_widget_is_locked();
    int se  = canvas_status_widget_is_enabled();
    int stz = canvas_status_widget_get_size();
    int stl = canvas_status_widget_is_locked();
    return snprintf(buf, cap,
                    "mode=%d\nscope_size=%d\nfc=%d\nfocal=%d\nfocal_lock=%d\nstatus=%d\nstatus_size=%d\nstatus_lock=%d\ngamma=%d\nwb=%d\ndisp_sep=%d\nscope_lock=%d\n",
                    m, ssz, fc, fm, fl, se, stz, stl, g, w, ds, sl);
}

void canvas_scope_widget_init(void)
{
    if (g_scope_panel)
        return;

    /* Register full-screen False Color panel FIRST (underneath Scope card, touch-passthrough) */
    PilotPanelDesc fc_desc = {
        .id          = "fc",
        .default_x   = 0,
        .default_y   = 0,
        .w           = CANVAS_SCREEN_W,
        .h           = CANVAS_SCREEN_H,
        .draggable   = 0,
        .user_data   = NULL,
        .on_render   = fc_on_render,
        .on_tap      = NULL,
        .on_cfg_load = NULL,
        .on_cfg_save = NULL,
    };
    g_fc_panel = canvas_panel_register(&fc_desc);

    PilotPanelDesc desc = {
        .id          = "scope",
        .default_x   = SCOPE_DEF_X,
        .default_y   = SCOPE_DEF_Y,
        .w           = SCOPE_PANEL_W,
        .h           = SCOPE_PANEL_H,
        .draggable   = 1,
        .user_data   = NULL,
        .on_render   = scope_on_render,
        .on_tap      = scope_on_tap,
        .on_cfg_load = scope_on_cfg_load,
        .on_cfg_save = scope_on_cfg_save,
    };
    g_scope_panel = canvas_panel_register(&desc);
    const PilotCanvasStrings *cs = canvas_ui_str();
    g_scope_options[0].label = cs->menu_scope;
    g_scope_options[1].label = cs->menu_fc;
    g_scope_options[2].label = cs->menu_focal;
    g_scope_options[3].label = cs->menu_status;
    g_scope_options[4].label = cs->menu_global;

    g_sub_scope_opts[0].label   = cs->opt_scope_mode;
    g_sub_scope_opts[0].choices = cs->mode_choices;
    g_sub_scope_opts[1].label   = cs->opt_size;
    g_sub_scope_opts[1].choices = cs->size_choices;
    g_sub_scope_opts[2].label   = cs->opt_gamma;
    g_sub_scope_opts[3].label   = cs->opt_wb_calib;
    g_sub_scope_opts[3].choices = cs->wb_choices;
    g_sub_scope_opts[4].label   = cs->opt_lock_pos;
    g_sub_scope_opts[4].choices = cs->lock_choices;

    g_sub_fc_opts[0].label   = cs->opt_fc_mode;
    g_sub_fc_opts[0].choices = cs->fc_choices;

    g_sub_focal_opts[0].label   = cs->opt_focal_mode;
    g_sub_focal_opts[0].choices = cs->focal_choices;
    g_sub_focal_opts[1].label   = cs->opt_lock_pos;
    g_sub_focal_opts[1].choices = cs->lock_choices;

    g_sub_status_opts[0].label   = cs->opt_status_show;
    g_sub_status_opts[0].choices = cs->onoff_choices;
    g_sub_status_opts[1].label   = cs->opt_size;
    g_sub_status_opts[1].choices = cs->size_choices;
    g_sub_status_opts[2].label   = cs->opt_lock_pos;
    g_sub_status_opts[2].choices = cs->lock_choices;

    g_sub_global_opts[0].label   = cs->opt_disp_mode;
    g_sub_global_opts[0].choices = cs->disp_choices;

    (void)pilot_ui_register_options(
        "pilot_canvas.so",
        g_scope_options,
        sizeof(g_scope_options) / sizeof(g_scope_options[0]));
}

void canvas_scope_widget_tick_label(void)
{
    if (!g_scope_panel || !canvas_pilot_page_visible() ||
        !canvas_panel_any_attached())
        return;

    pthread_mutex_lock(&g_scope_lock);
    int is_off = (g_scope_mode == SCOPE_MODE_OFF);
    pthread_mutex_unlock(&g_scope_lock);
    if (is_off)
        return;

    char tail[20] = {0};
    if (!canvas_cp_read_focal_mm(tail, sizeof(tail)))
        (void)canvas_cp_read_sync_geom(tail, sizeof(tail));

    int changed = 0;
    pthread_mutex_lock(&g_scope_lock);
    if (strcmp(g_scope_tail, tail) != 0) {
        snprintf(g_scope_tail, sizeof(g_scope_tail), "%s", tail);
        scope_rebuild_meta_locked();
        changed = 1;
    }
    pthread_mutex_unlock(&g_scope_lock);

    if (changed)
        canvas_panel_invalidate(g_scope_panel, 0);
}

void canvas_scope_widget_tick_opd(void)
{
    if (!g_scope_panel || !canvas_pilot_page_visible() ||
        !canvas_panel_any_attached())
        return;

    pthread_mutex_lock(&g_scope_lock);
    int scope_on = (g_scope_mode != SCOPE_MODE_OFF);
    int fc_on    = (g_fc_mode != FC_MODE_OFF);
    int gamma    = g_scope_gamma;
    int wb       = g_scope_wb;
    pthread_mutex_unlock(&g_scope_lock);
    if (!scope_on && !fc_on)
        return;

    PilotOpdFrame tmp;
    if (!canvas_cp_read_opd(&tmp, gamma, wb))
        return;

    pthread_mutex_lock(&g_scope_lock);
    g_scope_frame = tmp;
    pthread_mutex_unlock(&g_scope_lock);

    if (fc_on && g_fc_panel)
        canvas_panel_invalidate(g_fc_panel, 0);
    if (scope_on)
        canvas_panel_invalidate(g_scope_panel, 0);
}
