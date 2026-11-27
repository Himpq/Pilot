/* Standalone Sony-Style Horizontal Focal-Length Bar Widget (PilotPanel "focal"):
 *   - Compact 104x40 transparent-background panel matching Sony's native W/T zoom bar:
 *       Top:    Crisp white-framed horizontal bar (100x18) with "W" on left, "T" on right,
 *               dark-gray inner track split by a clean white sliding position gap.
 *       Bottom: Centered bold scale=2 actual focal length (e.g. "28mm" / "28.5mm")
 *               with 1px dark drop shadow.
 *   - Uses native CP ZoomRingPosition (0x016A4998 + 0x122, u16 0x0000..0xFFFF)
 *     for instant zero-calibration 0%..100% progress on any E-mount zoom lens!
 *   - 3 modes: Off / Auto (2s timeout after zoom change) / Always On
 *   - Independent per-widget Lock Pos. */

#include "pilot_canvas_internal.h"

#define FOCAL_PANEL_W      104
#define FOCAL_PANEL_H      40
#define FOCAL_DEF_X        268
#define FOCAL_DEF_Y        386
#define FOCAL_AUTO_HIDE_MS 2000u

static PilotPanel *g_focal_panel;
static pthread_mutex_t g_focal_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_focal_mode     = FOCAL_MODE_OFF;
static int g_focal_locked   = 0;
static int g_focal_visible  = 0;
static u64 g_focal_hide_ms  = 0u;
static u32 g_focal_cur_mm10 = 0u;
static u16 g_focal_cur_zpos = 0x8000u;

static void fmt_mm10(u32 mm10, char *out, size_t cap)
{
    if ((mm10 % 10u) == 0u)
        snprintf(out, cap, "%umm", mm10 / 10u);
    else
        snprintf(out, cap, "%u.%umm", mm10 / 10u, mm10 % 10u);
}

static void focal_on_render(PilotPanel *panel, PilotCanvas *c)
{
    (void)panel;
    if (!c)
        return;

    int show;
    u32 cur;
    u16 zpos;
    pthread_mutex_lock(&g_focal_lock);
    show = (g_focal_mode == FOCAL_MODE_ALWAYS) ||
           (g_focal_mode == FOCAL_MODE_AUTO2S && g_focal_visible);
    cur  = g_focal_cur_mm10;
    zpos = g_focal_cur_zpos;
    pthread_mutex_unlock(&g_focal_lock);

    pilot_canvas_begin_batch_v1(c);
    if (!show || cur == 0u) {
        pilot_canvas_end_batch_v1(c);
        return;
    }

    int cw = (int)c->w;

    /* 1. Top compact Sony-style W [###  #######] T horizontal bar (100x18) */
    int bx = 2, by = 1, bw = cw - 4, bh = 18;
    pilot_canvas_fill_rect_v1(c, bx - 1, by - 1, bw + 2, bh + 2,
                              0xD8181E24u, PILOT_CANVAS_BLEND_NONE);
    pilot_canvas_fill_rect_v1(c, bx, by, bw, bh,
                              0xF6FFFFFFu, PILOT_CANVAS_BLEND_NONE);

    pilot_canvas_draw_osd_text_v1(c, bx + 4, by + 5, 1, 0xFF20262Cu, 0x00000000u, "W");
    pilot_canvas_draw_osd_text_v1(c, bx + bw - 9, by + 5, 1, 0xFF20262Cu, 0x00000000u, "T");

    int tx = bx + 13;
    int ty = by + 3;
    int tw = bw - 26;
    int th = bh - 6;
    int gap_w = 10;
    if (tw > gap_w + 4) {
        int span = tw - gap_w;
        int pos = (int)(((u64)zpos * (u64)span + 32767u) / 65535u);
        if (pos < 0) pos = 0;
        if (pos > span) pos = span;
        if (pos > 0)
            pilot_canvas_fill_rect_v1(c, tx, ty, pos, th,
                                      0xF04C545Cu, PILOT_CANVAS_BLEND_NONE);
        int rx = tx + pos + gap_w;
        int rw = (tx + tw) - rx;
        if (rw > 0)
            pilot_canvas_fill_rect_v1(c, rx, ty, rw, th,
                                      0xF04C545Cu, PILOT_CANVAS_BLEND_NONE);
    }

    /* 2. Bottom centered focal length text (e.g. "28mm") on subtle dark pill */
    char cur_txt[16];
    fmt_mm10(cur, cur_txt, sizeof(cur_txt));
    int txt_w = (int)strlen(cur_txt) * 12;
    int txt_x = (cw - txt_w) / 2;
    if (txt_x < 4) txt_x = 4;
    int txt_y = by + bh + 4;
    pilot_canvas_fill_rect_v1(c, txt_x - 4, txt_y - 2, txt_w + 6, 17,
                              0x8806080Au, PILOT_CANVAS_BLEND_NONE);
    pilot_canvas_draw_osd_text_v1(c, txt_x, txt_y, 2,
                                  0xFFFFFFFFu, 0x00000000u, cur_txt);

    pilot_canvas_end_batch_v1(c);
}

void canvas_focal_widget_init(void)
{
    if (g_focal_panel)
        return;
    PilotPanelDesc desc = {
        .id          = "focal",
        .default_x   = FOCAL_DEF_X,
        .default_y   = FOCAL_DEF_Y,
        .w           = FOCAL_PANEL_W,
        .h           = FOCAL_PANEL_H,
        .draggable   = 1,
        .user_data   = NULL,
        .on_render   = focal_on_render,
        .on_tap      = NULL,
        .on_cfg_load = NULL,
        .on_cfg_save = NULL,
    };
    g_focal_panel = canvas_panel_register(&desc);
}

void canvas_focal_widget_set_mode(int mode, int invalidate)
{
    if (mode < 0 || mode >= FOCAL_MODE_COUNT)
        mode = FOCAL_MODE_OFF;
    u64 now = canvas_now_ms();
    pthread_mutex_lock(&g_focal_lock);
    g_focal_mode = mode;
    if (mode == FOCAL_MODE_AUTO2S) {
        g_focal_visible = 1;
        g_focal_hide_ms = now + FOCAL_AUTO_HIDE_MS;
    } else {
        g_focal_visible = (mode == FOCAL_MODE_ALWAYS);
        g_focal_hide_ms = 0u;
    }
    pthread_mutex_unlock(&g_focal_lock);
    if (invalidate && g_focal_panel)
        canvas_panel_invalidate(g_focal_panel, 0);
}

int canvas_focal_widget_get_mode(void)
{
    pthread_mutex_lock(&g_focal_lock);
    int m = g_focal_mode;
    pthread_mutex_unlock(&g_focal_lock);
    return m;
}

void canvas_focal_widget_set_locked(int locked)
{
    pthread_mutex_lock(&g_focal_lock);
    g_focal_locked = locked ? 1 : 0;
    pthread_mutex_unlock(&g_focal_lock);
    if (g_focal_panel)
        canvas_panel_set_locked(g_focal_panel, g_focal_locked);
}

int canvas_focal_widget_is_locked(void)
{
    pthread_mutex_lock(&g_focal_lock);
    int lk = g_focal_locked;
    pthread_mutex_unlock(&g_focal_lock);
    return lk;
}

void canvas_focal_widget_reset_range(void)
{
    /* No-op: native ZoomRingPosition (0x0000..0xFFFF) requires zero calibration. */
}

void canvas_focal_widget_tick(void)
{
    if (!g_focal_panel || !canvas_pilot_page_visible() ||
        !canvas_panel_any_attached())
        return;

    pthread_mutex_lock(&g_focal_lock);
    int mode = g_focal_mode;
    pthread_mutex_unlock(&g_focal_lock);
    if (mode == FOCAL_MODE_OFF)
        return;

    u32 mm10 = 0u;
    u16 zpos = 0u;
    if (!canvas_cp_read_focal_mm10_zpos(&mm10, &zpos))
        return;

    u64 now = canvas_now_ms();
    int changed = 0;

    pthread_mutex_lock(&g_focal_lock);
    u32 dz = (zpos > g_focal_cur_zpos) ? (u32)(zpos - g_focal_cur_zpos)
                                       : (u32)(g_focal_cur_zpos - zpos);
    int focal_moved = (g_focal_cur_mm10 != 0u &&
                       (mm10 != g_focal_cur_mm10 || dz >= 128u));
    if (mm10 != g_focal_cur_mm10 || zpos != g_focal_cur_zpos) {
        g_focal_cur_mm10 = mm10;
        g_focal_cur_zpos = zpos;
        changed = 1;
    }

    if (mode == FOCAL_MODE_AUTO2S) {
        if (focal_moved) {
            g_focal_visible = 1;
            g_focal_hide_ms = now + FOCAL_AUTO_HIDE_MS;
            changed = 1;
        } else if (g_focal_visible && g_focal_hide_ms > 0u && now >= g_focal_hide_ms) {
            g_focal_visible = 0;
            g_focal_hide_ms = 0u;
            changed = 1;
        }
    }
    pthread_mutex_unlock(&g_focal_lock);

    if (changed)
        canvas_panel_invalidate(g_focal_panel, 0);
}
