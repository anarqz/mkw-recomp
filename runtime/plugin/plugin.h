// Native plugins of the enabled mods (plugin.c; the API they get: mkw_plugin.h).
#pragma once
#include "mkw_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

void plugins_load(void);          // before ldr_load: each enabled mod's plugin/ libraries, their mkw_plugin_init
void plugins_game_loaded(void);   // after ldr_load, before hostfn_seal: the plugins' on_game_loaded callbacks
void plugins_started(void);       // after hostfn_seal: no more address overrides
// The mods' tabs of the in-game menu's settings (MkwPluginApi.settings_tab), for ui/menu/settings_tabs.cpp
int plugins_tab_count(void);
const char *plugins_tab_title(int i);
void plugins_tab_draw(int i, float scale);   // the tab's page, inside the current ImGui window
void plugins_tab_shown(int i);               // the tab was selected: its next frame is the page's first (opened)
void plugins_exit(void);          // the game ends normally (window closed, the guest's exit): the on_exit callbacks, once

// The plugins' overlay drawings (plugin_overlay.cpp): registered and switched by the API, laid out by ui/menu/menu.cpp
int plugin_overlay_add(MkwOverlayFn fn, void *user);   // 0 when full
// Version 6 (plugin_overlay.cpp): the in-game menu's settings for a plugin's page (MkwPluginApi.settings_*)
int plugin_settings_tabs(const char **names, int max);
int plugin_settings_rows(int tab, MkwSettingFn cb, void *user);
int plugin_settings_set(int tab, const char *id, const char *value);
int plugin_settings_confirm_reset(void);
const char *plugin_settings_status(void);
// Version 6 (plugin.c): while a plugin holds the in-game menu (menu_redirect), opening it (ui/menu/menu.cpp
// menu_toggle: F1, Esc, the pad's shortcut, menu_open) calls the plugin instead; 1 when one holds it
int plugins_menu_redirect(void);
void plugin_overlay_show(MkwOverlayFn fn, int on);
int plugins_overlay_wanted(void);                       // one is on: the overlay lays out a frame
// In the current ImGui frame. perf: where the menu's own display at the top right is this frame (x0, y0, x1, y1; zeros
// when off), told to the drawings.
void plugins_overlay_draw(float width, float height, float scale, const float perf[4]);
// Version 5 (plugin_overlay.cpp): the drawings' own triangles and textures, their input, the game's picture, their faults
enum {
    PLUGIN_OVERLAY_TEXTURES = 64,                       // texture_create's limit
    PLUGIN_OVERLAY_SETS = PLUGIN_OVERLAY_TEXTURES + 16, // ImGui's descriptor pool has room for these texture ids of
                                                        // theirs: the textures, the game's picture and the pictures
                                                        // of another size that frames in flight still sample
};
int plugin_overlay_on_fault(MkwOverlayFn fn, void (*cb)(void *user), void *user);
int plugin_overlay_capture(MkwOverlayFn fn, int flags);
int plugin_overlay_picture(MkwOverlayFn fn, int max_height);
uint32_t plugin_texture_create(int w, int h, const uint8_t *rgba, int row_bytes, int flags);
int plugin_texture_update(uint32_t handle, const uint8_t *rgba, int row_bytes);
void plugin_texture_destroy(uint32_t handle);
void plugins_overlay_submit(void);                      // after ImGui::Render: the drawings' lists into the frame, under ImGui's
int plugins_overlay_input(void);                        // a drawing has the input now: the window's events go to it
int plugins_overlay_text(void);                         // and wants the keyboard's text
void plugins_overlay_event(const MkwInputEvent *e);     // window thread (ui/menu/menu_sdl.cpp)
// (the present thread's side, plugins_overlay_vk, _record, _picture_take and _gpu, is in ui/menu/menu.h: Vulkan types)
void plugins_overlay_vk_lost(void);                     // ImGui's Vulkan back end is about to go: its texture ids with it
void plugins_overlay_vk_release(void);                  // before the device goes away

// Version 4 (plugin.c): the plugins' pad filters, run by input/input.c at each read of a player's input (sticks: left
// x, y, right x, y; guide NULL for players 2-8), and the shown picture a plugin asked for (gfx_present.c).
void plugins_input_filter(int player, uint64_t *buttons, int32_t sticks[4], int *guide);
int plugins_frame_wanted(void);                                    // this present copies its swapchain image
void plugins_frame_captured(const uint8_t *rgba, int w, int h);    // RGBA8, tight rows; the present thread

#ifdef __cplusplus
}
#endif
