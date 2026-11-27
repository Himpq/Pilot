#ifndef PILOT_MENU_UI_LOCALIZATION_H
#define PILOT_MENU_UI_LOCALIZATION_H

#include <stddef.h>
#include <string.h>

typedef enum {
    PILOT_LANG_EN = 0,
    PILOT_LANG_ZH = 1,
    PILOT_LANG_COUNT = 2
} PilotMenuLang;

typedef struct PilotMenuStrings {
    const char *version_title;
    const char *status_fmt;
    const char *status_enabled;
    const char *status_disabled;
    const char *no_description;
    const char *module_toggle_fmt;
    const char *toggle_on;
    const char *toggle_off;
    const char *module_info_switch;
    const char *option_default;
    const char *action_default;
    const char *lang_option_label;
    const char *busy_default;
} PilotMenuStrings;

static const char *const k_pilot_lang_names[PILOT_LANG_COUNT] = {
    "English",
    "简体中文",
};

static const char *const k_pilot_lang_codes[PILOT_LANG_COUNT] = {
    "en",
    "zh",
};

static const PilotMenuStrings k_pilot_menu_strings[PILOT_LANG_COUNT] = {
    [PILOT_LANG_EN] = {
        .version_title      = "Pilot Version",
        .status_fmt         = "Status: %s\nPath: %c:/pilot/%.63s",
        .status_enabled     = "Enabled",
        .status_disabled    = "Disabled",
        .no_description     = "No description.",
        .module_toggle_fmt  = "Module Switch: %s",
        .toggle_on          = "On",
        .toggle_off         = "Off",
        .module_info_switch = "Module Info & Switch",
        .option_default     = "Option",
        .action_default     = "Run",
        .lang_option_label  = "Language",
        .busy_default       = "Processing...",
    },
    [PILOT_LANG_ZH] = {
        .version_title      = "Pilot 版本",
        .status_fmt         = "状态: %s\n路径: %c:/pilot/%.63s",
        .status_enabled     = "已启用",
        .status_disabled    = "已停用",
        .no_description     = "暂无说明。",
        .module_toggle_fmt  = "模块开关: %s",
        .toggle_on          = "开",
        .toggle_off         = "关",
        .module_info_switch = "模块信息与开关",
        .option_default     = "选项",
        .action_default     = "执行",
        .lang_option_label  = "语言",
        .busy_default       = "处理中...",
    },
};

static inline const PilotMenuStrings *pilot_menu_get_strings(PilotMenuLang lang)
{
    size_t idx = ((size_t)lang < (size_t)PILOT_LANG_COUNT) ? (size_t)lang : 0u;
    return &k_pilot_menu_strings[idx];
}

static inline int pilot_menu_parse_lang(const char *val, PilotMenuLang *out)
{
    if (!val || !out)
        return 0;
    if (strncmp(val, "zh", 2) == 0 || val[0] == '1') {
        *out = PILOT_LANG_ZH;
        return 1;
    }
    if (strncmp(val, "en", 2) == 0 || val[0] == '0') {
        *out = PILOT_LANG_EN;
        return 1;
    }
    return 0;
}

#endif
