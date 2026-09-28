/*
 * psx_ui_host.h — what the overlay needs from the runtime. Implemented in the
 * runtime's main.cpp (PSX_HAS_UI_OVERLAY) and in its C modules.
 */
#ifndef PSX_UI_HOST_H
#define PSX_UI_HOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* main.cpp bridges */
int      psx_host_savestate_submit(int slot, int save); /* 1 if queued */
int      psx_host_video_smooth(void);
void     psx_host_set_video_smooth(int on);
int      psx_host_texture_filter(void);
void     psx_host_set_texture_filter(int on);
int      psx_host_screen_kind(void);                    /* 0 raw 1 crt 2 composite 3 trinitron */
void     psx_host_set_screen_kind(int kind);
int      psx_host_internal_scale(void);
uint64_t psx_host_frame_count(void);
void     psx_host_input_guard(void);    /* swallow buttons still held when the menu closes */
void     psx_host_shutdown(void);       /* flush saves etc. before the process goes away */

/* savestate.c */
int savestate_slot_exists(int slot);
int savestate_slot_path(int slot, char *out, size_t cap);

/* host_osd.c */
int  host_volume_get(void);
void host_volume_set(int percent);
void host_osd_push(const char *text, int duration_ms);

/* psx_cheats.c */
int psx_cheats_set_enabled(const char *name, int enabled);

/* memory.c: guest RAM access */
uint8_t  psx_read_byte(uint32_t addr);
uint16_t psx_read_half(uint32_t addr);
uint32_t psx_read_word(uint32_t addr);
void     psx_write_byte(uint32_t addr, uint8_t val);
void     psx_write_half(uint32_t addr, uint16_t val);
void     psx_write_word(uint32_t addr, uint32_t val);

/* Android glue (psx_android.cpp) */
void psx_android_restart_app(void);
void psx_android_quit_app(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_UI_HOST_H */
