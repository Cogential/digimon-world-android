/*
 * psx_ui_internal.h — state shared by the overlay's core (psx_ui.cpp) and its
 * menu pages (psx_ui_menu.cpp).
 */
#ifndef PSX_UI_INTERNAL_H
#define PSX_UI_INTERNAL_H

#include <string>

#include <SDL3/SDL.h>

#include "imgui.h"

/* PS1 pad bits, as the runtime lays them out (active-low in the pad word). */
enum : uint16_t {
    PADB_SELECT = 1u << 0,  PADB_L3 = 1u << 1,     PADB_R3 = 1u << 2,
    PADB_START = 1u << 3,   PADB_UP = 1u << 4,     PADB_RIGHT = 1u << 5,
    PADB_DOWN = 1u << 6,    PADB_LEFT = 1u << 7,   PADB_L2 = 1u << 8,
    PADB_R2 = 1u << 9,      PADB_L1 = 1u << 10,    PADB_R1 = 1u << 11,
    PADB_TRIANGLE = 1u << 12, PADB_CIRCLE = 1u << 13,
    PADB_CROSS = 1u << 14,  PADB_SQUARE = 1u << 15,
};

enum TouchMode { TOUCH_AUTO = 0, TOUCH_ALWAYS = 1, TOUCH_OFF = 2 };

/* Player-facing overlay settings, kept in <data>/ui_settings.ini. Settings the
 * runtime owns (renderer, filtering, colour) live in its settings.toml, and
 * mods in mods/state.toml. */
struct UiSettings {
    int   touch_mode = TOUCH_AUTO;
    float touch_opacity = 0.55f;
    float touch_scale = 1.0f;
    float speed = 1.0f;       /* chosen speed; fast-forward overrides it */
    float ff_speed = 3.0f;    /* speed while fast-forward is toggled on */
    bool  show_fps = false;
    int   volume = 100;
};

struct UiState {
    SDL_Window   *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    std::string   data_dir;
    UiSettings    s;
    bool          ready = false;
    bool          menu_open = false;
    bool          ff_active = false;
    float         dpi = 1.0f;           /* display density (1.0 = 160 dpi) */
    int           width = 0, height = 0; /* output size in pixels */
    ImFont       *font = nullptr;
    ImFont       *font_big = nullptr;
    double        fps = 0.0;
    bool          restart_needed = false; /* a setting that applies at boot changed */
};

extern UiState g_ui;

void ui_save_settings();
void ui_set_menu_open(bool open);
void ui_menu_draw();            /* psx_ui_menu.cpp */
void ui_menu_on_open();         /* psx_ui_menu.cpp: refresh cached state */

#endif /* PSX_UI_INTERNAL_H */
