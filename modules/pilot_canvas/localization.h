#ifndef PILOT_CANVAS_LOCALIZATION_H
#define PILOT_CANVAS_LOCALIZATION_H

#include "pilot_ui.h"

typedef struct PilotCanvasStrings {
    const char *menu_scope;
    const char *menu_fc;
    const char *menu_focal;
    const char *menu_status;
    const char *menu_global;

    const char *opt_scope_mode;
    const char *opt_size;
    const char *opt_gamma;
    const char *opt_wb_calib;
    const char *opt_lock_pos;
    const char *opt_fc_mode;
    const char *opt_focal_mode;
    const char *opt_status_show;
    const char *opt_disp_mode;

    const char *mode_choices[5];
    const char *size_choices[3];
    const char *wb_choices[2];
    const char *fc_choices[3];
    const char *disp_choices[2];
    const char *lock_choices[2];
    const char *focal_choices[3];
    const char *onoff_choices[2];
} PilotCanvasStrings;

static const PilotCanvasStrings k_canvas_strings[2] = {
    [PILOT_UI_LANG_EN] = {
        .menu_scope      = "Scope",
        .menu_fc         = "False Color",
        .menu_focal      = "Focal Bar",
        .menu_status     = "Status Bar",
        .menu_global     = "Display",
        .opt_scope_mode  = "Scope Mode",
        .opt_size        = "Size",
        .opt_gamma       = "Gamma Curve",
        .opt_wb_calib    = "WB Calib",
        .opt_lock_pos    = "Lock Pos",
        .opt_fc_mode     = "False Color",
        .opt_focal_mode  = "Display Mode",
        .opt_status_show = "Show Status Bar",
        .opt_disp_mode   = "DISP Page",
        .mode_choices    = { "RGB Parade", "Luma Wave", "RGB Wave", "RGB Hist", "Off" },
        .size_choices    = { "Small", "Medium", "Large" },
        .wb_choices      = { "Off", "Auto" },
        .fc_choices      = { "Off", "Key IRE", "ARRI" },
        .disp_choices    = { "Always On", "DISP Page" },
        .lock_choices    = { "Off", "Locked" },
        .focal_choices   = { "Off", "Zoom 2s", "Always On" },
        .onoff_choices   = { "Off", "On" },
    },
    [PILOT_UI_LANG_ZH] = {
        .menu_scope      = "示波器",
        .menu_fc         = "伪色",
        .menu_focal      = "焦段条",
        .menu_status     = "状态条",
        .menu_global     = "全局显示",
        .opt_scope_mode  = "示波器模式",
        .opt_size        = "尺寸",
        .opt_gamma       = "伽马曲线",
        .opt_wb_calib    = "白平衡校准",
        .opt_lock_pos    = "锁定位置",
        .opt_fc_mode     = "伪色模式",
        .opt_focal_mode  = "显示模式",
        .opt_status_show = "显示状态条",
        .opt_disp_mode   = "显示方式",
        .mode_choices    = { "RGB 分量", "亮度波形", "RGB 波形", "RGB 直方图", "关闭" },
        .size_choices    = { "小", "中", "大" },
        .wb_choices      = { "关", "自动" },
        .fc_choices      = { "关闭", "关键曝光区", "ARRI 全彩" },
        .disp_choices    = { "常驻显示", "独立 DISP 页" },
        .lock_choices    = { "关", "锁定" },
        .focal_choices   = { "关闭", "变焦弹出 2秒", "始终显示" },
        .onoff_choices   = { "关闭", "开启" },
    },
};

static inline const PilotCanvasStrings *canvas_ui_str(void)
{
    int lang = pilot_ui_get_language();
    return &k_canvas_strings[(lang == PILOT_UI_LANG_ZH) ? PILOT_UI_LANG_ZH : PILOT_UI_LANG_EN];
}

#endif
