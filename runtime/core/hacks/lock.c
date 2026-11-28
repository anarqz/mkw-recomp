// The hacks' lock (plugin API 6, MkwPluginApi.hacks_lock): while a plugin says the player is online, the hacks that
// could give an edge are ignored whatever mkw.ini or the environment says, each where it is read (turbo.c, freeze.c,
// skip.c, seed.c, classic.c, core/boot/game.c, input/input_script.c, sdk/fs/fs_host.c), and the menu's Hacks tab
// greys their rows out (ui/menu/settings_tabs.cpp). What only applies at start and already did cannot be taken back:
// hacks_lock says which.
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
        if (seed_used()) stuck |= MKW_HACK_SEED;            // the system's entropy and generators seeded at start
        if (unlock_applied()) stuck |= MKW_HACK_UNLOCK_ALL; // the save was edited when the game opened it
    }
    if ((on != 0) != (was != 0) || stuck)
        rt_log("hacks: %s by a plugin%s%s%s", on ? "locked" : "unlocked", stuck ? "; already applied at start:" : "",
               stuck & MKW_HACK_SEED ? " seed" : "", stuck & MKW_HACK_UNLOCK_ALL ? " unlock-all" : "");
    return stuck;
}
