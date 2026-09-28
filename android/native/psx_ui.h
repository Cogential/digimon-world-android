/*
 * psx_ui.h — in-game overlay: the settings menu and the on-screen touch
 * controls, drawn with Dear ImGui over the runtime's software-renderer
 * present path. Called from the runtime (main.cpp) when built with
 * PSX_HAS_UI_OVERLAY.
 */
#ifndef PSX_UI_H
#define PSX_UI_H

#include <stdint.h>

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* After the window and renderer exist. data_dir holds settings and mods. */
void psx_ui_init(SDL_Window *window, SDL_Renderer *renderer, const char *data_dir);

/* Draw the overlay (touch controls, FPS, menu) onto the frame about to be
 * presented. */
void psx_ui_render(SDL_Renderer *renderer);

/* 1 while the menu is open; the runtime holds the game paused meanwhile. */
int psx_ui_menu_open(void);

/* Buttons held on the touch controls, as a PS1 pad word (active-low, the
 * runtime's PAD_* bit layout). 0xFFFF when nothing is pressed. */
uint16_t psx_ui_pad_buttons(void);

/* Emulation speed the player picked (1.0 = normal). */
float psx_ui_game_speed(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_UI_H */
