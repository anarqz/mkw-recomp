// SDL3 adapter of the in-game menu: the window's events to menu events (menu.h).
// Window thread (gfx_window_sdl.c's event loop, after input_sdl_event).
//
// F1 opens and closes the menu, Esc opens it (with the menu open, Esc goes to the menu, which closes; with the number
// pad up, ui/menu/text_entry.h, keys go to the pad and Esc cancels it). Esc also releases
// the cursor (input_sdl.c sees the event first; closing the menu gives the capture back). While the menu is open, its
// keys, the mouse and focus changes go to it; the game reads the keyboard through input_sdl.c and gets nothing while
// the menu is open (input_menu). Mouse positions come in window units and go to the menu in pixels.
#include <SDL3/SDL.h>

#include "imgui.h"

extern "C" {
#include "input.h"
#include "input_sdl.h"
#include "plugin/plugin.h"
#include "ui/menu/menu.h"
#include "ui/menu/text_entry.h"
}

static bool takes_input(void) { return menu_is_open() || text_entry_up(); }

static ImGuiKey key_of(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return ImGuiKey_Enter;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;   // clears a cell of the remapping rows
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_Q: return ImGuiKey_Q;   // previous tab
    case SDL_SCANCODE_E: return ImGuiKey_E;   // next tab
    default: break;
    }
    // the number pad's digits: SDL has 1-9 then 0, ImGui 0-9
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return (ImGuiKey)(ImGuiKey_1 + (sc - SDL_SCANCODE_1));
    if (sc == SDL_SCANCODE_0) return ImGuiKey_0;
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return (ImGuiKey)(ImGuiKey_Keypad1 + (sc - SDL_SCANCODE_KP_1));
    if (sc == SDL_SCANCODE_KP_0) return ImGuiKey_Keypad0;
    return ImGuiKey_None;
}

// A plugin's overlay that has the input (MkwPluginApi.overlay_capture; plugin/plugin_overlay.cpp): the event goes to
// it, as an MkwInputEvent, and not to the menu. F1 (the menu, which then has the input), F10 and what the window does
// itself (F11) stay ours; Esc is the overlay's.
static bool plugin_event(const SDL_Event *e, float d) {
    static bool text_on;   // the system's text input, on while a capturing overlay asks for it
    bool text = plugins_overlay_text();
    if (text != text_on)
        if (SDL_Window *w = SDL_GetKeyboardFocus()) {
            if (text) SDL_StartTextInput(w);
            else SDL_StopTextInput(w);
            text_on = text;
        }
    if (!plugins_overlay_input()) return false;
    MkwInputEvent ev = {};
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (e->key.scancode == SDL_SCANCODE_F1 || e->key.scancode == SDL_SCANCODE_F10) return false;
        ev.type = MKW_EV_KEY;
        ev.down = e->type == SDL_EVENT_KEY_DOWN;
        ev.repeat = e->key.repeat;
        ev.mods = (uint8_t)((e->key.mod & SDL_KMOD_SHIFT ? MKW_MOD_SHIFT : 0) | (e->key.mod & SDL_KMOD_CTRL ? MKW_MOD_CTRL : 0) |
                            (e->key.mod & SDL_KMOD_ALT ? MKW_MOD_ALT : 0));
        ev.code = (uint32_t)e->key.scancode;
        break;
    case SDL_EVENT_TEXT_INPUT:
        ev.type = MKW_EV_TEXT;
        for (const char *s = e->text.text; s && *s;) {   // one event per character
            int n = (*s & 0x80) == 0 ? 1 : (*s & 0xE0) == 0xC0 ? 2 : (*s & 0xF0) == 0xE0 ? 3 : 4, k = 0;
            for (; k < n && s[k]; k++) ev.text[k] = s[k];
            ev.text[k] = 0;
            s += k;
            plugins_overlay_event(&ev);
        }
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        ev.type = MKW_EV_MOUSE_POS;
        ev.x = e->motion.x * d, ev.y = e->motion.y * d;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        ev.type = MKW_EV_MOUSE_BUTTON;
        ev.down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        ev.code = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1
                : e->button.button == SDL_BUTTON_MIDDLE ? 2 : e->button.button == SDL_BUTTON_X1 ? 3 : 4;
        ev.x = e->button.x * d, ev.y = e->button.y * d;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        ev.type = MKW_EV_MOUSE_WHEEL;
        ev.x = e->wheel.x, ev.y = e->wheel.y;
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        ev.type = MKW_EV_FOCUS;
        ev.down = e->type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        plugins_overlay_event(&ev);
        return false;   // the menu keeps its own account of the focus
    default:
        return false;
    }
    plugins_overlay_event(&ev);
    return true;
}

extern "C" void menu_sdl_event(const SDL_Event *e) {
    float d = (float)gfx_window_sdl_density();
    if (plugin_event(e, d)) return;
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        // Esc with the menu open (opened by F1 too) is not a toggle here: it goes to the menu, which closes itself
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F1 ||
                               (e->key.scancode == SDL_SCANCODE_ESCAPE && !takes_input()))) {
            menu_toggle();
            return;
        }
        if (e->key.scancode == SDL_SCANCODE_F10) {
            if (!e->key.repeat) menu_perf_cycle();
            return;
        }
        [[fallthrough]];
    case SDL_EVENT_KEY_UP:
        if (!takes_input()) return;
        if (ImGuiKey k = key_of(e->key.scancode)) menu_event_key(k, e->type == SDL_EVENT_KEY_DOWN);
        return;
    case SDL_EVENT_MOUSE_MOTION:
        if (takes_input()) menu_event_mouse_pos(e->motion.x * d, e->motion.y * d);
        return;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!takes_input()) return;
        int b = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1
              : e->button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
        if (b < 0) return;
        menu_event_mouse_pos(e->button.x * d, e->button.y * d);
        menu_event_mouse_button(b, e->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        return;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (takes_input()) menu_event_wheel(e->wheel.x, e->wheel.y);
        return;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        menu_event_focus(e->type == SDL_EVENT_WINDOW_FOCUS_GAINED);
        return;
    }
}
