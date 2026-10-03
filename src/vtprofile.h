// Vtable slot profiler.
//
// Static analysis said CKHkAsterixLife slot 13 was the per-frame tick, because
// it is the only thing that calls CKHkAsterix::update. Runtime says that slot
// fires exactly once per level and the game plays fine without it. Rather than
// guess again, this instruments every slot of a vtable with a counting thunk and
// reports which ones actually run.
//
// Thunks are generated at runtime as
//     FF 05 <&counter>     inc dword ptr [counter]
//     FF 25 <&original>    jmp dword ptr [original]
// which touches no registers and only clobbers flags (dead on function entry),
// so it is transparent to any calling convention.

#pragma once
#include <cstdint>

const int kMaxProfiledSlots = 64;

// Install counting thunks over slots [0, numSlots) of `vtable`.
bool VtProfileInstall(uintptr_t vtable, int numSlots, const char* name);

// Log per-slot call counts since the previous report.
void VtProfileReport();
