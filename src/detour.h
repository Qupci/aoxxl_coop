// Minimal 5-byte inline detour.
//
// Used for FUN_0048D4F0, the hero state-machine tick, which cannot be reached
// by patching a vtable: it is called directly from nineteen different hook-life
// tick functions (the engine swaps a hero's Life class depending on what the
// hero is doing, which is why hooking CKHkAsterixLife alone caught nothing).

#pragma once
#include <cstdint>
#include <cstddef>

// Overwrites `prologueLen` bytes at `target` with a jump to `hook`, and hands
// back a trampoline that runs the displaced bytes then continues at
// target+prologueLen. `expected` guards against patching the wrong build:
// installation is refused unless those bytes match.
//
// `prologueLen` must be >= 5 and must not split an instruction.
bool InstallDetour(uintptr_t target, void* hook, void** outTrampoline,
                   const uint8_t* expected, size_t prologueLen, const char* name);
