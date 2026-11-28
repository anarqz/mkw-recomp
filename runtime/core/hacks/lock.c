// The hacks' lock (plugin API 6, MkwPluginApi.hacks_lock): while a plugin says the player is online, the hacks that
// can be used to cheat are ignored whatever mkw.ini or the environment says, where each is read: turbo (turbo.c),
// freeze (freeze.c) and the input script's playback (input/input_script.c); the menu's Hacks tab greys their rows out
// with Same randomness (ui/menu/settings_tabs.cpp). The seed and the logic rate apply at start and cannot be taken back
// live: hacks_lock says when one is on. The other hacks (Classic GP / VS, unlock everything, the quick race, skip
// movies) are left as they are.
#include <stdatomic.h>

#include "cfg.h"
#include "hacks.h"
#include "host.h"
#include "mkw_plugin.h"

static _Atomic int locked;

int hacks_locked(void) { return atomic_load_explicit(&locked, memory_order_relaxed); }

int hacks_lock(int on) {
    int was = atomic_exchange(&locked, on != 0);   // a frozen game runs again at its next present (freeze.c)
    int stuck = 0;
    if (on) {
        if (seed_used() || *cfg_value("MKW_SEED")) stuck |= MKW_HACK_SEED;   // the generators seeded at start
        if (frame_logic_hz_set() != 60) stuck |= MKW_HACK_LOGIC_HZ;          // the game steps at N Hz online
    }
    if ((on != 0) != (was != 0) || stuck)
        rt_log("hacks: %s by a plugin%s%s%s", on ? "locked" : "unlocked", stuck ? "; on since the start:" : "",
               stuck & MKW_HACK_SEED ? " seed" : "", stuck & MKW_HACK_LOGIC_HZ ? " logic rate" : "");
    return stuck;
}
