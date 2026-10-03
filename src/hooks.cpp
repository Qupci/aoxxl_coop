// Installs the co-op hooks.
//
// Two different things are hooked:
//
// 1. CKInput::update -- vtable 0x623F38 slot 20 (0x436360), the input manager's
//    per-frame update. This is the frame tick: it runs every frame regardless of
//    what the heroes are doing, and by the time it returns the mapped-action
//    array is fresh. Signature is __thiscall with one stack argument (it ends in
//    `ret 4`), so as __fastcall that is (self, edx, timeInfo).
//
// 2. FUN_0048D4F0, the hero state-machine tick, which is where per-hero input
//    swapping has to happen. It picks a state via a virtual selector and then
//    calls the state handler with 0=run, 1=enter, 2=exit, so bracketing it
//    brackets everything the hero does for that frame.
//
//    This one needs an inline detour rather than a vtable patch. Hooking
//    CKHkAsterixLife slot 13 (0x4AA850) looked right on paper -- it is the only
//    caller of CKHkAsterix::update -- but runtime profiling showed that slot
//    firing exactly once per level while the game played on happily. The engine
//    swaps a hero's Life class depending on what the hero is doing, and
//    FUN_0048D4F0 is called directly from nineteen different life ticks. It is
//    the one choke point common to all of them.
//
//    `this` is the hero hook, so heroes are told apart by comparing against the
//    trio's pointers; every other hook that runs through here is ignored.

#include "coop.h"
#include "config.h"
#include "detour.h"
#include "log.h"
#include "probe.h"
#include "trio.h"

namespace {

const uintptr_t kInputVtbl       = 0x623F38;
const size_t    kInputUpdateSlot = 20;        // +0x50
const uintptr_t kInputUpdate     = 0x436360;

const uintptr_t kHeroStateTick   = 0x48D4F0;
// PUSH ECX / PUSH EBX / PUSH ESI / MOV ESI,ECX -- exactly five bytes, so the
// detour does not split an instruction.
const uint8_t   kHeroStateTickPrologue[5] = { 0x51, 0x53, 0x56, 0x8B, 0xF1 };

// 3. FUN_0049DE10, the hero *AI* state machine -- the exact mirror of the one
//    above, using hero+0x55C for its handler table and hero+0x561 for its
//    current state, with FUN_0049EE80 choosing the next state.
//
//    A hero driven by a player runs the first machine only; the companion runs
//    both, and the AI one is what actually steers it. Forcing hero+0x561 to the
//    engine's "no state" marker did not help because this runs afterwards and
//    immediately picks a new state. Skipping it outright for player 2's hero is
//    what leaves their input in charge. Same prologue, so the same detour works.
const uintptr_t kHeroAiTick = 0x49DE10;
const uint8_t   kHeroAiTickPrologue[5] = { 0x51, 0x53, 0x56, 0x8B, 0xF1 };

typedef void(__fastcall* HeroStateTickFn)(void* self, void* edx);
typedef void(__fastcall* InputUpdateFn)(void* self, void* edx, void* timeInfo);

HeroStateTickFn g_origHeroStateTick = nullptr;
HeroStateTickFn g_origHeroAiTick    = nullptr;
InputUpdateFn   g_origInputUpdate   = nullptr;

void __fastcall InputUpdateHook(void* self, void* edx, void* timeInfo) {
	g_origInputUpdate(self, edx, timeInfo);   // let the action array refresh first
	g_hookCounters.inputUpdates++;
	CoopFrame();
}

// Which hero, if any, is this? Non-hero hooks also run through the state tick.
int HeroIndexOf(void* hook) {
	Trio t = GetTrio();
	if (!t || !hook) return -1;
	for (int i = 0; i < HERO_COUNT; i++)
		if (t.GetHero(i).hook == (KHook*)hook) return i;
	return -1;
}

// FUN_0049DE10 is not "the AI" -- it is the hero's main action state machine
// (locomotion, gravity, jumping, attacking). Its handlers end in `RET 8` and
// take (mode, prevState); the *player's* input sequence lives inside one of
// them. FUN_0049EE80 chooses player handlers or AI handlers, and it decides
// that from the hero's own state.
//
// Skipping this for player 2's hero froze him completely: no gravity, animation
// stuck on whatever was playing, drifting on whatever velocity he had. So it
// must run -- it just has to run while the hero looks player-driven and player
// 2's input is installed. Same bracket as the animation machine.
void __fastcall HeroAiTickHook(void* self, void* edx) {
	int idx = HeroIndexOf(self);
	if (idx < 0) { g_origHeroAiTick(self, edx); return; }

	Hero hero;
	hero.hook = (KHook*)self;
	CoopBeforeHeroTick(idx, hero);
	g_origHeroAiTick(self, edx);
	CoopAfterHeroTick(idx, hero);
}

void __fastcall HeroStateTickHook(void* self, void* edx) {
	int idx = HeroIndexOf(self);
	if (idx < 0) {                    // not a hero, pass straight through
		g_origHeroStateTick(self, edx);
		return;
	}
	if (idx == HERO_ASTERIX) g_hookCounters.asterixTicks++;
	else if (idx == HERO_OBELIX) g_hookCounters.obelixTicks++;

	Hero hero;
	hero.hook = (KHook*)self;
	CoopBeforeHeroTick(idx, hero);
	g_origHeroStateTick(self, edx);
	CoopAfterHeroTick(idx, hero);
}

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

} // namespace

HookCounters g_hookCounters;
uintptr_t g_patchedInputUpdate = 0;
uintptr_t g_patchedAsterixTick = 0;
uintptr_t g_patchedObelixTick  = 0;

bool CoopInstallHooks() {
	bool ok = true;
	g_patchedInputUpdate = (uintptr_t)InputUpdateHook;
	g_patchedAsterixTick = (uintptr_t)HeroStateTickHook;
	g_patchedObelixTick  = (uintptr_t)HeroStateTickHook;

	ok &= PatchVtableSlot(kInputVtbl, kInputUpdateSlot, kInputUpdate,
	                      (void*)InputUpdateHook, (void**)&g_origInputUpdate,
	                      "CKInput::update");
	ok &= InstallDetour(kHeroStateTick, (void*)HeroStateTickHook,
	                    (void**)&g_origHeroStateTick, kHeroStateTickPrologue,
	                    sizeof(kHeroStateTickPrologue), "heroStateTick");
	ok &= InstallDetour(kHeroAiTick, (void*)HeroAiTickHook,
	                    (void**)&g_origHeroAiTick, kHeroAiTickPrologue,
	                    sizeof(kHeroAiTickPrologue), "heroAiTick");
	return ok;
}
