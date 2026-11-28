// The plugins' drawings over the picture (mkw_plugin.h overlay_set, overlay_show), laid out in the in-game menu's
// overlay frame (ui/menu/menu.cpp) on the present thread. Each drawing has a draw list of its own, put under every
// list of ImGui's when the frame is rendered: under the menu and its dimming, the number pad and the notices, which
// the player must always reach.
//
// Version 5: a drawing hands triangles of its own UI code (MkwOverlay.triangles) with textures of its own
// (texture_create), may show the game's own picture of the frame in a box (overlay_picture), may take the input
// (overlay_capture: the window's events are queued here and handed to its next frame, the game reads idle input,
// input.c input_plugin), and a fault inside it ends the drawing instead of the game (plat_guard_run).
//
// The GPU side never waits. A texture's pixels go into a staging buffer when the frame is laid out
// (plugins_overlay_vk) and from there into its image in the present's own command buffer, before the overlay's pass
// (plugins_overlay_record); the game's picture is blitted there too (gfx_present.c). What a frame in flight may still
// read (a staging buffer, a destroyed texture, a picture of another size) is freed once the present slot it was used
// in has come round again: gfx_present.c waits for a slot's fence before it records into it.
#include <atomic>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#include "vk_mem_alloc.h"
#pragma clang diagnostic pop

extern "C" {
#include "cfg.h"
#include "input.h"
#include "platform/platform.h"
#include "plugin/plugin.h"
#include "ui/menu/menu.h"
#include "ui/menu/text_entry.h"
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vk_queue_lock(void);
void vk_queue_unlock(void);
}
#include "ui/menu/binds_page.h"
#include "ui/menu/option_rows.h"
#include "ui/menu/settings_tabs.h"

static_assert(sizeof(MkwDrawVert) == sizeof(ImDrawVert) && offsetof(MkwDrawVert, u) == offsetof(ImDrawVert, uv) &&
              offsetof(MkwDrawVert, col) == offsetof(ImDrawVert, col), "MkwDrawVert is copied as ImDrawVert");
static_assert(sizeof(ImDrawIdx) == sizeof(uint16_t), "MkwDrawList.idx is copied as ImDrawIdx");
// The plugins' side of the ABI: these sizes never change (new fields go to the end of MkwOverlay and MkwPluginApi)
static_assert(sizeof(MkwDrawVert) == 20 && sizeof(MkwDrawCmd) == 32 && sizeof(MkwInputEvent) == 24, "plugin ABI");

enum {
    MAX_DRAWS = 16,
    MAX_TEX = PLUGIN_OVERLAY_TEXTURES,
    MAX_OLD = 128,                  // objects waiting for the frames in flight
    MAX_EVENTS = 256,
    MAX_LIST_VTX = 1 << 20,
    MAX_LIST_IDX = 3 << 20,
    PIC_SLOT = 0xff,                // the low byte of the game picture's handle (a texture's: its slot + 1)
};

static struct {
    MkwOverlayFn fn;
    void *user;
    std::atomic<int> shown;
    std::atomic<int> dead;      // it faulted: off for good
    std::atomic<int> picture;   // overlay_picture's max_height, 0 off
    void (*on_fault)(void *user);
    void *fault_user;
    int owner;                  // the plugin's slot (plugin.c), for menu_redirect
    ImDrawList *list;           // present thread: this frame's drawing
    int used;                   // present thread: list was drawn into this frame
} g_draws[MAX_DRAWS];
static std::atomic<int> g_n;       // registered; entries below it are complete
static std::atomic<int> g_shown;   // how many are on

// ---- the input (overlay_capture): one drawing at a time
static PlatMutex g_cap_lock = PLAT_MUTEX_INIT;
static std::atomic<int> g_cap{-1};     // the drawing that has the input, -1 none
static std::atomic<int> g_cap_flags;
static PlatMutex g_ev_lock = PLAT_MUTEX_INIT;
static MkwInputEvent g_ev[MAX_EVENTS];
static int g_ev_n;

static int index_of(MkwOverlayFn fn) {
    for (int i = 0, n = g_n.load(); fn && i < n; i++)
        if (g_draws[i].fn == fn) return i;
    return -1;
}

// g_cap_lock held
static void capture_set(int i, int flags) {
    int had = g_cap.load();
    if (i < 0 && had < 0) return;
    g_cap.store(i);
    g_cap_flags.store(i < 0 ? 0 : flags);
    plat_mutex_lock(&g_ev_lock);
    g_ev_n = 0;
    plat_mutex_unlock(&g_ev_lock);
    if ((i >= 0) != (had >= 0)) {
        input_plugin(i >= 0);
        rt_log("plugin: an overlay %s the input", i >= 0 ? "has" : "gave back");
    }
}

extern "C" int plugin_overlay_add(MkwOverlayFn fn, void *user, int owner) {
    int n = g_n.load();
    if (!fn || n == MAX_DRAWS || index_of(fn) >= 0) return 0;
    g_draws[n].fn = fn;
    g_draws[n].user = user;
    g_draws[n].owner = owner;
    g_n.store(n + 1);
    return 1;
}

extern "C" int plugin_overlay_owner_dead(int owner) {
    for (int i = 0, n = g_n.load(); i < n; i++)
        if (g_draws[i].owner == owner && g_draws[i].dead.load()) return 1;
    return 0;
}

extern "C" int plugin_overlay_on_fault(MkwOverlayFn fn, void (*cb)(void *user), void *user) {
    int i = index_of(fn);
    if (i < 0) return 0;
    g_draws[i].fault_user = user;
    g_draws[i].on_fault = cb;
    return 1;
}

extern "C" void plugin_overlay_show(MkwOverlayFn fn, int on) {
    int i = index_of(fn);
    if (i < 0 || (on && g_draws[i].dead.load())) return;
    bool changed = g_draws[i].shown.exchange(on != 0) != (on != 0), hid = changed && !on;
    if (changed) g_shown.fetch_add(on ? 1 : -1);
    if (!on && g_cap.load() == i) {
        plat_mutex_lock(&g_cap_lock);
        if (g_cap.load() == i) capture_set(-1, 0);
        plat_mutex_unlock(&g_cap_lock);
    }
    if (hid) opt_save_tick(1);   // API 6: a setting its page changed goes to mkw.ini now
}

extern "C" int plugin_overlay_capture(MkwOverlayFn fn, int flags) {
    int i = index_of(fn), ok = 0, released = 0;
    if (i < 0) return 0;
    plat_mutex_lock(&g_cap_lock);
    int cur = g_cap.load();
    if (!(flags & MKW_CAPTURE_INPUT)) {
        if (cur == i) capture_set(-1, 0), released = 1;
        ok = 1;
    } else if ((cur < 0 || cur == i) && g_draws[i].shown.load() && !g_draws[i].dead.load()) {
        capture_set(i, flags & (MKW_CAPTURE_INPUT | MKW_CAPTURE_TEXT));
        ok = 1;
    }
    plat_mutex_unlock(&g_cap_lock);
    if (released) opt_save_tick(1);   // API 6: a setting its page changed goes to mkw.ini now
    return ok;
}

extern "C" int plugin_overlay_picture(MkwOverlayFn fn, int max_height) {
    int i = index_of(fn);
    if (i < 0 || (max_height && (max_height < 16 || max_height > 8192))) return 0;
    g_draws[i].picture.store(max_height);
    return 1;
}

extern "C" int plugins_overlay_wanted(void) { return g_shown.load(std::memory_order_relaxed) > 0; }

// A drawing has the input and nothing of the runtime's is over it: the window's events go to it (ui/menu/menu_sdl.cpp)
extern "C" int plugins_overlay_input(void) {
    return g_cap.load(std::memory_order_relaxed) >= 0 && !menu_is_open() && !text_entry_up();
}

extern "C" int plugins_overlay_text(void) {
    return plugins_overlay_input() && (g_cap_flags.load(std::memory_order_relaxed) & MKW_CAPTURE_TEXT);
}

// Window thread. A full queue drops the oldest mouse position first, else the event: a key-up must not be lost for
// a position.
extern "C" void plugins_overlay_event(const MkwInputEvent *e) {
    if (!plugins_overlay_input()) return;
    plat_mutex_lock(&g_ev_lock);
    if (e->type == MKW_EV_MOUSE_POS && g_ev_n && g_ev[g_ev_n - 1].type == MKW_EV_MOUSE_POS) g_ev[g_ev_n - 1] = *e;
    else if (g_ev_n < MAX_EVENTS) g_ev[g_ev_n++] = *e;
    else if (e->type != MKW_EV_MOUSE_POS)
        for (int i = 0; i < g_ev_n; i++)
            if (g_ev[i].type == MKW_EV_MOUSE_POS) {
                memmove(&g_ev[i], &g_ev[i + 1], (size_t)(g_ev_n - i - 1) * sizeof *g_ev);
                g_ev[g_ev_n - 1] = *e;
                break;
            }
    plat_mutex_unlock(&g_ev_lock);
}

// ---- textures (texture_create). A plugin's thread leaves the pixels in memory; the present thread owns every Vulkan
// object.
struct Gpu {                     // what one free undoes
    VkImage img;
    VkBuffer buf;
    VmaAllocation alloc;         // of img or of buf
    VkImageView view;
    VkDescriptorSet ds;          // ImGui's texture id; gone with its back end (plugins_overlay_vk_lost)
};
struct Tex {
    uint32_t gen;                // of the handle: a freed slot's old handles name nothing
    int live;                    // the slot is taken
    int dead;                    // texture_destroy: no handle names it any more
    int zombie;                  // ... and its image waits for the frames in flight (at)
    int dirty;                   // pixels wait for the GPU
    int failed;                  // its image could not be made: it stays empty
    int uploaded;                // its image was written once (its layout is SHADER_READ_ONLY from then on)
    int w, h, flags;
    uint32_t at;                 // zombie: the present it was last recorded in at the latest
    uint8_t *pixels;             // tight rows, until staged
    Gpu g;                       // img, alloc, view, ds
    Gpu up;                      // buf, alloc: the staged pixels, copied by this present's command buffer
};
struct Old { Gpu g; uint32_t at; int stamped; };

static Tex g_tex[MAX_TEX];
static PlatMutex g_tex_lock = PLAT_MUTEX_INIT;
static std::atomic<int> g_tex_work;    // pixels to stage, or texture ids to make again
static std::atomic<int> g_tex_dead;    // a texture_destroy to carry out
static std::atomic<int> g_tex_count;   // live slots
// present thread only
static Old g_old[MAX_OLD];
static int g_old_n;
static int g_pending;                  // staged uploads + zombies + g_old_n: plugins_overlay_record has work
static struct {
    VkDevice dev;
    VmaAllocator vma;
    VkSampler linear, nearest;
    int failed;
    uint32_t serial, frames;           // the present being recorded and the present slots in use (gfx_present.c)
} V;
// The game's picture of the frame (overlay_picture): one image, written by gfx_present.c's blit each frame it is
// wanted; present thread only
static struct {
    Gpu g;
    uint32_t w, h, gen, handle;
    int copy;                          // this frame's layout may name it: the present records the blit
    int failed;
} P;

// g_tex_lock held
static Tex *tex_of(uint32_t handle) {
    uint32_t slot = (handle & 0xff) - 1;
    if (!handle || slot >= MAX_TEX) return nullptr;
    Tex *t = &g_tex[slot];
    return t->live && !t->dead && t->gen == handle >> 8 ? t : nullptr;
}

static uint8_t *tight_copy(const uint8_t *rgba, int w, int h, int row_bytes) {
    uint8_t *to = (uint8_t *)malloc((size_t)w * (size_t)h * 4);
    for (int y = 0; to && y < h; y++) memcpy(to + (size_t)y * (size_t)w * 4, rgba + (size_t)y * (size_t)row_bytes, (size_t)w * 4);
    return to;
}

extern "C" uint32_t plugin_texture_create(int w, int h, const uint8_t *rgba, int row_bytes, int flags) {
    if (!rgba || w < 1 || h < 1 || w > 8192 || h > 8192 || row_bytes < w * 4) return 0;
    uint8_t *px = tight_copy(rgba, w, h, row_bytes);
    if (!px) return 0;
    uint32_t handle = 0;
    plat_mutex_lock(&g_tex_lock);
    for (uint32_t i = 0; i < MAX_TEX && !handle; i++) {
        Tex *t = &g_tex[i];
        if (t->live) continue;
        uint32_t gen = (t->gen + 1) & 0xffffff;
        *t = Tex{};
        t->gen = gen ? gen : 1;
        t->live = t->dirty = 1, t->w = w, t->h = h, t->flags = flags & MKW_TEXTURE_NEAREST, t->pixels = px;
        handle = t->gen << 8 | (i + 1);
    }
    if (handle) {
        g_tex_count.fetch_add(1);
        g_tex_work.store(1);
    }
    plat_mutex_unlock(&g_tex_lock);
    if (!handle) free(px);
    return handle;
}

// (the copy is made outside the lock: the present thread takes it each frame)
extern "C" int plugin_texture_update(uint32_t handle, const uint8_t *rgba, int row_bytes) {
    if (!rgba) return 0;
    plat_mutex_lock(&g_tex_lock);
    Tex *t = tex_of(handle);
    int w = t ? t->w : 0, h = t ? t->h : 0;
    plat_mutex_unlock(&g_tex_lock);
    if (!t || row_bytes < w * 4) return 0;
    uint8_t *px = tight_copy(rgba, w, h, row_bytes), *old = px;
    if (!px) return 0;
    int ok = 0;
    plat_mutex_lock(&g_tex_lock);
    if ((t = tex_of(handle)) != nullptr && !t->failed) {   // (the same handle: the same size)
        old = t->pixels;
        t->pixels = px;
        t->dirty = ok = 1;
        g_tex_work.store(1);
    }
    plat_mutex_unlock(&g_tex_lock);
    free(old);
    return ok;
}

extern "C" void plugin_texture_destroy(uint32_t handle) {
    uint8_t *px = nullptr;
    plat_mutex_lock(&g_tex_lock);
    if (Tex *t = tex_of(handle)) {
        px = t->pixels;
        t->pixels = nullptr;
        t->dirty = 0;
        t->dead = 1;
        g_tex_dead.store(1);
    }
    plat_mutex_unlock(&g_tex_lock);
    free(px);
}

// ---- the present thread's side
static int g_ds_n;   // texture ids of ours in ImGui's descriptor pool (menu.cpp sizes it for PLUGIN_OVERLAY_SETS)

// ImGui's texture id for a view, 0 when the pool has no room left for one of ours (the back end does not check).
static VkDescriptorSet ds_add(VkSampler sampler, VkImageView view) {
    if (g_ds_n >= PLUGIN_OVERLAY_SETS) return VK_NULL_HANDLE;
    VkDescriptorSet ds = ImGui_ImplVulkan_AddTexture(sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (ds) g_ds_n++;
    return ds;
}

static void gpu_free(Gpu *g) {
    if (g->ds) ImGui_ImplVulkan_RemoveTexture(g->ds), g_ds_n--;
    if (g->view) vkDestroyImageView(V.dev, g->view, nullptr);
    if (g->img) vmaDestroyImage(V.vma, g->img, g->alloc);
    if (g->buf) vmaDestroyBuffer(V.vma, g->buf, g->alloc);
    *g = Gpu{};
}

// A frame in flight may still read g: it is freed when plugins_overlay_record finds its present slot come round.
// stamped 0: the present being laid out now is the first that cannot have used it (plugins_overlay_record stamps it).
static void old_add(Gpu *g, int stamped) {
    if (g_old_n == MAX_OLD) {   // (never seen: it takes a new picture size every frame) what is in flight ends, and
        vk_queue_lock();        // all go but what the command buffer being recorded reads
        vkDeviceWaitIdle(V.dev);
        vk_queue_unlock();
        int k = 0;
        for (int i = 0; i < g_old_n; i++) {
            if (g_old[i].stamped && g_old[i].at == V.serial) g_old[k++] = g_old[i];
            else gpu_free(&g_old[i].g), g_pending--;
        }
        g_old_n = k;
    }
    if (g_old_n == MAX_OLD) {   // (cannot be: a present stages 64 textures at most) left to the device's end
        *g = Gpu{};
        return;
    }
    g_old[g_old_n++] = Old{*g, V.serial, stamped};
    g_pending++;
    *g = Gpu{};
}

static int image_make(Gpu *g, uint32_t w, uint32_t h, int opaque) {
    VkImageCreateInfo ii = {};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {w, h, 1};
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(V.vma, &ii, &ai, &g->img, &g->alloc, nullptr) != VK_SUCCESS) {
        g->img = VK_NULL_HANDLE;
        return 0;
    }
    VkImageViewCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = g->img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ii.format;
    if (opaque) vi.components.a = VK_COMPONENT_SWIZZLE_ONE;   // the game's picture: whatever its alpha holds
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(V.dev, &vi, nullptr, &g->view) != VK_SUCCESS) {
        g->view = VK_NULL_HANDLE;
        gpu_free(g);
        return 0;
    }
    return 1;
}

// The texture's pixels into a staging buffer of their own, mapped; plugins_overlay_record copies it into the image.
static int stage(Tex *t) {
    VkDeviceSize bytes = (VkDeviceSize)t->w * (VkDeviceSize)t->h * 4;
    VkBufferCreateInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info = {};
    if (vmaCreateBuffer(V.vma, &bi, &ai, &t->up.buf, &t->up.alloc, &info) != VK_SUCCESS) {
        t->up.buf = VK_NULL_HANDLE;
        return 0;
    }
    if (!info.pMappedData) {
        gpu_free(&t->up);
        return 0;
    }
    memcpy(info.pMappedData, t->pixels, (size_t)bytes);
    vmaFlushAllocation(V.vma, t->up.alloc, 0, VK_WHOLE_SIZE);
    return 1;
}

// The game's picture this frame: wanted by a shown drawing, on an SDR swapchain (an HDR10 one holds PQ codes, which a
// plugin's sRGB page cannot show), at the size the drawings asked for at most. Its image is made here, before the
// layout, so the drawings get its handle; gfx_present.c fills it in this present's command buffer.
static void picture_frame(const MenuTarget *mt) {
    int want = 0;
    for (int i = 0, n = g_n.load(); i < n; i++) {
        int p = g_draws[i].picture.load(std::memory_order_relaxed);
        if (p > want && g_draws[i].shown.load(std::memory_order_relaxed) && !g_draws[i].dead.load(std::memory_order_relaxed)) want = p;
    }
    P.copy = 0;
    int pw = mt->picture[2], ph = mt->picture[3];
    if (!want || mt->hdr || pw <= 0 || ph <= 0 || V.failed) {
        if (P.g.img) old_add(&P.g, 0);
        P.failed = 0;
        return;
    }
    uint32_t h = (uint32_t)(ph < want ? ph : want), w = (uint32_t)(((int64_t)pw * h + ph / 2) / ph);
    if (w < 1) w = 1;
    if (P.g.img && (P.w != w || P.h != h)) old_add(&P.g, 0);
    if (!P.g.img) {
        if (P.failed) return;
        if (!image_make(&P.g, w, h, 1)) {
            P.failed = 1;   // (until the drawings stop asking: no line a frame)
            rt_log("plugin: no %u x %u image for the game's picture in an overlay", w, h);
            return;
        }
        P.w = w, P.h = h;
        P.gen = (P.gen + 1) & 0xffffff;
        if (!P.gen) P.gen = 1;
        P.handle = P.gen << 8 | PIC_SLOT;
    }
    if (!P.g.ds) P.g.ds = ds_add(V.linear, P.g.view);
    P.copy = P.g.ds != VK_NULL_HANDLE;
}

// Present thread, ImGui's Vulkan back end up, before the frame is laid out: the images and staging buffers of what
// the plugins' threads asked for since the last frame, and the game's picture. No queue submission, no wait.
extern "C" void plugins_overlay_vk(const MenuTarget *mt) {
    V.dev = mt->device, V.vma = mt->vma;
    if (!V.linear && !V.failed) {
        VkSamplerCreateInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        int ok = vkCreateSampler(V.dev, &si, nullptr, &V.linear) == VK_SUCCESS;
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        if (!ok || vkCreateSampler(V.dev, &si, nullptr, &V.nearest) != VK_SUCCESS) {
            V.failed = 1;
            rt_log("plugin: the overlays' textures could not be set up; they stay empty");
        }
    }
    picture_frame(mt);
    if (V.failed || !g_tex_work.exchange(0)) return;
    plat_mutex_lock(&g_tex_lock);
    for (Tex &t : g_tex) {
        if (!t.live || t.dead) continue;
        if (t.dirty && t.pixels && !t.g.img && !t.failed && !image_make(&t.g, (uint32_t)t.w, (uint32_t)t.h, 0)) {
            t.failed = 1;
            rt_log("plugin: a %d x %d overlay texture could not be made; it stays empty", t.w, t.h);
        }
        if (t.failed) {
            free(t.pixels);
            t.pixels = nullptr, t.dirty = 0;
            continue;
        }
        if (t.g.view && !t.g.ds)   // new, and every one after the back end was made again
            t.g.ds = ds_add(t.flags & MKW_TEXTURE_NEAREST ? V.nearest : V.linear, t.g.view);
        if (!t.dirty || !t.pixels || !t.g.img) continue;
        if (t.up.buf) {   // (the last frame's staging was not recorded: this one waits a frame)
            g_tex_work.store(1);
            continue;
        }
        if (stage(&t)) g_pending++;
        else rt_log("plugin: no memory to upload a %d x %d overlay texture; it stays as it was", t.w, t.h);
        free(t.pixels);
        t.pixels = nullptr, t.dirty = 0;
    }
    plat_mutex_unlock(&g_tex_lock);
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa,
                    VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = sa, b.dstAccessMask = da, b.oldLayout = from, b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// g_tex_lock held
static void slot_free(Tex *t) {
    uint32_t gen = t->gen;
    free(t->pixels);
    *t = Tex{};
    t->gen = gen;
    g_tex_count.fetch_sub(1);
}

// gfx_present.c, every present, into its command buffer before any rendering: the staged pixels into their images,
// and what the frames in flight no longer read freed. serial counts the presents recorded; the one `frames` before it
// used the same slot, whose fence the present has waited for, so everything up to that one is done on the GPU.
// With nothing pending it is two loads.
extern "C" void plugins_overlay_record(VkCommandBuffer cb, uint32_t serial, uint32_t frames) {
    V.serial = serial, V.frames = frames < 2 ? 2 : frames;
    if (!g_pending && !g_tex_dead.load(std::memory_order_relaxed)) return;
    plat_mutex_lock(&g_tex_lock);
    int dead = g_tex_dead.exchange(0);
    for (Tex &t : g_tex) {
        if (!t.live) continue;
        if (t.up.buf) {
            {   // (also for one destroyed since the layout: this frame's commands may name it)
                barrier(cb, t.g.img, t.uploaded ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, t.uploaded ? VK_ACCESS_SHADER_READ_BIT : 0,
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                        t.uploaded ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy r = {};
                r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                r.imageExtent = {(uint32_t)t.w, (uint32_t)t.h, 1};
                vkCmdCopyBufferToImage(cb, t.up.buf, t.g.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
                barrier(cb, t.g.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                t.uploaded = 1;
            }
            g_pending--;           // (the staged upload; old_add counts the buffer again)
            old_add(&t.up, 1);     // this command buffer reads it
        }
        if (dead && t.dead && !t.zombie) {
            if (t.g.img || t.g.view || t.g.ds) t.zombie = 1, t.at = serial, g_pending++;   // sampled by this present at the latest
            else slot_free(&t);
        }
        if (t.live && t.zombie && serial - t.at >= V.frames) {
            gpu_free(&t.g);
            slot_free(&t);
            g_pending--;
        }
    }
    plat_mutex_unlock(&g_tex_lock);
    int k = 0;
    for (int i = 0; i < g_old_n; i++) {
        Old &o = g_old[i];
        if (!o.stamped) o.stamped = 1, o.at = serial;
        if (serial - o.at >= V.frames) {
            gpu_free(&o.g);
            g_pending--;
        } else {
            g_old[k++] = o;
        }
    }
    g_old_n = k;
}

// gfx_present.c, after the game's picture is in the swapchain image and before the overlay's pass: 1 when this
// frame's layout may have named the picture, with the image to blit it into (left in SHADER_READ_ONLY after it).
extern "C" int plugins_overlay_picture_take(VkImage *img, uint32_t *w, uint32_t *h) {
    if (!P.copy) return 0;
    P.copy = 0;
    *img = P.g.img, *w = P.w, *h = P.h;
    return 1;
}

// Something of the plugins' is on the GPU or on its way there: gfx_present.c then keeps its presents in order when
// their queue changes (frame generation's own), since an upload in one present is sampled by the next.
extern "C" int plugins_overlay_gpu(void) {
    return g_tex_count.load(std::memory_order_relaxed) > 0 || g_pending > 0 || P.g.img != VK_NULL_HANDLE;
}

// menu.cpp, before ImGui's Vulkan back end goes (another swapchain format): its descriptor sets go with it
extern "C" void plugins_overlay_vk_lost(void) {
    plat_mutex_lock(&g_tex_lock);
    for (Tex &t : g_tex) t.g.ds = VK_NULL_HANDLE;
    g_tex_work.store(1);
    plat_mutex_unlock(&g_tex_lock);
    for (int i = 0; i < g_old_n; i++) g_old[i].g.ds = VK_NULL_HANDLE;
    P.g.ds = VK_NULL_HANDLE;
    P.copy = 0;
    g_ds_n = 0;
}

// menu.cpp menu_vk_release, before the device goes away (it is idle): nothing waits for a frame any more. The
// textures' handles stay taken and name nothing that can be drawn; the process is ending.
extern "C" void plugins_overlay_vk_release(void) {
    if (!V.dev) return;
    plugins_overlay_vk_lost();   // the back end's pool goes next
    plat_mutex_lock(&g_tex_lock);
    for (Tex &t : g_tex) {
        gpu_free(&t.g);
        gpu_free(&t.up);
        t.uploaded = 0;
        if (t.live && t.zombie) slot_free(&t);
    }
    plat_mutex_unlock(&g_tex_lock);
    for (int i = 0; i < g_old_n; i++) gpu_free(&g_old[i].g);
    g_old_n = g_pending = 0;
    gpu_free(&P.g);
    P.w = P.h = 0, P.failed = 0;
    if (V.linear) vkDestroySampler(V.dev, V.linear, nullptr);
    if (V.nearest) vkDestroySampler(V.dev, V.nearest, nullptr);
    V.dev = VK_NULL_HANDLE, V.vma = VK_NULL_HANDLE, V.linear = V.nearest = VK_NULL_HANDLE, V.failed = 0;
}

// Present thread, while a drawing is laid out: the texture id of a command's handle, 0 when it names nothing that
// this frame can sample (not uploaded yet, destroyed, the picture of another frame).
static VkDescriptorSet tex_ds(uint32_t handle) {
    if ((handle & 0xff) == PIC_SLOT) return P.copy && handle == P.handle ? P.g.ds : VK_NULL_HANDLE;
    plat_mutex_lock(&g_tex_lock);
    Tex *t = tex_of(handle);
    VkDescriptorSet ds = t && (t->uploaded || t->up.buf) ? t->g.ds : VK_NULL_HANDLE;
    plat_mutex_unlock(&g_tex_lock);
    return ds;
}

// ---- drawing
static ImDrawList *g_dl;     // the list of the drawing being called
static float g_w, g_h;
static ImU32 col(uint32_t rgba) { return IM_COL32(rgba >> 24, rgba >> 16 & 255, rgba >> 8 & 255, rgba & 255); }
static void ov_rect(float x0, float y0, float x1, float y1, uint32_t rgba, float rounding) {
    g_dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col(rgba), rounding);
}
static void ov_text(float x, float y, uint32_t rgba, const char *utf8) {
    if (utf8) g_dl->AddText(ImVec2(x, y), col(rgba), utf8);
}
static void ov_text_size(const char *utf8, float *w, float *h) {
    ImVec2 v = ImGui::CalcTextSize(utf8 ? utf8 : "");
    if (w) *w = v.x;
    if (h) *h = v.y;
}

// The plugin's lists appended to its draw list as they are: vertices and indices copied, one ImDrawCmd per command
// that stays inside its list (the copy is what is checked: the plugin's memory may change under us). Then the list's
// own state (rect, text) starts a command again after them.
static void ov_triangles(const MkwDrawList *lists, int n) {
    ImDrawList *dl = g_dl;
    if (!dl || !lists || n <= 0 || !(dl->Flags & ImDrawListFlags_AllowVtxOffset)) return;   // (the Vulkan back end has it)
    dl->_PopUnusedDrawCmd();   // rect and text drew nothing since the last command
    uint32_t last = 0;         // the texture id of the last handle looked up
    VkDescriptorSet last_ds = VK_NULL_HANDLE;
    for (int k = 0; k < n; k++) {
        const MkwDrawList l = lists[k];
        if (!l.vtx || !l.idx || !l.cmd || !l.n_vtx || !l.n_idx || !l.n_cmd || l.n_vtx > MAX_LIST_VTX || l.n_idx > MAX_LIST_IDX)
            continue;
        int v0 = dl->VtxBuffer.Size, i0 = dl->IdxBuffer.Size;
        if ((int64_t)v0 + l.n_vtx > 16 * MAX_LIST_VTX || (int64_t)i0 + l.n_idx > 16 * MAX_LIST_IDX) break;   // a frame's all
        dl->VtxBuffer.resize(v0 + (int)l.n_vtx);
        memcpy(dl->VtxBuffer.Data + v0, l.vtx, (size_t)l.n_vtx * sizeof(ImDrawVert));
        dl->IdxBuffer.resize(i0 + (int)l.n_idx);
        memcpy(dl->IdxBuffer.Data + i0, l.idx, (size_t)l.n_idx * sizeof(ImDrawIdx));
        const ImDrawIdx *idx = dl->IdxBuffer.Data + i0;
        for (uint32_t c = 0; c < l.n_cmd; c++) {
            const MkwDrawCmd m = l.cmd[c];
            if (!m.idx_count || m.idx_count % 3 || m.idx_offset > l.n_idx || m.idx_count > l.n_idx - m.idx_offset ||
                m.vtx_offset >= l.n_vtx)
                continue;
            ImDrawCmd d;
            d.ClipRect = ImVec4(m.clip[0] > 0 ? m.clip[0] : 0, m.clip[1] > 0 ? m.clip[1] : 0,
                                m.clip[2] < g_w ? m.clip[2] : g_w, m.clip[3] < g_h ? m.clip[3] : g_h);
            if (!(d.ClipRect.x < d.ClipRect.z) || !(d.ClipRect.y < d.ClipRect.w)) continue;
            if (m.texture != last) last = m.texture, last_ds = tex_ds(m.texture);
            if (!last_ds) continue;
            uint32_t room = l.n_vtx - m.vtx_offset, bad = 0;
            for (uint32_t i = 0; i < m.idx_count; i++) bad |= idx[m.idx_offset + i] >= room;
            if (bad) continue;
            d.TextureId = (ImTextureID)last_ds;
            d.VtxOffset = (unsigned)v0 + m.vtx_offset;
            d.IdxOffset = (unsigned)i0 + m.idx_offset;
            d.ElemCount = m.idx_count;
            dl->CmdBuffer.push_back(d);
        }
    }
    dl->_VtxWritePtr = dl->VtxBuffer.Data + dl->VtxBuffer.Size;
    dl->_IdxWritePtr = dl->IdxBuffer.Data + dl->IdxBuffer.Size;
    dl->_CmdHeader.VtxOffset = (unsigned)dl->VtxBuffer.Size;
    dl->_VtxCurrentIdx = 0;
    dl->AddDrawCmd();   // what rect and text draw next: after the plugin's, indexed from here
}

struct Call { int i; const MkwOverlay *o; };
static void call_draw(void *p) {
    Call *c = (Call *)p;
    g_draws[c->i].fn(c->o, g_draws[c->i].user);
}
static void call_fault(void *p) {
    int i = *(int *)p;
    g_draws[i].on_fault(g_draws[i].fault_user);
}

// MKW_OVERLAY_GUARD=0: the drawings run unguarded, as before version 5 (a fault in one is the game's crash, with its
// report): to see a plugin's fault where it happens.
static int guarded(void (*fn)(void *), void *arg, PlatCrash *why) {
    static int on = -1;
    if (on < 0) {
        const char *v = cfg_get("MKW_OVERLAY_GUARD");
        on = !(v && !strcmp(v, "0"));
    }
    if (on) return plat_guard_run(fn, arg, why);
    fn(arg);
    return 1;
}

static void faulted(int i, const PlatCrash *why) {
    g_draws[i].dead.store(1);
    plugin_overlay_show(g_draws[i].fn, 0);   // off, and its input given back
    rt_log("plugin: an overlay drawing faulted (%s at %s+%#llx, address %#llx): it is off until the game is started "
           "again; the game goes on", why->what ? why->what : "fault", why->module ? why->module : "?",
           (unsigned long long)why->offset, (unsigned long long)why->addr);
    PlatCrash again = {};
    if (g_draws[i].on_fault && !guarded(call_fault, &i, &again))
        rt_log("plugin: its on_fault callback faulted too (%s+%#llx)", again.module ? again.module : "?",
               (unsigned long long)again.offset);
}

// ---- version 6: the in-game menu's settings for a plugin's page (settings_tabs.cpp replays its tabs into a recorder)
static thread_local int t_drawing;   // inside a drawing's callback: the present thread, in the overlay's frame
static PlatMutex g_set_lock = PLAT_MUTEX_INIT;
struct SetReq { int tab; char *id, *value; };   // tab -1: settings_confirm_reset
enum { MAX_SETS = 64 };
static SetReq g_sets[MAX_SETS];
static int g_sets_n;

static int set_now(int tab, const char *id, const char *value) {
    OptSink s = {};
    s.inject_id = id;
    s.inject_value = value ? value : "";
    if (settings_tab_record(tab, &s) < 0) return -1;
    return s.result > 0 ? 0 : -1;
}

static int set_queue(int tab, const char *id, const char *value) {
    int ok = 0;
    plat_mutex_lock(&g_set_lock);
    if (g_sets_n < MAX_SETS) {
        SetReq &r = g_sets[g_sets_n];
        r.tab = tab, r.id = strdup(id), r.value = strdup(value ? value : "");
        if (r.id && r.value) g_sets_n++, ok = 1;
        else free(r.id), free(r.value);
    }
    plat_mutex_unlock(&g_set_lock);
    return ok ? 0 : -1;
}

static void sets_drain(void) {
    SetReq q[MAX_SETS];
    plat_mutex_lock(&g_set_lock);
    int n = g_sets_n;
    memcpy(q, g_sets, sizeof *q * (size_t)n);
    g_sets_n = 0;
    plat_mutex_unlock(&g_set_lock);
    for (int i = 0; i < n; i++) {
        if (q[i].tab < 0) settings_confirm_reset();
        else if (set_now(q[i].tab, q[i].id, q[i].value) < 0)
            rt_log("plugin: settings_set(%d, %s, \"%s\") refused", q[i].tab, q[i].id, q[i].value);
        free(q[i].id);
        free(q[i].value);
    }
}

extern "C" int plugin_settings_tabs(const char **names, int max) {
    int n = settings_tab_count();
    for (int i = 0; names && i < n && i < max; i++) names[i] = settings_tab_name(i);
    return n;
}

extern "C" int plugin_settings_rows(int tab, MkwSettingFn cb, void *user) {
    if (!t_drawing || !cb || opt_sinking()) return -1;
    OptSink s = {};
    s.cb = cb, s.user = user;
    return settings_tab_record(tab, &s);
}

extern "C" int plugin_settings_set(int tab, const char *id, const char *value) {
    if (!id || !*id) return -1;
    if (!t_drawing || opt_sinking()) return set_queue(tab, id, value);   // another thread, or inside a row's cb
    return set_now(tab, id, value);
}

extern "C" int plugin_settings_confirm_reset(void) {
    if (!t_drawing || opt_sinking()) return set_queue(-1, "", "");
    settings_confirm_reset();
    return 0;
}

extern "C" const char *plugin_settings_status(void) {
    return cfg_last_save_ok() ? nullptr : menu_text(UI_MENU_SAVE_FAILED);
}

// perf: where the runtime's own display at the top right is this frame (menu.cpp), zeros when off
extern "C" void plugins_overlay_draw(float width, float height, float scale, const float perf[4]) {
    if (!plugins_overlay_wanted()) return;
    sets_drain();             // settings_set from other threads (API 6)
    binds_capture_tick();     // a binding capture a page started goes on with the menu shut
    opt_save_tick(0);         // and what a page changed reaches mkw.ini with the menu shut
    ImGuiIO &io = ImGui::GetIO();
    int cap = g_cap.load();
    bool over = menu_is_open() || text_entry_up(), input = cap >= 0 && !over;
    static MkwInputEvent ev[MAX_EVENTS];
    int nev = 0;
    plat_mutex_lock(&g_ev_lock);
    if (input) memcpy(ev, g_ev, (size_t)(nev = g_ev_n) * sizeof *ev);
    g_ev_n = 0;
    plat_mutex_unlock(&g_ev_lock);
    MkwPad pad = {};
    pad.size = sizeof pad;
    int32_t st[4] = {0, 0, 0, 0};
    int guide = 0, have_pad = input && input_pad_state(&pad.buttons, st, &guide);
    pad.buttons &= 0xffff;
    pad.lx = st[0], pad.ly = st[1], pad.rx = st[2], pad.ry = st[3], pad.guide = guide;
    g_w = width, g_h = height;
    for (int i = 0, n = g_n.load(); i < n; i++) {
        if (!g_draws[i].shown.load(std::memory_order_relaxed) || g_draws[i].dead.load(std::memory_order_relaxed)) continue;
        if (!g_draws[i].list) g_draws[i].list = IM_NEW(ImDrawList)(ImGui::GetDrawListSharedData());
        ImDrawList *dl = g_dl = g_draws[i].list;
        dl->_ResetForNewFrame();
        dl->PushTextureID(io.Fonts->TexID);
        dl->PushClipRectFullScreen();
        bool mine = input && cap == i;
        bool pic = P.copy && g_draws[i].picture.load(std::memory_order_relaxed) > 0;
        MkwOverlay o = {};
        o.size = sizeof o;
        o.width = width, o.height = height, o.scale = scale, o.line_height = ImGui::GetTextLineHeight();
        o.rect = ov_rect, o.text = ov_text, o.text_size = ov_text_size;
        o.flags = (mine ? MKW_OV_INPUT : 0) | (over ? MKW_OV_MENU : 0);
        o.dt = io.DeltaTime;
        o.triangles = ov_triangles;
        o.events = mine ? ev : nullptr, o.n_events = mine ? (uint32_t)nev : 0, o.event_size = sizeof(MkwInputEvent);
        o.pad = mine && have_pad ? &pad : nullptr;
        o.picture = pic ? P.handle : 0, o.picture_w = pic ? (int)P.w : 0, o.picture_h = pic ? (int)P.h : 0;
        for (int k = 0; k < 4; k++) o.perf[k] = perf ? perf[k] : 0;
        Call c = {i, &o};
        PlatCrash why = {};
        t_drawing = 1;
        int ok = guarded(call_draw, &c, &why);
        t_drawing = 0;
        if (ok) {
            g_draws[i].used = 1;
        } else {
            opt_sink_abort();          // a fault inside a settings_rows callback: the menu's rows as they were
            dl->_ResetForNewFrame();   // whatever it had drawn is left out
            faulted(i, &why);
        }
    }
    g_dl = nullptr;
}

// After ImGui::Render: the drawings' lists go under ImGui's own, in registration order.
extern "C" void plugins_overlay_submit(void) {
    ImDrawData *dd = ImGui::GetDrawData();
    int at = 0;
    for (int i = 0, n = g_n.load(); dd && i < n; i++) {
        if (!g_draws[i].used) continue;
        g_draws[i].used = 0;
        ImDrawList *dl = g_draws[i].list;
        dl->_PopUnusedDrawCmd();
        if (!dl->CmdBuffer.Size) continue;
        dd->CmdLists.insert(dd->CmdLists.Data + at++, dl);
        dd->CmdListsCount++;
        dd->TotalVtxCount += dl->VtxBuffer.Size;
        dd->TotalIdxCount += dl->IdxBuffer.Size;
    }
}
