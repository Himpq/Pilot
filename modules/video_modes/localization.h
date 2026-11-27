#ifndef PILOT_VIDEO_MODES_LOCALIZATION_H
#define PILOT_VIDEO_MODES_LOCALIZATION_H

#include "pilot_ui.h"

typedef struct VideoModesStrings {
    const char *option_label;
    const char *toast_ok_fmt;
    const char *toast_err_fmt;
    const char *busy_fmt;
} VideoModesStrings;

static const VideoModesStrings k_vmode_strings[2] = {
    [PILOT_UI_LANG_EN] = {
        .option_label  = "Video Mode",
        .toast_ok_fmt  = "Video Mode: %s",
        .toast_err_fmt = "Video Mode err (%s)",
        .busy_fmt      = "Switching to %s...\nPlease wait",
    },
    [PILOT_UI_LANG_ZH] = {
        .option_label  = "视频模式",
        .toast_ok_fmt  = "视频模式: %s",
        .toast_err_fmt = "视频模式切换失败 (%s)",
        .busy_fmt      = "正在切换至 %s...\n请稍候",
    },
};

static inline const VideoModesStrings *vmode_ui_str(void)
{
    int lang = pilot_ui_get_language();
    return &k_vmode_strings[(lang == PILOT_UI_LANG_ZH) ? PILOT_UI_LANG_ZH : PILOT_UI_LANG_EN];
}

#endif
