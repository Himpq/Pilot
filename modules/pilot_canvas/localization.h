#ifndef PILOT_CANVAS_LOCALIZATION_H
#define PILOT_CANVAS_LOCALIZATION_H

#include "pilot_ui.h"

typedef struct PilotCanvasStrings {
    const char *opt_scope_mode;
    const char *opt_false_color;
    const char *opt_gamma_curve;
    const char *opt_wb_calib;
    const char *opt_disp_page;
    const char *opt_lock_pos;
} PilotCanvasStrings;

static const PilotCanvasStrings k_canvas_strings[2] = {
    [PILOT_UI_LANG_EN] = {
        .opt_scope_mode  = "Scope Mode",
        .opt_false_color = "False Color",
        .opt_gamma_curve = "Gamma Curve",
        .opt_wb_calib    = "WB Calib",
        .opt_disp_page   = "DISP Page",
        .opt_lock_pos    = "Lock Pos",
    },
    [PILOT_UI_LANG_ZH] = {
        .opt_scope_mode  = "示波器模式",
        .opt_false_color = "伪色彩",
        .opt_gamma_curve = "伽马曲线",
        .opt_wb_calib    = "白平衡校准",
        .opt_disp_page   = "DISP 页面",
        .opt_lock_pos    = "锁定位置",
    },
};

static inline const PilotCanvasStrings *canvas_ui_str(void)
{
    int lang = pilot_ui_get_language();
    return &k_canvas_strings[(lang == PILOT_UI_LANG_ZH) ? PILOT_UI_LANG_ZH : PILOT_UI_LANG_EN];
}

#endif
