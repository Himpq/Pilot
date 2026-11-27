#ifndef PILOT_DESQUEEZE_LOCALIZATION_H
#define PILOT_DESQUEEZE_LOCALIZATION_H

#include "pilot_ui.h"

typedef struct DesqueezeStrings {
    const char *opt_mode;
    const char *opt_src;
    const char *opt_lcd;
    const char *opt_evf;
    const char *opt_hdmi;
    const char *mode_names[8];
    const char *src_names[2];
    const char *toast_on_fmt;
    const char *toast_off;
    const char *toast_err;
} DesqueezeStrings;

static const DesqueezeStrings k_dsq_strings[2] = {
    [PILOT_UI_LANG_EN] = {
        .opt_mode = "De-Squeeze",
        .opt_src  = "Aspect Base",
        .opt_lcd  = "Monitor (LCD)",
        .opt_evf  = "Viewfinder (EVF)",
        .opt_hdmi = "External (HDMI)",
        .mode_names = {
            "Off",
            "1.33x",
            "1.3x",
            "1.5x",
            "1.6x",
            "1.8x",
            "2.0x (Crop)",
            "2.0x (Full)",
        },
        .src_names = {
            "16:9 (Normal)",
            "3:2 (OpenGate)",
        },
        .toast_on_fmt = "De-Squeeze: %s (%s)",
        .toast_off    = "De-Squeeze: Off",
        .toast_err    = "De-Squeeze error",
    },
    [PILOT_UI_LANG_ZH] = {
        .opt_mode = "反挤压倍率",
        .opt_src  = "画幅基准",
        .opt_lcd  = "屏幕 (LCD)",
        .opt_evf  = "取景器 (EVF)",
        .opt_hdmi = "外接 (HDMI)",
        .mode_names = {
            "关闭",
            "1.33x",
            "1.3x",
            "1.5x",
            "1.6x",
            "1.8x",
            "2.0x (裁幅)",
            "2.0x (全宽)",
        },
        .src_names = {
            "16:9 (常规)",
            "3:2 (OpenGate)",
        },
        .toast_on_fmt = "反挤压: %s (%s)",
        .toast_off    = "反挤压: 关闭",
        .toast_err    = "反挤压切换失败",
    },
};

static inline const DesqueezeStrings *dsq_ui_str(void)
{
    int lang = pilot_ui_get_language();
    return &k_dsq_strings[(lang == PILOT_UI_LANG_ZH) ? PILOT_UI_LANG_ZH : PILOT_UI_LANG_EN];
}

#endif
