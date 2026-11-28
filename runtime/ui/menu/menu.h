// The in-game menu (ui/menu/menu.cpp, Dear ImGui) and its seams to the platform side: the window and its input (ui/menu/menu_sdl.cpp,
// SDL3), the gamepad (input_pad_read, input.h) and the present path
// (gfx_present.c). The menu core calls no window system: events come in through menu_event_*, the drawing happens in the
// present path's command buffer.
//
// F1 or Esc on the keyboard and the controller's menu shortcut (Minus and Plus held together, or the Guide button
// when Settings > Controls says so; consumed in poll_merged, input_menu_gate in input.h) open it;
// F1, Esc, B or "Continue" close it. While it is open the game gets no input (input_menu, input.h) and keeps running; the
// cursor is released from the mouse camera. Tabs Screen, Game, Sound, Controls and Menu (L / R or Q / E switch them) hold
// option rows (ui/menu/option_rows.h) over the cfg settings; the bottom row has "Continue" and "Quit game". Opening
// focuses "Continue", so a stray gamepad deflection cannot retune a setting; switching tabs focuses the new page's
// first row.
// Test hooks (ui/menu/menu.cpp): MKW_MENU_TEST=<frame>[:<frames per page>] opens the menu at that presented frame and saves
// one swapchain shot per tab into MKW_MENU_TEST_DIR; MKW_MENU_TEST_SET=<frame>:<key>=<value>[,...][;...] sets settings
// at given frames without input (SAVE=1 writes mkw.ini).
//
// Threads: the event functions are called by the window thread, menu_toggle by whichever thread polls the pad, the
// overlay functions by the present path under its lock. ImGui itself runs on the present path only.
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- state, any thread
int menu_is_open(void);
void menu_toggle(void);                   // F1 / Esc on the keyboard, Minus+Plus on the pad
void menu_perf_cycle(void);               // F10: the performance overlay off, fps, full
void menu_notice(const char *text, int seconds);   // a line at the top of the picture for that long, menu closed or not

// ---- events from the window adapter, used only while the menu is open. key: an ImGuiKey; coordinates in pixels of the
// window's client area, which is the swapchain image.
void menu_event_key(int key, int down);
void menu_event_mouse_pos(float x, float y);
void menu_event_mouse_button(int button, int down);   // 0 left, 1 right, 2 middle
void menu_event_wheel(float dx, float dy);
void menu_event_focus(int focused);

// ---- the overlay, drawn into the swapchain image after the game image (gfx_present.c, present lock held)
typedef struct {
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    uint32_t queue_family;
    VkQueue queue;                        // submissions take vk_queue_lock
    VkFormat format;                      // of the swapchain image, a colour attachment
    uint32_t width, height;
    int image_w, image_h;                 // the game image this present shows (its crop)
    int scaler;                           // how the last present scaled it to the window: 0 not, 1 bilinear blit, 2 FSR 1
    int picture[4];                       // where the game's picture is in the swapchain image: x, y, w, h (bars around it)
    int hdr;                              // an HDR10 swapchain: the overlay is drawn in sRGB and composed (vk_hdr_ui.c)
    struct VmaAllocator_T *vma;           // the device's allocator (a VmaAllocator), for the plugins' overlay textures
} MenuTarget;
// Lays out this frame's overlay; 1 when there is something to draw. frame: the presented frame (0 unknown). *shot: a
// path when the test hook wants this frame's swapchain image saved, else NULL. The first open starts ImGui's Vulkan
// back end and uploads the font. With the menu closed it costs two loads and a compare.
int menu_overlay_frame(uint64_t frame, const MenuTarget *t, const char **shot);
// Records the frame menu_overlay_frame laid out into cb, which is inside dynamic rendering on the image.
void menu_overlay_draw(VkCommandBuffer cb);
void menu_vk_release(void);               // before the device goes away (gfx_window_release_vk)
// (plugin/plugin_overlay.cpp) the plugins' overlay textures and the game's picture for them, on the present path.
// plugins_overlay_vk: menu_overlay_frame calls it once ImGui's Vulkan back end is up, before the layout: images and
// staging buffers for what the plugins' threads asked for since the last frame. The present then, every time:
// plugins_overlay_record(cb, serial, frames) first in its command buffer (the staged pixels into their images; what
// no frame in flight reads any more is freed. serial: the presents recorded so far; frames: the present slots in
// use, each waited for before it is recorded into again), and plugins_overlay_picture_take after the game's picture
// is in the swapchain image: 1 with the image (w x h, RGBA8) this present blits that picture into and leaves in
// SHADER_READ_ONLY_OPTIMAL. plugins_overlay_gpu: 1 while a present may sample what an earlier one wrote.
void plugins_overlay_vk(const MenuTarget *t);
void plugins_overlay_record(VkCommandBuffer cb, uint32_t serial, uint32_t frames);
int plugins_overlay_picture_take(VkImage *img, uint32_t *w, uint32_t *h);
int plugins_overlay_gpu(void);

// ---- what the menu asks of the platform side (gfx_window_sdl.c)
void gfx_window_apply_settings(void);     // MKW_FULLSCREEN or MKW_WINDOW_SIZE changed: the window follows
void gfx_window_request_close(void);      // as the window's close button: the process ends
int gfx_window_monitor_size(int *w, int *h);   // pixels of the monitor the window is on; 0 when unknown
// The present mode the swapchain got, as a MKW_VK_PRESENT value ("fifo" when mailbox is missing); "" before the first
// swapchain. Any thread.
const char *gfx_window_present_mode(void);
// (gfx/vk/device/vk_framegen.c) a fixed frame generation factor does not split the screen's refreshes evenly at the
// game's rate now, without MKW_VRR: the menu's note
int vk_fg_uneven(void);
// The notes under the upscaler, frame generation engine and Reflex rows for their values now: 0 none, 1 after a
// restart, 2 not available on this PC (gfx/vk/exec/vk_temporal.c, gfx/vk/device/vk_reflex_frame.c)
int vk_tmp_menu_state(const char *key);
int vk_rfx_menu_state(void);

#ifdef __cplusplus
}
#endif
