// dispatch.c's entries for the plugins' overrides by address (plugin.c). Kept out of runtime/cpu: RT_GAME_ID hashes the
// headers there (tools/build/build.py game_id), and these declarations change nothing the game code is compiled
// against, so adding them must not make players build their game code again.
#pragma once
#include <stdint.h>

#include "cpu.h"

// Indirect calls to the guest function starting at addr run fn instead (plugins: plugin/plugin.c); a later call for
// the same addr replaces fn. Before rt_dispatch_init only (0 otherwise); an addr where no function starts is fatal
// there. rt_dispatch_original: the translated function it replaced, NULL before rt_dispatch_init or if none.
int rt_dispatch_override(uint64_t addr, GuestFn fn);
GuestFn rt_dispatch_original(uint64_t addr);
