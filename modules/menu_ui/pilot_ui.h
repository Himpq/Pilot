#ifndef PILOT_UI_H
#define PILOT_UI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PILOT_UI_PAGE_DETAIL = 0,
    PILOT_UI_PAGE_LIST = 1
} PilotUiPageKind;

typedef enum {
    PILOT_UI_COMP_TEXT = 1,
    PILOT_UI_COMP_SUBMENU = 2,
    PILOT_UI_COMP_TOGGLE = 3
} PilotUiComponentKind;

typedef struct PilotUiPage PilotUiPage;

typedef struct PilotUiComponent {
    PilotUiComponentKind kind;
    const char *label;
    const char *value;
    const PilotUiPage *target_page;
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} PilotUiComponent;

struct PilotUiPage {
    PilotUiPageKind kind;
    const char *title;
    const char *content;
    const char *summary;
    const PilotUiComponent *components;
    size_t component_count;
    void (*on_select)(const PilotUiPage *page, size_t index, void *user);
    const PilotUiPage *(*build_dynamic)(void *user);
    void *user_data;
};

#ifdef __cplusplus
}
#endif

#endif
