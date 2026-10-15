#include <intrin.h>
// Installs the co-op hooks. All of them are single vtable-slot swaps: reversible,
// checked against the value this build of the game is expected to hold, and
// unable to collide with byte patches other mods put inside the functions.
//
// 1. CKInput::update (vtable 0x623F38 slot 20, 0x436360) -- the per-frame tick.
//    It runs every frame, and by the time it returns the action array is fresh.
//    __thiscall with one stack argument (ret 4).
//
// 2. CKHkAsterixLife / CKHkObelixLife slot 17 -- each hero's per-frame Life tick,
//    where the player/AI decision is made (life.cpp).
//
// 3. CKGrpTrio::sendEvent (vtable 0x6256A0 slot 5, 0x47AC10) -- so leader switches
//    and scripted mode changes see the hero arrangement they were written for.
//    __thiscall (event, param), ret 8.

#include "coop.h"
#include "autotest.h"
#include "config.h"
#include "crash.h"
#include "life.h"
#include "log.h"
#include "shared.h"
#include "hints.h"

namespace {

const uintptr_t kInputVtbl       = 0x623F38;
const size_t    kInputUpdateSlot = 20;        // +0x50
const uintptr_t kInputUpdate     = 0x436360;

const uintptr_t kTrioVtbl          = 0x6256A0;
const size_t    kTrioSendEventSlot = 5;       // +0x14
const uintptr_t kTrioSendEvent     = 0x47AC10;

typedef void(__fastcall* InputUpdateFn)(void* self, void* edx, void* timeInfo);
typedef int(__fastcall* SendEventFn)(void* self, void* edx, int event, void* param);

InputUpdateFn g_origInputUpdate = nullptr;
SendEventFn   g_origTrioSendEvent = nullptr;
} // namespace
void* g_trioEventCaller = nullptr;   // who sends the trio event under way (coop.cpp)
namespace {

void __fastcall InputUpdateHook(void* self, void* edx, void* timeInfo) {
	g_origInputUpdate(self, edx, timeInfo);   // let the action array refresh first
	InterlockedIncrement(&g_hookCounters.inputUpdates);
	StallRegisterGameThread();
	AutotestFrame();
	CoopFrame();
	SharedFrame();
	HintsFrame();
}

int __fastcall TrioSendEventHook(void* self, void* edx, int event, void* param) {
	g_trioEventCaller = _ReturnAddress();
	if (CoopDeferTrioEvent(self, event)) return 0;
	CoopNoteTrioEvent(self, event, param);
	int cookie = CoopBeforeTrioEvent(event, _ReturnAddress());
	int r = g_origTrioSendEvent(self, edx, event, param);
	CoopAfterTrioEvent(event, cookie);
	return r;
}

} // namespace

uintptr_t g_patchedInputUpdate = 0;

// Swap one vtable entry, checking it still holds what this build expects.
bool PatchVtableSlot(uintptr_t vtbl, size_t slot, uintptr_t expected,
                     void* replacement, void** outOriginal, const char* what) {
	uintptr_t* entry = (uintptr_t*)(vtbl + slot * sizeof(uintptr_t));

	DWORD oldProtect = 0;
	if (!VirtualProtect(entry, sizeof(uintptr_t), PAGE_READWRITE, &oldProtect)) {
		Log("hooks: cannot unprotect %s vtable slot at %p (err %lu)",
			what, entry, GetLastError());
		return false;
	}

	uintptr_t current = *entry;
	if (current != expected) {
		// Either this is a different build of the game, or something else got
		// there first. Refusing is much better than corrupting a live vtable.
		Log("hooks: %s vtable slot holds %08X, expected %08X -- not hooking",
			what, (unsigned)current, (unsigned)expected);
		VirtualProtect(entry, sizeof(uintptr_t), oldProtect, &oldProtect);
		return false;
	}

	*outOriginal = (void*)current;
	*entry = (uintptr_t)replacement;
	VirtualProtect(entry, sizeof(uintptr_t), oldProtect, &oldProtect);
	Log("hooks: %s %08X -> %p", what, (unsigned)current, replacement);
	return true;
}

bool CoopInstallHooks() {
	bool ok = true;
	g_patchedInputUpdate = (uintptr_t)InputUpdateHook;
	ok &= PatchVtableSlot(kInputVtbl, kInputUpdateSlot, kInputUpdate,
	                      (void*)InputUpdateHook, (void**)&g_origInputUpdate,
	                      "CKInput::update");
	ok &= LifeInstallHooks();
	ok &= PatchVtableSlot(kTrioVtbl, kTrioSendEventSlot, kTrioSendEvent,
	                      (void*)TrioSendEventHook, (void**)&g_origTrioSendEvent,
	                      "CKGrpTrio::sendEvent");
	return ok;
}
