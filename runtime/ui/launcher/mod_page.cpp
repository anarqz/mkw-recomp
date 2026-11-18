// A mod's options page on the start screen (mod_page.h): mkw_plugin.h's MkwPage drawn with the start screen's ImGui.
//
// The plugin's library is loaded here only when the player presses the mod's Options button (the Mods page already
// says that a plugin runs its own program), and only its mkw_mod_page is called: no game exists in this process. The
// settings the widgets show and store are mod_settings.c's, for the player the next start plays, so the plugin reads
// them in the game (MkwPluginApi.setting_get).
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "imgui.h"

#include "launcher_look.h"
#include "launcher_players.h"
#include "mkw_plugin.h"
#include "mod_page.h"
#include "mod_settings.h"

extern "C" {   // the runtime's C headers, as mods.cpp includes them
#include "cfg.h"
#include "platform/platform.h"
#include "ui_text.h"
}

extern "C" void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern const char rt_port_version[];

#ifdef _WIN32
#define PLUGIN_EXT ".dll"
#else
#define PLUGIN_EXT ".so"
#endif

enum { MAX_INPUTS = 32, INPUT_CAP = 256, MAX_LIBS = 16 };

// Every library a page loaded stays loaded: its threads may still run when the page closes.
static struct {
    char path[PLAT_PATH_MAX * 2 + 300];
    MkwModPage fn;   // NULL: the library loaded but exports no page
} libs[MAX_LIBS];
static int nlibs;

typedef struct {
    char key[MOD_SETTING_KEY_MAX + 1];
    char buf[INPUT_CAP];
    int editing;   // the field had the keyboard in the last frame: its text is the player's, not the setting's
} Input;

static struct {
    int open;
    char mod[64], title[96], mod_dir[PLAT_PATH_MAX * 2];
    MkwModPage fn;
    MkwPage page;
    char player_dir[PLAT_PATH_MAX * 2], player_name[96];
    float scale;
    int disabled;                // disabled(1) calls not yet closed this frame
    int in_list, list_rows;
    char list_empty[256];
    Input inputs[MAX_INPUTS];
    int ninputs;
} G;

// ---- the entries that are not ImGui's (any thread)
static void page_log(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    rt_log("mod %s: %s", G.mod, line);
}

static int page_setting_get(const char *key, char *out, size_t cap) {
    return mod_settings_get(G.player_dir, G.mod, key, out, cap);
}

static int page_setting_set(const char *key, const char *value) {
    return mod_settings_set(G.player_dir, G.mod, key, value);
}

static void page_redraw(void) {
    // the start screen draws every frame already; an event wakes it in case it ever waits for input
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_EVENT_USER;
    SDL_PushEvent(&e);
}

// ---- text and layout
static ImVec4 col4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

static void page_heading(const char *text) {
    ImGui::Spacing();
    ImGui::TextColored(col4(LOOK_HEADING), "%s", text ? text : "");
    ImGui::Separator();
}

static void page_text(const char *text) { ImGui::TextWrapped("%s", text ? text : ""); }

static void page_note(int kind, const char *text) {
    ImU32 c = kind == MKW_NOTE_INFO ? LOOK_ACCENT_HOT : kind == MKW_NOTE_OK ? LOOK_OK : kind == MKW_NOTE_WARN ? LOOK_WARN
            : kind == MKW_NOTE_ERROR ? LOOK_BAD : LOOK_DIM;
    ImGui::PushStyleColor(ImGuiCol_Text, col4(c));
    ImGui::TextWrapped("%s", text ? text : "");
    ImGui::PopStyleColor();
}

static void page_separator(void) { ImGui::Separator(); }
static void page_same_line(void) { ImGui::SameLine(); }

static void page_disabled(int on) {
    if (on) {
        ImGui::BeginDisabled(true);
        G.disabled++;
    } else if (G.disabled) {
        ImGui::EndDisabled();
        G.disabled--;
    }
}

static void page_busy(const char *text) {
    float h = ImGui::GetFrameHeight();
    ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(h * 3, h * 0.4f), "");
    ImGui::SameLine();
    ImGui::TextUnformatted(text ? text : "");
}

// ---- widgets bound to a setting
static int page_checkbox(const char *key, const char *label, int default_on) {
    char v[8];
    bool on = page_setting_get(key, v, sizeof v) ? !strcmp(v, "1") : default_on != 0;
    ImGui::PushID(key);
    bool changed = ImGui::Checkbox(label ? label : key, &on);
    ImGui::PopID();
    if (changed && !page_setting_set(key, on ? "1" : "0")) page_log("setting %s not stored", key);
    return changed;
}

static Input *input_of(const char *key) {
    for (int i = 0; i < G.ninputs; i++)
        if (!strcmp(G.inputs[i].key, key)) return &G.inputs[i];
    if (G.ninputs == MAX_INPUTS) return nullptr;
    Input *in = &G.inputs[G.ninputs++];
    snprintf(in->key, sizeof in->key, "%s", key);
    in->buf[0] = 0;
    in->editing = 0;
    return in;
}

static int digits_only(ImGuiInputTextCallbackData *d) { return !(d->EventChar >= '0' && d->EventChar <= '9'); }

static int page_input(const char *key, const char *label, const char *hint, int max_bytes, int flags) {
    if (!mod_settings_key_ok(key)) return 0;
    Input *in = input_of(key);
    if (!in) {
        page_log("more than %d text fields: %s not shown", MAX_INPUTS, key);
        return 0;
    }
    if (max_bytes <= 0 || max_bytes > INPUT_CAP - 1) max_bytes = INPUT_CAP - 1;
    ImGui::PushID(key);
    if (!in->editing) {   // not being edited: show the stored value (a thread may have changed it)
        if (!page_setting_get(key, in->buf, sizeof in->buf)) in->buf[0] = 0;
    }
    if (label && *label) ImGui::TextUnformatted(label);
    ImGuiInputTextFlags f = ImGuiInputTextFlags_None;
    if (flags & MKW_INPUT_PASSWORD) f |= ImGuiInputTextFlags_Password;
    if (flags & MKW_INPUT_DIGITS) f |= ImGuiInputTextFlags_CallbackCharFilter;
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##field", hint ? hint : "", in->buf, (size_t)max_bytes + 1, f,
                             (flags & MKW_INPUT_DIGITS) ? digits_only : nullptr);
    in->editing = ImGui::IsItemActive();
    int changed = 0;
    if (ImGui::IsItemDeactivatedAfterEdit()) {   // Enter, a click away, the cursor moving on
        changed = 1;
        if (!page_setting_set(key, in->buf[0] ? in->buf : nullptr)) page_log("setting %s not stored", key);
    }
    ImGui::PopID();
    return changed;
}

static int page_choice(const char *key, const char *label, const char *const *values, const char *const *labels,
                       int n) {
    if (!values || n <= 0) return 0;
    char v[MOD_SETTING_VALUE_MAX + 1];
    int cur = 0;
    if (page_setting_get(key, v, sizeof v))
        for (int i = 0; i < n; i++)
            if (values[i] && !strcmp(values[i], v)) cur = i;
    auto shown = [&](int i) { return labels && labels[i] ? labels[i] : values[i] ? values[i] : ""; };
    int changed = 0;
    ImGui::PushID(key);
    if (label && *label) ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##choice", shown(cur))) {
        for (int i = 0; i < n; i++) {
            if (ImGui::Selectable(shown(i), i == cur) && i != cur && values[i]) {
                changed = page_setting_set(key, values[i]);
                if (!changed) page_log("setting %s not stored", key);
            }
            if (i == cur) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    return changed;
}

// ---- actions
static int page_button(const char *label) { return ImGui::Button(label ? label : "?"); }

// ---- lists
static void page_list_begin(const char *id, int visible_rows) {
    if (G.in_list) return;   // no nesting: the rows go into the open list
    if (visible_rows < 1) visible_rows = 1;
    float row = ImGui::GetFrameHeightWithSpacing();
    ImGui::PushID(id ? id : "list");
    ImGui::BeginChild("##list", ImVec2(0, row * (float)visible_rows + ImGui::GetStyle().WindowPadding.y * 2),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
    G.in_list = 1;
    G.list_rows = 0;
    G.list_empty[0] = 0;
}

static void page_list_empty(const char *text) { snprintf(G.list_empty, sizeof G.list_empty, "%s", text ? text : ""); }

static int page_list_row(const char *name, const char *detail, const char *const *buttons, int nbuttons) {
    if (nbuttons < 0) nbuttons = 0;
    if (nbuttons > 4) nbuttons = 4;
    ImGui::PushID(G.list_rows++);
    ImGui::PushID(name ? name : "");
    float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;   // the row's right edge
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(name ? name : "");
    if (detail && *detail) {
        ImGui::SameLine();
        ImGui::TextColored(col4(LOOK_DIM), "%s", detail);
    }
    int pressed = 0;
    if (nbuttons) {
        const ImGuiStyle &st = ImGui::GetStyle();
        float w = 0;
        for (int i = 0; i < nbuttons; i++)
            w += ImGui::CalcTextSize(buttons[i] ? buttons[i] : "?").x + st.FramePadding.x * 2 + (i ? st.ItemSpacing.x : 0);
        ImGui::SameLine();
        ImGui::SetCursorPosX(fmaxf(ImGui::GetCursorPosX(), right - w));
        for (int i = 0; i < nbuttons; i++) {
            if (i) ImGui::SameLine();
            ImGui::PushID(i);
            if (ImGui::SmallButton(buttons[i] ? buttons[i] : "?")) pressed = i + 1;
            ImGui::PopID();
        }
    }
    ImGui::PopID();
    ImGui::PopID();
    return pressed;
}

static void page_list_end(void) {
    if (!G.in_list) return;
    if (!G.list_rows && G.list_empty[0]) ImGui::TextColored(col4(LOOK_DIM), "%s", G.list_empty);
    ImGui::EndChild();
    ImGui::PopID();
    G.in_list = 0;
}

// ---- opening
static int lib_cb(const char *name, void *ctx) {
    const char *dir = (const char *)ctx;
    if (nlibs == MAX_LIBS) return 0;
    char path[sizeof libs[0].path];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    for (int i = 0; i < nlibs; i++)   // loaded by an earlier opening of this page
        if (!strcmp(libs[i].path, path)) {
            if (libs[i].fn && !G.fn) G.fn = libs[i].fn;
            return !G.fn;
        }
    void *lib = plat_lib_open(path);
    if (!lib) {
        rt_log("mod %s: plugin/%s does not load: no options from it", G.mod, name);
        return 1;
    }
    snprintf(libs[nlibs].path, sizeof libs[nlibs].path, "%s", path);
    libs[nlibs].fn = (MkwModPage)plat_lib_sym(lib, "mkw_mod_page");
    if (libs[nlibs].fn && !G.fn) G.fn = libs[nlibs].fn;
    rt_log("mod %s: plugin/%s loaded for its options page (%s)", G.mod, name,
           libs[nlibs].fn ? "it has one" : "it has none");
    nlibs++;
    return !G.fn;   // stop at the first page
}

extern "C" void mod_page_open(const char *mod, const char *title) {
    memset(&G.page, 0, sizeof G.page);
    snprintf(G.mod, sizeof G.mod, "%s", mod ? mod : "");
    snprintf(G.title, sizeof G.title, "%s", title && *title ? title : G.mod);
    snprintf(G.mod_dir, sizeof G.mod_dir, "%s/mods/%s", cfg_game_dir(), G.mod);
    G.fn = nullptr;
    G.ninputs = 0;
    G.player_name[0] = 0;
    char dir[sizeof G.mod_dir + 16];
    snprintf(dir, sizeof dir, "%s/plugin", G.mod_dir);
    if (G.mod[0] && plat_dir_exists(dir)) plat_file_list(dir, PLUGIN_EXT, lib_cb, dir);
    G.open = 1;
}

extern "C" int mod_page_is_open(void) { return G.open; }
extern "C" const char *mod_page_title(void) { return G.title; }
extern "C" void mod_page_close(void) { G.open = 0; }

extern "C" void mod_page_draw(float scale) {
    if (!G.open) return;
    if (!G.fn) {
        ImGui::TextDisabled("%s", menu_text(UI_MOD_NO_OPTIONS));
        return;
    }
    // the player may change between two openings (the Profiles page): the settings are theirs
    const char *name = launcher_player_name();
    int opened = strcmp(G.player_name, name) != 0;
    if (opened) {
        snprintf(G.player_name, sizeof G.player_name, "%s", name);
        launcher_player_dir(G.player_dir, sizeof G.player_dir);
        G.ninputs = 0;
    }
    G.scale = scale;
    MkwPage *p = &G.page;
    *p = MkwPage{};
    p->size = sizeof(MkwPage);
    p->version = MKW_PLUGIN_API_VERSION;
    p->port_version = rt_port_version;
    p->mod_dir = G.mod_dir;
    p->mod_name = G.mod;
    p->language = menu_lang();
    p->profile_name = G.player_name;
    p->scale = scale;
    p->opened = opened;
    p->log = page_log;
    p->setting_get = page_setting_get;
    p->setting_set = page_setting_set;
    p->redraw = page_redraw;
    p->heading = page_heading;
    p->text = page_text;
    p->note = page_note;
    p->separator = page_separator;
    p->same_line = page_same_line;
    p->disabled = page_disabled;
    p->busy = page_busy;
    p->checkbox = page_checkbox;
    p->input = page_input;
    p->choice = page_choice;
    p->button = page_button;
    p->list_begin = page_list_begin;
    p->list_row = page_list_row;
    p->list_end = page_list_end;
    p->list_empty = page_list_empty;
    ImGui::PushID("##mod_page");
    ImGui::PushTextWrapPos(0);
    G.fn(p);
    // whatever the plugin left open closes here, so one page's mistake cannot break the start screen's frame
    if (G.in_list) page_list_end();
    while (G.disabled) page_disabled(0);
    ImGui::PopTextWrapPos();
    ImGui::PopID();
}
