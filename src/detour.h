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

// Replaces the `len` bytes at `at` (>= 5, whole instructions, checked against
// `expected`) with `call target`, padded with NOPs. For redirecting one call
// site, e.g. an indirect `mov edx,[ecx]; call [edx+N]` pair, to a function that
// does the same call with something around it.
bool PatchCallSite(uintptr_t at, const uint8_t* expected, size_t len, void* target,
                   const char* name);
