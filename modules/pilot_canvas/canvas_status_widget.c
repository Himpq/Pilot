/* Standalone LiveView Telemetry Status Bar (CMOS temp, Body temp, optional USB-PD V).
 * Reads CP RAM UPM telemetry block (0x011C6964) at ~1 Hz via single DMA read.
 * Rendered on Sony's UI thread via canvas_panel_invalidate(panel, 0). */

#include "pilot_canvas_internal.h"

#define STATUS_MAX_W     380
#define STATUS_MAX_H      36
#define STATUS_DEF_X      16
#define STATUS_DEF_Y      44

static pthread_mutex_t g_status_lock = PTHREAD_MUTEX_INITIALIZER;
static PilotPanel     *g_status_panel;
static int             g_status_enabled;
static int             g_status_size;   /* 0=Small(scale=1), 1=Medium(scale=2), 2=Large(scale=3) */
static int             g_status_locked;
static PilotTelemetry  g_status_data;

static u32 temp_color(int16_t c10)
{
    if (c10 >= 550)
        return 0xFFFF4444u; /* >= 55.0 C: red alert */
    if (c10 >= 450)
        return 0xFFFFC840u; /* 45.0..54.9 C: amber warm */
    return 0xFFE8F4FFu;     /* < 45.0 C: crisp ice-white */
}

static void fmt_temp(char *buf, size_t cap, const char *prefix, int16_t c10)
{
    if (c10 <= -900) {
        buf[0] = '\0';
        return;
    }
    int neg = (c10 < 0);
    int v = neg ? -c10 : c10;
    snprintf(buf, cap, "%s%s%d.%dC", prefix, neg ? "-" : "", v / 10, v % 10);
}

static void status_on_render(PilotPanel *panel, PilotCanvas *c)
{
    pthread_mutex_lock(&g_status_lock);
    int enabled = g_status_enabled;
    int sz      = g_status_size;
    PilotTelemetry t = g_status_data;
    pthread_mutex_unlock(&g_status_lock);

    pilot_canvas_begin_batch_v1(c);
    if (!enabled || !t.valid) {
        pilot_canvas_end_batch_v1(c);
        return;
    }

    int scale = (sz == 2) ? 3 : ((sz == 1) ? 2 : 1);
    int pad_x = 4 + scale * 2;
    int pad_y = 3 + scale * 2;
    int gap   = 6 * scale;
    int adv   = 6 * scale;
    int pill_h = 7 * scale + pad_y * 2;
    if (pill_h > STATUS_MAX_H)
        pill_h = STATUS_MAX_H;

    char s_cmos[16] = {0};
    char s_body[16] = {0};
    char s_pd[12]   = {0};

    fmt_temp(s_cmos, sizeof(s_cmos), "C:", t.cmos_c10);
    fmt_temp(s_body, sizeof(s_body), "B:", t.body_c10);
    if (t.ext_mv >= 3000u)
        snprintf(s_pd, sizeof(s_pd), "%u.%uV",
                 (unsigned)(t.ext_mv / 1000u),
                 (unsigned)((t.ext_mv % 1000u) / 100u));

    const char *segs[3] = { s_cmos, s_body, s_pd };
    u32 cols[3] = {
        temp_color(t.cmos_c10),
        temp_color(t.body_c10),
        0xFF55D8FFu
    };

    int total_w = pad_x * 2;
    int parts = 0;
    for (int i = 0; i < 3; ++i) {
        if (!segs[i][0])
            continue;
        if (parts > 0)
            total_w += gap;
        total_w += (int)strlen(segs[i]) * adv - scale;
        ++parts;
    }
    if (parts == 0) {
        pilot_canvas_end_batch_v1(c);
        return;
    }
    if (total_w > STATUS_MAX_W)
        total_w = STATUS_MAX_W;

    if (panel)
        canvas_panel_set_hit_size(panel, total_w, pill_h);

    /* Translucent dark pill + subtle 1px border */
    const u32 bg_rgba     = 0xAA080B10u;
    const u32 border_rgba = 0x883A4452u;
    pilot_canvas_fill_rect_v1(c, 0, 0, total_w, pill_h, bg_rgba,
                              PILOT_CANVAS_BLEND_NONE);
    pilot_canvas_fill_rect_v1(c, 0, 0, total_w, 1, border_rgba, PILOT_CANVAS_BLEND_NONE);
    pilot_canvas_fill_rect_v1(c, 0, pill_h - 1, total_w, 1, border_rgba, PILOT_CANVAS_BLEND_NONE);
    pilot_canvas_fill_rect_v1(c, 0, 0, 1, pill_h, border_rgba, PILOT_CANVAS_BLEND_NONE);
    pilot_canvas_fill_rect_v1(c, total_w - 1, 0, 1, pill_h, border_rgba, PILOT_CANVAS_BLEND_NONE);

    int x = pad_x;
    int y = (pill_h - 7 * scale) / 2;
    int drawn = 0;
    for (int i = 0; i < 3; ++i) {
        if (!segs[i][0])
            continue;
        if (drawn > 0)
            x += gap;
        pilot_canvas_draw_osd_text_v1(c, x, y, scale, cols[i], 0u, segs[i]);
        x += (int)strlen(segs[i]) * adv - scale;
        ++drawn;
    }

    pilot_canvas_end_batch_v1(c);
}

void canvas_status_widget_set_enabled(int enabled, int invalidate)
{
    int en = enabled ? 1 : 0;
    pthread_mutex_lock(&g_status_lock);
    int changed = (g_status_enabled != en);
    g_status_enabled = en;
    pthread_mutex_unlock(&g_status_lock);

    if (invalidate && g_status_panel && changed)
        canvas_panel_invalidate(g_status_panel, 0);
}

int canvas_status_widget_is_enabled(void)
{
    pthread_mutex_lock(&g_status_lock);
    int en = g_status_enabled;
    pthread_mutex_unlock(&g_status_lock);
    return en;
}

void canvas_status_widget_set_size(int size_idx, int invalidate)
{
    if (size_idx < 0) size_idx = 0;
    if (size_idx > 2) size_idx = 2;
    pthread_mutex_lock(&g_status_lock);
    int changed = (g_status_size != size_idx);
    g_status_size = size_idx;
    pthread_mutex_unlock(&g_status_lock);

    if (invalidate && g_status_panel && changed)
        canvas_panel_invalidate(g_status_panel, 0);
}

int canvas_status_widget_get_size(void)
{
    pthread_mutex_lock(&g_status_lock);
    int sz = g_status_size;
    pthread_mutex_unlock(&g_status_lock);
    return sz;
}

void canvas_status_widget_set_locked(int locked)
{
    int lk = locked ? 1 : 0;
    pthread_mutex_lock(&g_status_lock);
    g_status_locked = lk;
    pthread_mutex_unlock(&g_status_lock);
    if (g_status_panel)
        canvas_panel_set_locked(g_status_panel, lk);
}

int canvas_status_widget_is_locked(void)
{
    pthread_mutex_lock(&g_status_lock);
    int lk = g_status_locked;
    pthread_mutex_unlock(&g_status_lock);
    return lk;
}

void canvas_status_widget_init(void)
{
    if (g_status_panel)
        return;

    PilotPanelDesc desc = {
        .id          = "status",
        .default_x   = STATUS_DEF_X,
        .default_y   = STATUS_DEF_Y,
        .w           = STATUS_MAX_W,
        .h           = STATUS_MAX_H,
        .draggable   = 1,
        .user_data   = NULL,
        .on_render   = status_on_render,
        .on_tap      = NULL,
        .on_cfg_load = NULL,
        .on_cfg_save = NULL,
    };
    g_status_panel = canvas_panel_register(&desc);
}

void canvas_status_widget_tick(void)
{
    if (!g_status_panel || !canvas_pilot_page_visible() ||
        !canvas_panel_any_attached())
        return;

    pthread_mutex_lock(&g_status_lock);
    int en = g_status_enabled;
    pthread_mutex_unlock(&g_status_lock);
    if (!en)
        return;

    PilotTelemetry cur;
    if (!canvas_cp_read_telemetry(&cur))
        return;

    int changed = 0;
    pthread_mutex_lock(&g_status_lock);
    if (!g_status_data.valid ||
        g_status_data.cmos_c10 != cur.cmos_c10 ||
        g_status_data.body_c10 != cur.body_c10 ||
        (g_status_data.ext_mv / 100u) != (cur.ext_mv / 100u)) {
        g_status_data = cur;
        changed = 1;
    }
    pthread_mutex_unlock(&g_status_lock);

    if (changed)
        canvas_panel_invalidate(g_status_panel, 0);
}
