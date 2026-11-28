// An example of the overlay of plugin API 5: a plugin that draws with triangles and a texture of its own, takes the
// input while its page is open and shows the game's live picture in a box. It changes nothing in the game.
//
//  - A card at the top right for 8 seconds after the game is loaded: a tinted disc from the plugin's texture and two
//    lines of text, placed under the runtime's FPS line or performance panel when one is shown (MkwOverlay.perf).
//    With nothing to show the drawing is off (overlay_show(draw, 0)), which costs the game nothing.
//  - Shift+Tab on the keyboard, or L + R + D-pad down on the pad, opens a page (key_state and the pad from the
//    input_filter): the game goes on behind it and reads an idle pad (overlay_capture), the page shows the game's
//    picture (overlay_picture), what the keyboard, the mouse and the pad do, and a square under the mouse. Esc,
//    Shift+Tab or B close it.
//  - With MKW_OVERLAY_EXAMPLE_FAULT=1 in the environment, F on the page writes through a null pointer: the runtime
//    turns the drawing off, tells the plugin (overlay_on_fault) and the game goes on.
//
// A real overlay hands the draw lists of its UI library (Dear ImGui's ImDrawData, say) to MkwOverlay.triangles and its
// font atlas to texture_create; here a few quads are built by hand and the text is the runtime's (MkwOverlay.text).
// Built into an importable mod by tools/plugin/overlay.sh. It includes only mkw_plugin.h.
#include <stdio.h>
#include <string.h>

#include "mkw_plugin.h"

enum { TEX = 64, KEY_F = 9, KEY_ESC = 41, KEY_TAB = 43, KEY_LSHIFT = 225, KEY_RSHIFT = 229 };

static const MkwPluginApi *api;
static uint32_t tex;                  // TEX x TEX: opaque white in its top left 16 x 16, a soft white disc in the rest
static volatile int want_open;        // the pad filter (the game's input thread) asks; the drawing answers
static int open, fault_armed, can_fault;
static float shown_s, mouse_x, mouse_y;
static unsigned n_events, last_key;
static uint64_t pad_was;

// ---- a draw list by hand: quads into one MkwDrawList, a command per texture and clip rectangle
static MkwDrawVert vtx[256];
static uint16_t idx[384];
static MkwDrawCmd cmd[8];
static uint32_t n_vtx, n_idx, n_cmd;

static void command(const MkwOverlay *o, uint32_t texture) {
    if (n_cmd == 8) return;
    cmd[n_cmd++] = (MkwDrawCmd){{0, 0, o->width, o->height}, texture, n_vtx, n_idx, 0};
}

// rgba 0xRRGGBBAA, as MkwOverlay.rect takes it; a vertex has the bytes R, G, B, A in memory
static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t rgba) {
    if (!n_cmd || n_vtx + 4 > 256 || n_idx + 6 > 384) return;
    MkwDrawCmd *c = &cmd[n_cmd - 1];
    uint32_t col = (rgba >> 24) | (rgba >> 8 & 0xff00) | (rgba << 8 & 0xff0000) | (rgba << 24);
    uint16_t b = (uint16_t)(n_vtx - c->vtx_offset);
    vtx[n_vtx++] = (MkwDrawVert){x0, y0, u0, v0, col};
    vtx[n_vtx++] = (MkwDrawVert){x1, y0, u1, v0, col};
    vtx[n_vtx++] = (MkwDrawVert){x1, y1, u1, v1, col};
    vtx[n_vtx++] = (MkwDrawVert){x0, y1, u0, v1, col};
    const uint16_t q[6] = {b, (uint16_t)(b + 1), (uint16_t)(b + 2), b, (uint16_t)(b + 2), (uint16_t)(b + 3)};
    memcpy(idx + n_idx, q, sizeof q);
    n_idx += 6, c->idx_count += 6;
}
static void fill(float x0, float y0, float x1, float y1, uint32_t rgba) {
    quad(x0, y0, x1, y1, 4.0f / TEX, 4.0f / TEX, 5.0f / TEX, 5.0f / TEX, rgba);   // a white texel
}
static void disc(float x, float y, float r, uint32_t rgba) {
    quad(x - r, y - r, x + r, y + r, 16.0f / TEX, 16.0f / TEX, 1, 1, rgba);
}
static void flush(const MkwOverlay *o) {
    const MkwDrawList list = {vtx, idx, cmd, n_vtx, n_idx, n_cmd};
    if (n_cmd) o->triangles(&list, 1);
    n_vtx = n_idx = n_cmd = 0;
}

static void close_page(MkwOverlayFn draw) {
    open = 0;
    shown_s = 0;
    api->overlay_capture(draw, 0);   // the game reads the pad again once every button is up
    api->overlay_picture(draw, 0);
}

static void draw(const MkwOverlay *o, void *user) {
    (void)user;
    if (o->size < offsetof(MkwOverlay, perf) + sizeof o->perf) return;
    float s = o->scale;
    shown_s += o->dt;
    if (want_open && !open) {
        want_open = 0;
        open = api->overlay_capture(draw, MKW_CAPTURE_INPUT);
        if (open) api->overlay_picture(draw, 540);
    }
    for (uint32_t i = 0; open && i < o->n_events; i++) {
        const MkwInputEvent *e = (const MkwInputEvent *)((const char *)o->events + (size_t)i * o->event_size);
        n_events++;
        if (e->type == MKW_EV_MOUSE_POS || e->type == MKW_EV_MOUSE_BUTTON) mouse_x = e->x, mouse_y = e->y;
        if (e->type != MKW_EV_KEY || !e->down || e->repeat) continue;
        last_key = e->code;
        if (e->code == KEY_ESC || (e->code == KEY_TAB && (e->mods & MKW_MOD_SHIFT))) close_page(draw);
        if (e->code == KEY_F && can_fault) fault_armed = 1;
    }
    uint64_t pad = open && o->pad ? o->pad->buttons : 0;
    if (open && (pad & ~pad_was & 1u << MKW_PAD_B)) close_page(draw);
    pad_was = pad;
    if (fault_armed) {   // the fault the runtime survives: a write through a null pointer
        static int *volatile nowhere;
        fault_armed = 0;
        *nowhere = 1;
    }

    if (open) {
        float w = 1100 * s, h = 660 * s, x = (o->width - w) / 2, y = (o->height - h) / 2;
        command(o, tex);
        fill(0, 0, o->width, o->height, 0x000000a0u);
        fill(x, y, x + w, y + h, 0x1c2030f0u);
        fill(x + 30 * s - 2, y + 80 * s - 2, x + 670 * s + 2, y + 440 * s + 2, 0x000000ffu);
        if (o->picture) {   // the game, live: a command of its own with the picture as its texture
            command(o, o->picture);
            quad(x + 30 * s, y + 80 * s, x + 670 * s, y + 440 * s, 0, 0, 1, 1, 0xffffffffu);
            command(o, tex);
        }
        disc(mouse_x, mouse_y, 10 * s, 0xffd75affu);
        flush(o);
        char line[160];
        o->text(x + 30 * s, y + 24 * s, 0xffffffffu, "Overlay example (plugin API 5) - Esc, Shift+Tab or B closes");
        if (!o->picture) o->text(x + 50 * s, y + 100 * s, 0xa0a0a0ffu, "no game picture (an HDR display, or the menu is open)");
        snprintf(line, sizeof line, "game picture %d x %d   events %u   last key %u   mouse %.0f, %.0f", o->picture_w,
                 o->picture_h, n_events, last_key, (double)mouse_x, (double)mouse_y);
        o->text(x + 30 * s, y + 470 * s, 0xffffffffu, line);
        snprintf(line, sizeof line, "pad %s buttons %04x  left stick %d, %d%s", o->pad ? "" : "(none)",
                 (unsigned)pad, o->pad ? (int)o->pad->lx : 0, o->pad ? (int)o->pad->ly : 0,
                 o->flags & MKW_OV_MENU ? "   (the runtime's menu has the input)" : "");
        o->text(x + 30 * s, y + 470 * s + o->line_height * 1.4f, 0xffffffffu, line);
        if (can_fault) o->text(x + 30 * s, y + 470 * s + o->line_height * 2.8f, 0xffb0b0ffu, "F: fault on purpose");
        return;
    }
    if (shown_s > 8) {   // nothing to show: off, and the runtime lays out no frame for us
        api->overlay_show(draw, 0);
        return;
    }
    // the card: 48 from the right, 40 down, or under the runtime's own display when it is on
    float w = 520 * s, h = 96 * s, x = o->width - 48 * s - w, y = 40 * s;
    if (o->perf[3] > 0 && o->perf[3] + 12 * s > y) y = o->perf[3] + 12 * s;
    uint32_t a = shown_s < 0.2f ? (uint32_t)(shown_s / 0.2f * 255) : shown_s > 7.7f ? (uint32_t)((8 - shown_s) / 0.3f * 255) : 255;
    command(o, tex);
    fill(x, y, x + w, y + h, 0x1c203000u | (a * 230 / 255));
    disc(x + 48 * s, y + 48 * s, 30 * s, 0x5ad7ff00u | a);
    flush(o);
    o->text(x + 96 * s, y + 20 * s, 0xffffff00u | a, "Overlay example (plugin API 5)");
    o->text(x + 96 * s, y + 20 * s + o->line_height * 1.2f, 0xc0c8d800u | a, "Shift+Tab, or L + R + Down: the page");
}

// The game's input thread, at each read of player 1's pad: the shortcuts that open the page
static void filter(MkwPad *pad, void *user) {
    (void)user;
    static int was;
    if (pad->player) return;
    const uint64_t chord = 1u << MKW_PAD_L | 1u << MKW_PAD_R | 1u << MKW_PAD_DOWN;
    int now = (api->key_state(KEY_TAB) && (api->key_state(KEY_LSHIFT) || api->key_state(KEY_RSHIFT))) ||
              (pad->buttons & chord) == chord;
    if (now && !was && !open) {
        want_open = 1;
        api->overlay_show(draw, 1);
    }
    was = now;
}

static void faulted(void *user) {
    (void)user;
    open = 0;
    api->notice("Overlay example: its drawing faulted and is off; the game goes on", 6);
}

static void loaded(void *user) {
    (void)user;
    static uint8_t px[TEX * TEX * 4];
    for (int y = 0; y < TEX; y++)
        for (int x = 0; x < TEX; x++) {
            float dx = x - 39.5f, dy = y - 39.5f, d = 24 - (dx * dx + dy * dy) / 24;   // (a disc of radius 24, soft edge)
            uint8_t *p = px + 4 * (y * TEX + x);
            p[0] = p[1] = p[2] = 255;
            p[3] = x < 16 && y < 16 ? 255 : x < 16 || y < 16 ? 0 : d > 1 ? 255 : d < 0 ? 0 : (uint8_t)(d * 255);
        }
    tex = api->texture_create(TEX, TEX, px, TEX * 4, 0);
    api->overlay_show(draw, 1);
}

MKW_PLUGIN_EXPORT int mkw_plugin_init(const MkwPluginApi *a, uint32_t version, const char **message) {
    (void)version;
    // the table's size tells what the runtime has: overlay_picture is version 5's last entry
    if (a->size < offsetof(MkwPluginApi, overlay_picture) + sizeof a->overlay_picture) {
        *message = "needs a mkw-recomp with plugin API 5";
        return 1;
    }
    api = a;
    const char *f = api->cfg_get("MKW_OVERLAY_EXAMPLE_FAULT");
    can_fault = f && !strcmp(f, "1");
    api->overlay_set(draw, 0);
    api->overlay_on_fault(draw, faulted, 0);
    api->input_filter(filter, 0);
    api->on_game_loaded(loaded, 0);
    api->log("overlay example: init (API %u)", (unsigned)a->version);
    return 0;
}
