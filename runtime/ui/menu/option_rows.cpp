// Two-pane settings pages (option_rows.h): rows on the left, the selected or hovered row's description on the right.
//
// Adapted from SavageWindRecomp's runtime/ui/option_rows.mm (https://github.com/Revivedx/SavageWindRecomp @
// 575ad88e02f9a89a9a885d90039d48af33c982f4, MIT, Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors,
// Copyright (c) 2026 Peiikai): the row look, the two panes, the left / right stepping and the hold speed-up. The rows
// read and write cfg keys instead of a settings struct, take their text from the menu catalog (menu_text), and show
// the default, "after restart" and environment notes; the text field, radio and on-screen keyboard rows are not ported.
#include <atomic>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "imgui_internal.h"   // SetKeyOwner, SetFocusID, NavCursorVisible

#include "ui/menu/option_rows.h"
#include "ui/glyphs/pad_icons.h"

extern "C" {
#include "cfg.h"
#include "input.h"
#include "platform/platform.h"
#include "ui_text.h"
// host.h pulls in cpu.h, which is C only: the one function needed from it, as declared there.
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}

// The in-game menu's colours: gold for the selection and the headings on dark green (menu_look.cpp).
static const OptLook kMenuLook = {
    IM_COL32(236, 194, 84, 255), IM_COL32(236, 194, 84, 120), IM_COL32(236, 194, 84, 110),   // select, select_off, divider
    IM_COL32(150, 150, 140, 255), IM_COL32(110, 110, 102, 255), IM_COL32(170, 170, 160, 255),   // dim, off, arrow
    IM_COL32(240, 236, 224, 255), IM_COL32(255, 255, 255, 255), IM_COL32(205, 205, 195, 255),   // label, value, default
    IM_COL32(220, 170, 100, 255), IM_COL32(200, 150, 90, 255),                                    // note, note_off
    IM_COL32(0, 0, 0, 40), IM_COL32(255, 255, 255, 22),                                            // fill, fill_hot
    IM_COL32(255, 255, 255, 38), IM_COL32(255, 255, 255, 18), IM_COL32(255, 255, 255, 14),        // border, _off, cell_off
    IM_COL32(237, 194, 84, 255), IM_COL32(242, 168, 102, 255),                                    // group, warn
    9,                                                                                             // radius
    false, 0,                                                                                      // quiet, changed
};
static const OptLook *g_look = &kMenuLook;

void opt_look(const OptLook *l) { g_look = l ? l : &kMenuLook; }

static float g_scale = 1;
static bool g_focus_req, g_take_first;   // opt_focus_first: requested / this page's first row takes it
static char g_focus_key[64];             // opt_focus_key: the row that takes the cursor when it is next drawn
static bool g_held;                      // the last row's left / right has been held a while (steppers go faster)
static float g_bottom;                   // opt_reserve_bottom: kept free under the panes
static bool g_disable_next;              // opt_disable_next: the next row is greyed out with g_disable_note
static UiTextId g_disable_note;
static bool g_indent;                    // opt_indent
// The mouse moved after the last navigation key: the row under it is the one highlighted and described, else the
// cursor's. One source, so the pane never describes a row other than the one drawn selected.
static bool g_mouse;
static bool g_hide_desc;   // opt_hide_desc: a bottom button has the cursor, the pane shows nothing

// The description pane: this frame's hovered and selected rows, the page's first row, and the last one shown.
struct Desc { char text[512]; char note[512]; };   // the Steam note is 346 bytes in Thai (#579)
static Desc g_hover, g_focus, g_first, g_shown;

// disabled: the note of a row opt_disable_next greyed out, else NULL. note: a dim line under the description,
// for a row whose value needs a word (NULL: none).
static void desc_set(Desc &d, UiTextId desc, const char *key, const CfgMeta *m, const char *disabled,
                     const char *note = nullptr) {
    int n = snprintf(d.text, sizeof d.text, "%s", menu_text(desc));
    if (n < 0 || n >= (int)sizeof d.text) return;
    if (disabled)
        snprintf(d.text + n, sizeof d.text - n, "\n\n%s", disabled);
    else if (cfg_from_env(key))
        snprintf(d.text + n, sizeof d.text - n, "\n\n%s %s", menu_text(UI_MENU_SET_BY_ENV), key);
    else if (m && !m->live && !g_look->quiet)
        snprintf(d.text + n, sizeof d.text - n, "\n\n%s", menu_text(UI_MENU_AFTER_RESTART));
    snprintf(d.note, sizeof d.note, "%s", note ? note : "");
}

void opt_focus_first(void) { g_focus_req = true; }

void opt_focus_key(const char *key) { snprintf(g_focus_key, sizeof g_focus_key, "%s", key); }

void opt_reserve_bottom(float h) { g_bottom = h; }

void opt_disable_next(UiTextId note) {
    g_disable_next = true;
    g_disable_note = note;
}

void opt_indent(bool on) { g_indent = on; }

void opt_hide_desc(bool hide) { g_hide_desc = hide; }

static OptSink *g_sink;   // the recorder (below): no ImGui while it is set
static void sink_flush(void);

void opt_begin(const char *id, float scale) {
    if (g_sink) {
        g_indent = false;
        return;
    }
    g_scale = scale;
    g_take_first = g_focus_req;
    g_focus_req = false;
    if (g_take_first) ImGui::NavMoveRequestCancel();   // this frame's move goes to the first row instead
    g_hover.text[0] = g_focus.text[0] = g_first.text[0] = 0;
    // A tab or page left behind with the mouse keeps g_mouse set, so on this one g_hover stays empty and opt_end would
    // keep the pane describing the rows of the one before; the pane starts again with this page's own rows.
    static char g_page[64] = "";
    if (strcmp(g_page, id)) {
        g_shown.text[0] = 0;
        snprintf(g_page, sizeof g_page, "%s", id);
    }
    g_indent = false;
    static const ImGuiKey kNav[] = {ImGuiKey_UpArrow,         ImGuiKey_DownArrow,        ImGuiKey_LeftArrow,
                                    ImGuiKey_RightArrow,      ImGuiKey_Tab,              ImGuiKey_GamepadDpadUp,
                                    ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight};
    const ImGuiIO &io = ImGui::GetIO();
    if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0) g_mouse = true;
    for (ImGuiKey k : kNav)
        if (ImGui::IsKeyPressed(k, false)) g_mouse = false;
    if (g_take_first) g_mouse = false;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.y -= g_bottom;
    ImGui::PushID(id);
    ImGui::BeginChild("##rows", ImVec2(avail.x * 0.58f, avail.y), ImGuiChildFlags_NavFlattened);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 5 * scale));
}

// opt_test_scroll (test hooks of the menu and the start screen): the pages scroll to that fraction of their height
static float g_test_scroll = -1;
void opt_test_scroll(float f) { g_test_scroll = f; }

void opt_end(void) {
    if (g_sink) {
        sink_flush();
        return;
    }
    ImGui::PopStyleVar();
    if (g_test_scroll >= 0) ImGui::SetScrollY(ImGui::GetScrollMaxY() * g_test_scroll);
    ImGui::EndChild();
    // the row the player used last (g_mouse), else the last shown, else the first row; nothing while a bottom
    // button ("Continue", "Quit game") has the cursor and no row is hovered or selected
    const Desc &used = g_mouse ? g_hover : g_focus;
    if (used.text[0]) g_shown = used;
    else if (g_hide_desc) g_shown.text[0] = g_shown.note[0] = 0;
    else if (!g_shown.text[0]) g_shown = g_first;
    ImGui::SameLine(0, 0);
    ImVec2 p = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    avail.y -= g_bottom;
    float gap = 24 * g_scale;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + gap * 0.5f, p.y), ImVec2(p.x + gap * 0.5f, p.y + avail.y),
                                        g_look->divider, 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(p.x + gap, p.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * g_scale, 6 * g_scale));
    ImGui::BeginChild("##desc", ImVec2(0, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(g_shown.text);
    if (g_shown.note[0]) {
        ImGui::Spacing();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(g_look->dim), "%s", g_shown.note);
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    ImGui::PopID();
}

// What a row shows besides its label.
struct RowView {
    const char *value;   // right side; "" for none
    bool is_default;     // the value is the setting's default: the "default" mark, the value not highlighted
    bool arrows;         // a stepper: < value >, the arrows click too
    const char *note;    // under the value instead of the "default" mark; NULL for none
    const char *desc_note = nullptr;   // a dim line under the description; NULL for none
};

// One row: draws it, handles A / click and left / right while selected. Returns 2 (A / click), -1 / +1 (left / right) or
// 0; always 0 for a row the environment fixes (greyed out, still selectable so its description shows).
static int row_text(const char *key, const char *label, UiTextId desc, const RowView &v) {
    const CfgMeta *m = cfg_meta(key);
    const char *disabled = g_disable_next ? menu_text(g_disable_note) : NULL;
    g_disable_next = false;
    bool off = disabled || cfg_from_env(key) != 0;
    ImGui::PushID(key);
    if (g_indent) ImGui::Indent(24 * g_scale);
    ImVec2 p = ImGui::GetCursorScreenPos();
    // h whole pixels: ImGui truncates cursor positions, which rounds a fractional height the other way above the window's
    // top (negative y), so the page grew a pixel per row scrolled out and a row scrolled to the bottom was cut off
    float fs = ImGui::GetFontSize(), w = ImGui::GetContentRegionAvail().x, h = floorf(fs * 2.2f);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));   // the row's own border shows the selection
    bool clicked = ImGui::InvisibleButton("##row", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    bool to_key = g_focus_key[0] && !strcmp(key, g_focus_key);
    if (g_take_first || to_key) {
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true);
        if (g_take_first) ImGui::SetScrollY(0);   // the page's top: text and a heading above the first row show
        else ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);   // as a cursor move would (the last row whole)
        g_take_first = false;
        if (to_key) g_focus_key[0] = 0;
    }
    // foc: the cursor is on it (left / right step it); sel: drawn and described as selected, the mouse's row when the
    // mouse was used last, else the cursor's
    bool foc = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
    bool hov = g_mouse && ImGui::IsItemHovered(), sel = hov || (foc && !g_mouse);
    int step = clicked && !off ? 2 : 0;
    g_held = false;
    if (foc && !off) {
        // the left stick arrives as the D-pad (menu.cpp pad_events)
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) step = 1;
        static const ImGuiKey steps[] = {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight, ImGuiKey_LeftArrow,
                                         ImGuiKey_RightArrow};
        for (ImGuiKey k : steps) {
            if (ImGui::GetKeyData(k)->DownDuration > 1.0f) g_held = true;
            // left and right belong to the row (navigation would otherwise move sideways into the description pane)
            ImGui::SetKeyOwner(k, ImGui::GetItemID());
        }
    }
    if (foc && !g_mouse) desc_set(g_focus, desc, key, m, disabled, v.desc_note);
    if (hov) desc_set(g_hover, desc, key, m, disabled, v.desc_note);
    if (!g_first.text[0]) desc_set(g_first, desc, key, m, disabled, v.desc_note);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = ImGui::GetFont();
    float r = g_look->radius * g_scale, pad = 16 * g_scale, small = fs * 0.72f;
    if (g_indent)   // the line the indented rows hang from
        dl->AddLine(ImVec2(p.x - 12 * g_scale, p.y - 5 * g_scale), ImVec2(p.x - 12 * g_scale, p.y + h), g_look->border,
                    1.0f);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), sel ? g_look->fill_hot : g_look->fill, r);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h),
                sel ? (off ? g_look->select_off : g_look->select) : off ? g_look->border_off : g_look->border, r, 0,
                sel ? 2.0f : 1.0f);
    float ty = p.y + fs * 0.25f, ny = p.y + fs * 1.3f;   // the label line and the note line under it
    float right = p.x + w - pad;                         // the value's right edge
    float value_left = right;
    if (v.arrows && v.value[0]) {   // < value >, drawn (the font has no arrow glyphs); a click on one steps
        float aw = fs * 0.42f, gap = fs * 0.55f, cy = ty + fs * 0.52f;
        float rx = right - aw, lx = rx - gap - ImGui::CalcTextSize(v.value).x - gap - aw;
        ImU32 ac = off ? g_look->off : sel ? g_look->select : g_look->arrow;
        dl->AddTriangleFilled(ImVec2(lx, cy), ImVec2(lx + aw, cy - aw * 0.75f), ImVec2(lx + aw, cy + aw * 0.75f), ac);
        dl->AddTriangleFilled(ImVec2(rx + aw, cy), ImVec2(rx, cy - aw * 0.75f), ImVec2(rx, cy + aw * 0.75f), ac);
        if (clicked && !off) {
            float mx = ImGui::GetIO().MousePos.x;
            if (mx >= lx - gap && mx <= lx + aw + gap) step = -1;
            else if (mx >= rx - gap && mx <= rx + aw + gap) step = 1;
        }
        right = rx - gap;
        value_left = lx;
    }
    if (v.value[0]) {
        float vx = right - ImGui::CalcTextSize(v.value).x;
        dl->AddText(ImVec2(vx, ty), off ? g_look->off : v.is_default ? g_look->value_default : g_look->value,
                    v.value);
        if (!v.arrows) value_left = vx;
    }
    if (v.note || (v.is_default && !g_look->quiet)) {   // under the value, right-aligned with it
        const char *t = v.note ? v.note : menu_text(UI_MENU_DEFAULT_MARK);
        float tw = font->CalcTextSizeA(small, FLT_MAX, 0, t).x;
        dl->AddText(font, small, ImVec2(right - tw, ny), v.note ? g_look->note : g_look->dim, t);
    }
    // the label and its note, clipped before the value
    ImVec4 clip(p.x, p.y, value_left - fs * 0.6f, p.y + h);
    dl->AddText(font, fs, ImVec2(p.x + pad, ty), off ? g_look->off : g_look->label, label, NULL, 0,
                &clip);
    const char *note = disabled                          ? disabled
                     : off                               ? menu_text(UI_MENU_SET_BY_ENV)
                     : m && !m->live && !g_look->quiet ? menu_text(UI_MENU_AFTER_RESTART)
                                                         : NULL;
    if (note) dl->AddText(font, small, ImVec2(p.x + pad, ny), off ? g_look->note_off : g_look->dim, note);
    if (g_look->quiet && !off && v.value[0] && !v.is_default) {   // changed from the default: a dot after the label
        float dx = p.x + pad + ImGui::CalcTextSize(label).x + fs * 0.5f, dr = fs * 0.14f;
        if (dx + dr < value_left - fs * 0.6f) dl->AddCircleFilled(ImVec2(dx, ty + fs * 0.55f), dr, g_look->changed, 12);
    }
    if (g_indent) ImGui::Unindent(24 * g_scale);
    ImGui::PopID();
    return off ? 0 : step;
}

// ---- writing a setting: at once in memory (live settings follow), to mkw.ini a moment later (opt_save_tick)
// g_save_at is atomic: a plugin's overlay flushes the write from the thread that hides it (plugin_overlay.cpp)
static std::atomic<uint64_t> g_save_at;   // plat_now_ns deadline of the pending write; 0 none
static std::atomic<bool> g_save_logged;

static void apply(const char *key, const char *value) {
    if (*value) {
        cfg_set(key, value);
    } else {   // the automatic choice: the key unset, its line removed from mkw.ini
        const char *k = key;
        cfg_reset(&k, 1);
    }
    opt_save_soon();
}

void opt_save_soon(void) { g_save_at.store(plat_now_ns() + 400000000ull); }

int opt_save_tick(int force) {
    uint64_t at = g_save_at.load();
    if (!at || (!force && plat_now_ns() < at)) return at != 0;
    if (!g_save_at.compare_exchange_strong(at, 0)) return 1;   // changed meanwhile: the newer deadline holds
    if (!cfg_save() && !g_save_logged.exchange(true))
        rt_log("menu: could not write mkw.ini; the changed settings hold for this session");
    return 0;
}

// ---- the recorder (option_rows.h OptSink): rows reported as MkwSettingRow instead of drawn
static struct {   // the menu's per-frame state a pass may touch, put back by opt_sink_end
    bool disable_next, indent, held;
    UiTextId disable_note;
} g_sink_saved;
static int g_bind_cap = -1;   // opt_bind_capture

// A reported row is held until the next one (a paragraph right after it becomes its note), with its strings and lists
// copied: the tab's own arrays may be gone by then.
static MkwSettingRow g_pend;
static bool g_have_pend;
static char g_arena[65536];
static size_t g_arena_n;
enum { SINK_CHOICES = 64 };
static const char *g_pv[SINK_CHOICES], *g_pn[SINK_CHOICES];

static const char *adup(const char *s) {
    if (!s) return nullptr;
    size_t n = strlen(s) + 1;
    if (g_arena_n + n > sizeof g_arena) return "";
    char *d = g_arena + g_arena_n;
    memcpy(d, s, n);
    g_arena_n += n;
    return d;
}

static void sink_flush(void) {
    if (g_have_pend && g_sink && g_sink->cb) g_sink->cb(&g_pend, g_sink->user);
    g_have_pend = false;
    g_arena_n = 0;
}

static void sink_emit(const MkwSettingRow &r) {
    sink_flush();
    g_pend = r;
    g_pend.id = adup(r.id);
    g_pend.label = adup(r.label ? r.label : "");
    g_pend.desc = adup(r.desc);
    g_pend.note = r.note && *r.note ? adup(r.note) : nullptr;
    g_pend.value = adup(r.value ? r.value : "");
    g_pend.value_text = adup(r.value_text ? r.value_text : "");
    g_pend.fmt = adup(r.fmt);
    for (int c = 0; c < 3; c++) g_pend.cells[c] = adup(r.cells[c]);
    int n = r.n_choices < 0 ? 0 : r.n_choices > SINK_CHOICES ? SINK_CHOICES : r.n_choices;
    for (int i = 0; i < n; i++) g_pv[i] = adup(r.choice_values[i]), g_pn[i] = adup(r.choice_names[i]);
    g_pend.n_choices = n;
    g_pend.choice_values = n ? g_pv : nullptr;
    g_pend.choice_names = n ? g_pn : nullptr;
    if (g_pend.current >= n && r.kind == MKW_SET_CHOICE) g_pend.current = -1;
    if (g_pend.def >= n && r.kind == MKW_SET_CHOICE) g_pend.def = -1;
    g_have_pend = true;
}

static void sink_blank(MkwSettingRow &r, uint32_t kind) {
    memset(&r, 0, sizeof r);
    r.size = sizeof r;
    r.kind = kind;
    r.flags = g_sink->flags;
    r.capture_cell = -1;
    r.current = r.def = -1;
    r.value = r.value_text = "";
}

static void sink_text(const char *t, bool warn) {
    if (g_have_pend && g_pend.kind != MKW_SET_GROUP && g_pend.kind != MKW_SET_TEXT) {   // the row's note
        if (g_pend.note) {
            char both[1200];
            snprintf(both, sizeof both, "%s\n%s", g_pend.note, t);
            g_pend.note = adup(both);
        } else {
            g_pend.note = adup(t);
        }
        if (warn) g_pend.flags |= MKW_SETF_WARNING;
        return;
    }
    MkwSettingRow r;
    sink_blank(r, MKW_SET_TEXT);
    if (warn) r.flags |= MKW_SETF_WARNING;
    r.label = t;
    sink_emit(r);
}

// The parts every setting row has; takes opt_disable_next as row_text does. Returns true when the row is greyed out.
static bool sink_fill(MkwSettingRow &r, uint32_t kind, const char *key, const char *label, UiTextId desc,
                      const char *desc_note, const char *value_note, bool is_default) {
    static char d[1024], n[256];
    const CfgMeta *m = cfg_meta(key);
    const char *disabled = g_disable_next ? menu_text(g_disable_note) : NULL;
    g_disable_next = false;
    bool env = cfg_from_env(key) != 0, off = disabled || env;
    sink_blank(r, kind);
    r.flags |= (off ? MKW_SETF_DISABLED : 0) | (env ? MKW_SETF_ENV : 0) | (m && !m->live ? MKW_SETF_RESTART : 0) |
               (is_default ? MKW_SETF_DEFAULT : 0) | (g_indent ? MKW_SETF_INDENT : 0);
    r.id = key;
    r.label = label;
    snprintf(d, sizeof d, "%s%s%s", menu_text(desc), desc_note ? "\n\n" : "", desc_note ? desc_note : "");
    r.desc = d;
    if (value_note) {
        r.note = value_note;
    } else if (disabled) {
        r.note = disabled;
    } else if (env) {
        snprintf(n, sizeof n, "%s %s", menu_text(UI_MENU_SET_BY_ENV), key);
        r.note = n;
    }
    return off;
}

// The injected value when this row is the one it is for; a greyed-out row refuses it.
static const char *sink_take(const char *id, bool off) {
    if (!g_sink->inject_id || g_sink->result || !id || strcmp(g_sink->inject_id, id)) return nullptr;
    if (off) {
        g_sink->result = -1;
        return nullptr;
    }
    g_sink->result = 2;
    return g_sink->inject_value ? g_sink->inject_value : "";
}

static bool sink_changed(void) { return g_sink->result = 1, true; }
static bool sink_bad(void) { return g_sink->result = -1, false; }

void opt_sink_begin(OptSink *s) {
    g_sink_saved = {g_disable_next, g_indent, g_held, g_disable_note};
    g_disable_next = g_indent = g_held = false;
    g_have_pend = false;
    g_arena_n = 0;
    g_bind_cap = -1;
    s->result = 0;
    g_sink = s;
}

void opt_sink_end(void) {
    if (!g_sink) return;
    sink_flush();
    g_sink = nullptr;
    g_disable_next = g_sink_saved.disable_next, g_indent = g_sink_saved.indent, g_held = g_sink_saved.held;
    g_disable_note = g_sink_saved.disable_note;
}

void opt_sink_abort(void) {
    if (!g_sink) return;
    g_have_pend = false;   // nothing more is reported to a callback that faulted
    opt_sink_end();
}

bool opt_sinking(void) { return g_sink != nullptr; }

void opt_sink_refuse(void) {
    if (g_sink && g_sink->result > 0) g_sink->result = -1;
}

void opt_bind_capture(int cell) { g_bind_cap = cell; }

// ---- rows

static int row(const char *key, UiTextId label, UiTextId desc, const RowView &v) {
    return row_text(key, menu_text(label), desc, v);
}

bool opt_toggle(const char *key, UiTextId label, UiTextId desc, bool inverted) {
    const CfgMeta *m = cfg_meta(key);
    bool set = cfg_bool(key) != 0, def = m && strcmp(m->def, "0") != 0;
    bool on = set != inverted;   // what the row shows
    RowView v = {menu_text(on ? UI_MENU_ON : UI_MENU_OFF), set == def, false, NULL};
    if (g_sink) {
        MkwSettingRow r;
        bool off = sink_fill(r, MKW_SET_TOGGLE, key, menu_text(label), desc, nullptr, nullptr, set == def);
        r.value = on ? "1" : "0";
        r.value_text = v.value;
        sink_emit(r);
        const char *in = sink_take(key, off);
        if (!in) return false;
        if (strcmp(in, "0") && strcmp(in, "1")) return sink_bad();
        if ((*in == '1') == on) return false;
        apply(key, set ? "0" : "1");
        return sink_changed();
    }
    if (!row(key, label, desc, v)) return false;   // any step flips it
    apply(key, set ? "0" : "1");
    return true;
}

void opt_info(const char *id, const char *label, const char *value, UiTextId desc, const char *note) {
    RowView v = {value ? value : "", false, false, note};
    if (g_sink) {
        MkwSettingRow r;
        bool off = sink_fill(r, MKW_SET_INFO, id, label, desc, nullptr, note, false);
        r.value = r.value_text = v.value;
        sink_emit(r);
        if (sink_take(id, off)) sink_bad();   // nothing to change
        return;
    }
    row_text(id, label, desc, v);
}

// A button in a pass: "press" runs it
static bool sink_button(const char *id, const char *label, UiTextId desc, const char *value) {
    MkwSettingRow r;
    bool off = sink_fill(r, MKW_SET_BUTTON, id, label, desc, nullptr, nullptr, false);
    r.value_text = value ? value : "";
    sink_emit(r);
    const char *in = sink_take(id, off);
    if (!in) return false;
    return strcmp(in, "press") ? sink_bad() : sink_changed();
}

bool opt_button(const char *id, UiTextId label, UiTextId desc, const char *value) {
    if (g_sink) return sink_button(id, menu_text(label), desc, value);
    RowView v = {value ? value : "", false, false, NULL};
    return row(id, label, desc, v) == 2;   // A or a click; left and right do nothing
}

bool opt_start_screen(void) { return g_look->quiet; }

bool opt_button_text(const char *id, const char *label, UiTextId desc, const char *value) {
    if (g_sink) return sink_button(id, label, desc, value);
    RowView v = {value ? value : "", false, false, NULL};
    return row_text(id, label, desc, v) == 2;
}

void opt_group(UiTextId title) {
    if (g_sink) {
        MkwSettingRow r;
        sink_blank(r, MKW_SET_GROUP);
        r.label = menu_text(title);
        sink_emit(r);
        return;
    }
    ImGui::Dummy(ImVec2(0, 6 * g_scale));
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(g_look->group), "%s", menu_text(title));
}

void opt_text(UiTextId text, bool warn) { opt_text(menu_text(text), warn); }

void opt_text(const char *text, bool warn) {
    if (g_sink) {
        sink_text(text, warn);
        return;
    }
    ImGui::PushTextWrapPos(0);
    if (warn) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(g_look->warn), "%s", text);
    else ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
}

bool opt_choice_list(const char *key, UiTextId label, const char *const *values, const char *const *names, int n,
                     UiTextId desc, int cur, const char *note, const char *desc_note) {
    const CfgMeta *m = cfg_meta(key);
    const char *now = cfg_value(key);
    if (cur < 0)
        for (int i = 0; i < n; i++)
            if (!strcmp(values[i], now)) cur = i;
    int def = -1;
    for (int i = 0; m && i < n; i++)
        if (!strcmp(values[i], m->def)) def = i;
    RowView v = {cur >= 0 ? names[cur] : now, cur == def, true, note, desc_note};
    if (g_sink) {
        MkwSettingRow r;
        bool off = sink_fill(r, MKW_SET_CHOICE, key, menu_text(label), desc, desc_note, note, cur == def);
        r.value = now;
        r.value_text = v.value;
        r.n_choices = n, r.current = cur, r.def = def;
        r.choice_values = values, r.choice_names = names;
        sink_emit(r);
        const char *in = sink_take(key, off);
        if (!in) return false;
        int nv = -1;
        for (int i = 0; i < n && nv < 0; i++)
            if (!strcmp(values[i], in)) nv = i;
        if (nv < 0) return sink_bad();
        if (nv == cur) return false;
        apply(key, values[nv]);
        return sink_changed();
    }
    int step = row(key, label, desc, v);
    if (!step || n <= 0) return false;
    int nv;
    if (cur < 0) nv = 0;
    else if (step == 2) nv = (cur + 1) % n;               // A / click: the next value, round to the first
    else nv = cur + step < 0 ? 0 : cur + step >= n ? n - 1 : cur + step;   // left / right stop at the ends
    if (nv == cur) return false;
    apply(key, values[nv]);
    return true;
}

bool opt_choice(const char *key, UiTextId label, const UiTextId *names, int n, UiTextId desc, const char *note,
                const char *desc_note) {
    enum { MAX = 32 };
    const CfgMeta *m = cfg_meta(key);
    const char *values[MAX], *shown[MAX];
    char buf[256];
    int k = 0;
    if (m && !m->def[0]) values[k++] = "";   // the automatic choice
    snprintf(buf, sizeof buf, "%s", m && m->values ? m->values : "");
    for (char *s = buf, *e; *s && k < MAX; s = e) {
        e = strchr(s, '|');
        if (e) *e++ = 0;
        else e = s + strlen(s);
        values[k++] = s;
    }
    if (n > k) n = k;
    for (int i = 0; i < n; i++) shown[i] = menu_text(names[i]);
    return opt_choice_list(key, label, values, shown, n, desc, -1, note, desc_note);
}

bool opt_slider(const char *key, UiTextId label, float lo, float hi, UiTextId desc, float step, const char *fmt) {
    const CfgMeta *m = cfg_meta(key);
    double v = cfg_double(key), def = m ? strtod(m->def, NULL) : lo;
    if (!strcmp(key, "MKW_STICK_DEADZONE")) {   // unset: the open gamepad's own default (an Xbox pad's is not 8 %)
        const char *set = cfg_get(key);
        double d = input_stick_deadzone_default();
        if (!(set && *set) && d >= 0) v = def = d;
    }
    char shown[48];
    snprintf(shown, sizeof shown, fmt, v);
    RowView rv = {shown, fabs(v - def) < step * 0.01, true, NULL};
    if (g_sink) {
        MkwSettingRow r;
        bool off = sink_fill(r, MKW_SET_SLIDER, key, menu_text(label), desc, nullptr, nullptr, rv.is_default);
        r.value = cfg_value(key);
        r.value_text = shown;
        r.num = v, r.lo = lo, r.hi = hi, r.step = step, r.auto_start = lo;
        r.fmt = fmt;
        sink_emit(r);
        const char *in = sink_take(key, off);
        if (!in) return false;
        char *end;
        double x = strtod(in, &end);
        if (!*in || *end || !isfinite(x)) return sink_bad();
        double n = round(x / step) * step;
        n = n < lo ? lo : n > hi ? hi : n;
        if (fabs(n - v) < step * 0.01) return false;
        char out[32];
        if (step >= 1) snprintf(out, sizeof out, "%d", (int)lround(n));
        else snprintf(out, sizeof out, "%g", round(n / step) * step);
        apply(key, out);
        return sink_changed();
    }
    int s = row(key, label, desc, rv);
    if (!s) return false;
    int steps = g_held && (hi - lo) > 40 * step ? 10 : 1;   // held: ten steps at a time over a long range
    double n = round(v / step) * step + (s == 2 ? step : s * step * steps);
    if (s == 2 && n > hi + step * 0.01) n = lo;   // A / click past the top goes round; left / right stop at the ends
    n = n < lo ? lo : n > hi ? hi : n;
    if (fabs(n - v) < step * 0.01) return false;
    char out[32];
    if (step >= 1) snprintf(out, sizeof out, "%d", (int)lround(n));
    else snprintf(out, sizeof out, "%g", round(n / step) * step);
    apply(key, out);
    return true;
}

bool opt_slider_auto(const char *key, UiTextId label, int lo, int hi, UiTextId desc, const char *fmt,
                     const char *auto_name, int start) {
    const char *cur = cfg_value(key);
    int v = atoi(cur), is_auto = v <= 0;   // "auto" (or nothing) reads as 0
    char shown[48];
    if (is_auto) snprintf(shown, sizeof shown, "%s", auto_name);
    else snprintf(shown, sizeof shown, fmt, v);
    RowView rv = {shown, is_auto != 0, true, NULL};
    if (g_sink) {
        MkwSettingRow r;
        bool off = sink_fill(r, MKW_SET_SLIDER, key, menu_text(label), desc, nullptr, nullptr, is_auto != 0);
        r.flags |= MKW_SETF_AUTO;
        char num[16], dfmt[48];
        snprintf(num, sizeof num, "%d", v);
        r.value = is_auto ? "" : num;
        r.value_text = shown;
        r.num = is_auto ? lo - 1 : v, r.lo = lo, r.hi = hi, r.step = 1;
        r.auto_start = start < lo ? lo : start > hi ? hi : start;
        // fmt takes an int here ("%d nits"); the row's takes a double
        const char *d = strstr(fmt, "%d");
        if (d) snprintf(dfmt, sizeof dfmt, "%.*s%%.0f%s", (int)(d - fmt), fmt, d + 2);
        else snprintf(dfmt, sizeof dfmt, "%%.0f");
        r.fmt = dfmt;
        sink_emit(r);
        const char *in = sink_take(key, off);
        if (!in) return false;
        int n;
        if (!*in || !strcmp(in, "auto")) {
            n = 0;
        } else {
            char *end;
            double x = strtod(in, &end);
            if (*end || !isfinite(x)) return sink_bad();
            n = (int)lround(x < lo ? lo : x > hi ? hi : x);
        }
        if (n == (is_auto ? 0 : v)) return false;
        char out[16];
        if (n <= 0) snprintf(out, sizeof out, "auto");
        else snprintf(out, sizeof out, "%d", n);
        apply(key, out);
        return sink_changed();
    }
    int s = row(key, label, desc, rv);
    if (!s) return false;
    int steps = g_held ? 10 : 1;   // held: ten at a time
    int n;
    if (is_auto) {
        if (s < 0) return false;   // left stops at automatic
        n = start < lo ? lo : start > hi ? hi : start;
    } else {
        n = v + (s == 2 ? 1 : s * steps);
        if (s == 2 && n > hi) n = 0;           // A / click past the top goes round to automatic
        else if (n < lo) n = s < 0 && v == lo ? 0 : lo;   // left from the lowest: automatic
        else if (n > hi) n = hi;
    }
    if (n == v && !is_auto) return false;
    char out[16];
    if (n <= 0) snprintf(out, sizeof out, "auto");
    else snprintf(out, sizeof out, "%d", n);
    apply(key, out);
    return true;
}

// ---- binding rows (the remapping rows of the Controls tab): a label and three cells, each its own nav item

// Text centred (left-aligned when centred is false) in [x0, x1] on a row from y to y + h, at the font size or smaller so
// a long name ("Нажатие правого стика", "Стрелка вправо") fits; under 65 % it is clipped instead.
static void fit_text(ImDrawList *dl, float x0, float x1, float y, float h, ImU32 col, const char *t, bool centred) {
    ImFont *font = ImGui::GetFont();
    float fs = ImGui::GetFontSize(), room = x1 - x0, tw = font->CalcTextSizeA(fs, FLT_MAX, 0, t).x;
    if (tw > room) {
        fs = fmaxf(fs * room / tw, fs * 0.65f);
        tw = font->CalcTextSizeA(fs, FLT_MAX, 0, t).x;
    }
    float x = centred && tw < room ? x0 + (room - tw) * 0.5f : x0;
    ImVec4 clip(x0, y, x1, y + h);
    dl->AddText(font, fs, ImVec2(x, y + (h - fs) * 0.5f), col, t, NULL, 0, &clip);
}

// The label column and the three cells of a binding row of width w.
static void bind_columns(float x, float w, float cx[3], float *cw) {
    float lw = floorf(w * 0.34f), gap = floorf(8 * g_scale);
    *cw = floorf((w - lw - gap * 3) / 3);
    for (int c = 0; c < 3; c++) cx[c] = x + lw + gap + c * (*cw + gap);
}

void opt_bind_header(UiTextId c0, UiTextId c1, UiTextId c2) {
    if (g_sink) {   // the column titles, as an INFO row of their own
        MkwSettingRow r;
        sink_blank(r, MKW_SET_INFO);
        r.id = "##bind_header";
        r.cells[0] = menu_text(c0), r.cells[1] = menu_text(c1), r.cells[2] = menu_text(c2);
        sink_emit(r);
        return;
    }
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, cx[3], cw, fs = ImGui::GetFontSize() * 0.8f;
    bind_columns(p.x, w, cx, &cw);
    const UiTextId cols[3] = {c0, c1, c2};
    ImFont *font = ImGui::GetFont();
    for (int c = 0; c < 3; c++) {
        const char *t = menu_text(cols[c]);
        float tw = font->CalcTextSizeA(fs, FLT_MAX, 0, t).x;
        ImGui::GetWindowDrawList()->AddText(font, fs, ImVec2(cx[c] + (cw - tw) * 0.5f, p.y), g_look->dim, t);
    }
    ImGui::Dummy(ImVec2(w, fs * 1.2f));
}

// opt_bind_icon: the next row's cell shows that controller button as an icon (ui/glyphs/pad_icons.h) instead of its name
static struct { int pad, kind; } g_icon[3];
void opt_bind_icon(int cell, int pad, int kind) {
    if (cell >= 0 && cell < 3 && !g_sink) g_icon[cell] = {pad, kind};
}

static int sink_bind_row(const char *key, const char *label, UiTextId desc, const char *const cells[3],
                         const bool enabled[3], const char *note) {
    char k[64];
    snprintf(k, sizeof k, "MKW_%s", key);
    MkwSettingRow r;
    sink_fill(r, MKW_SET_BIND, k, label, desc, nullptr, note, false);
    r.flags &= ~(uint32_t)MKW_SETF_DISABLED;   // as the menu: a binding row takes changes either way
    r.id = key;
    for (int c = 0; c < 3; c++) {
        r.cells[c] = cells[c] ? cells[c] : "";
        r.cell_enabled[c] = enabled[c];
    }
    if (g_bind_cap >= 0) r.flags |= MKW_SETF_CAPTURE, r.capture_cell = g_bind_cap;
    g_bind_cap = -1;
    sink_emit(r);
    const char *in = sink_take(key, false);
    if (!in) return -1;
    int cell = -1, clear = 0;
    if (!strncmp(in, "capture:", 8)) cell = atoi(in + 8);
    else if (!strncmp(in, "clear:", 6)) cell = atoi(in + 6), clear = 1;
    if (cell < 0 || cell > 2 || !enabled[cell]) return sink_bad(), -1;
    sink_changed();
    return clear ? 10 + cell : cell;
}

int opt_bind_row(const char *key, const char *label, UiTextId desc, const char *const cells[3], const bool enabled[3],
                 const char *note) {
    if (g_sink) return sink_bind_row(key, label, desc, cells, enabled, note);
    ImGui::PushID(key);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float fs = ImGui::GetFontSize(), w = ImGui::GetContentRegionAvail().x, h = floorf(fs * 1.9f), cx[3], cw;
    bind_columns(p.x, w, cx, &cw);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float r = 8 * g_scale, pad = 6 * g_scale;
    fit_text(dl, p.x + 16 * g_scale, cx[0] - fs * 0.3f, p.y, h, g_look->label, label, false);
    int result = -1;
    bool row_foc = false, row_hov = false;
    for (int c = 0; c < 3; c++) {
        ImVec2 a(cx[c], p.y), b(cx[c] + cw, p.y + h);
        const char *t = cells[c] && cells[c][0] ? cells[c] : "-";
        if (!enabled[c]) {   // not a nav item: the cursor skips it
            dl->AddRect(a, b, g_look->cell_off, r);
            fit_text(dl, a.x + pad, b.x - pad, a.y, h, g_look->off, t, true);
            continue;
        }
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(c);
        ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));   // the cell's own border shows the selection
        bool clicked = ImGui::InvisibleButton("##cell", ImVec2(cw, h), ImGuiButtonFlags_EnableNav);
        ImGui::PopStyleColor();
        char id[80];
        snprintf(id, sizeof id, "%s#%d", key, c);
        bool to_key = g_focus_key[0] && !strcmp(id, g_focus_key);
        if (g_take_first || to_key) {
            ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
            ImGui::SetNavCursorVisible(true);
            if (g_take_first) ImGui::SetScrollY(0);
            else ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
            g_take_first = false;
            if (to_key) g_focus_key[0] = 0;
        }
        bool foc = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
        bool hov = g_mouse && ImGui::IsItemHovered(), sel = hov || (foc && !g_mouse);   // as row()
        row_foc |= foc && !g_mouse;
        row_hov |= hov;
        if (clicked) result = c;
        if (foc && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)))
            result = 10 + c;
        dl->AddRectFilled(a, b, sel ? g_look->fill_hot : g_look->fill, r);
        dl->AddRect(a, b, sel ? g_look->select : g_look->border, r, 0, sel ? 2.0f : 1.0f);
        if (!(g_icon[c].pad && pad_icon_draw(dl, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), h * 0.8f, g_icon[c].pad,
                                             g_icon[c].kind)))
            fit_text(dl, a.x + pad, b.x - pad, a.y, h, g_look->value, t, true);
        ImGui::PopID();
    }
    char k[64];
    snprintf(k, sizeof k, "MKW_%s", key);
    if (row_foc) desc_set(g_focus, desc, k, NULL, NULL);
    if (row_hov) desc_set(g_hover, desc, k, NULL, NULL);
    if (!g_first.text[0]) desc_set(g_first, desc, k, NULL, NULL);
    if (note && (row_foc || row_hov)) {   // e.g. what moved here, after the description
        Desc &d = row_hov ? g_hover : g_focus;
        size_t n = strlen(d.text);
        snprintf(d.text + n, sizeof d.text - n, "\n\n%s", note);
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, h));
    ImGui::PopID();
    memset(g_icon, 0, sizeof g_icon);   // one row only
    return result;
}
