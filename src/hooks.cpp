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
//
// 4. CKPathFindingCinematicBloc slot 15 (vtable 0x624D04, 0x467660) -- a cutscene's
//    path block tick. __thiscall (time), ret 4. A game bug the co-op game sets
//    off (LV2-3's powder keg: crash at 0x531CE5; not seen without the mod): a
//    block walks a character along a path in one of the pathfinding service's
//    20 slots (yp +0x64, 0x514 bytes each; block +0x98 = slot, -1 none). With
//    +0x2C bit 0x40 clear the tick asks for a slot (+0x118 tries, 10 from the
//    start vf13); set, FUN_00467830 steps the slot -- slot -1 too: slot 255,
//    0x50FEC past the service, its path point written over whatever lies there
//    (LV2-3: a Roman's component pointer +0x54, read by its next move). The
//    level stores blocks with the bit set and vf14 drops the slot keeping it. A
//    block ticked with the bit and no slot asks for one first, as its start does.

#include "coop.h"
#include "autotest.h"
#include "config.h"
#include "crash.h"
#include "life.h"
#include "log.h"
#include "shared.h"
#include "hints.h"
#include "icons.h"

namespace {

const uintptr_t kInputVtbl       = 0x623F38;
const size_t    kInputUpdateSlot = 20;        // +0x50
const uintptr_t kInputUpdate     = 0x436360;

const uintptr_t kTrioVtbl          = 0x6256A0;
const size_t    kTrioSendEventSlot = 5;       // +0x14
const uintptr_t kTrioSendEvent     = 0x47AC10;

const uintptr_t kPathBlocVtbl     = 0x624D04;
const size_t    kPathBlocTickSlot = 15;       // +0x3C
const uintptr_t kPathBlocTick     = 0x467660;

typedef void(__fastcall* InputUpdateFn)(void* self, void* edx, void* timeInfo);
typedef int(__fastcall* SendEventFn)(void* self, void* edx, int event, void* param);
typedef void(__fastcall* PathBlocTickFn)(char* bloc, void* edx, void* time);

InputUpdateFn  g_origInputUpdate = nullptr;
SendEventFn    g_origTrioSendEvent = nullptr;
PathBlocTickFn g_origPathBlocTick = nullptr;
volatile long  g_pathBlocsFixed = 0;
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
	IconsFrame();
}

int __fastcall TrioSendEventHook(void* self, void* edx, int event, void* param) {
	g_trioEventCaller = _ReturnAddress();
	if (CoopDeferTrioEvent(self, event)) return 0;
	// (Again: an event sent in there -- the merchant's close, 0x5124, for a
	// launcher's 0x4804 -- set its own.)
	g_trioEventCaller = _ReturnAddress();
	CoopNoteTrioEvent(self, event, param);
	bool ride = CoopRideSliceBegin(event, _ReturnAddress());
	bool beam = CoopBeamCatchBegin(event, _ReturnAddress());
	int cookie = CoopBeforeTrioEvent(event, _ReturnAddress());
	int r = g_origTrioSendEvent(self, edx, event, param);
	CoopAfterTrioEvent(event, cookie);
	CoopBeamCatchEnd(beam);
	CoopRideSliceEnd(ride);
	return r;
}

void __fastcall PathBlocTickHook(char* bloc, void* edx, void* time) {
	uint8_t& flags = *(uint8_t*)(bloc + 0x2C);
	if (*(int8_t*)(bloc + 0x98) == -1 && (flags & 0x40)) {
		flags &= ~0x40;
		*(uint8_t*)(bloc + 0x118) = 10;
		if (InterlockedIncrement(&g_pathBlocsFixed) <= 20)
			Log("hooks: cutscene path block %p ticked with no path slot -- asks for one first", bloc);
	}
	g_origPathBlocTick(bloc, edx, time);
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
	// a game fix: not counted in ok, the co-op runs without it
	PatchVtableSlot(kPathBlocVtbl, kPathBlocTickSlot, kPathBlocTick, (void*)PathBlocTickHook,
	                (void**)&g_origPathBlocTick, "CKPathFindingCinematicBloc tick");
	return ok;
}
