// Finds out which code reads the CKInput action array.
//
// Player 2's synthesised input is swapped in for every one of Obelix's state
// ticks, with correct values, and he still does not move. So either the hero
// reads input somewhere outside that bracket, or it does not read this array at
// all. Guessing has been expensive; this measures it.
//
// Method: mark the page holding the action array PAGE_GUARD and catch the
// resulting STATUS_GUARD_PAGE_VIOLATION in a vectored exception handler. The OS
// clears the guard bit when it fires, so rather than single-stepping to re-arm
// (fiddly, and it would trap every access) the watchdog thread simply re-arms
// every so often. That samples readers instead of catching all of them, which is
// plenty to identify who they are.

#include "coop.h"
#include "config.h"
#include "inputmgr.h"
#include "log.h"
#include "trio.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <cstdio>

namespace {

const int kMaxSites = 24;

struct Site {
	uintptr_t eip = 0;
	uint32_t  hits = 0;
	uintptr_t lastAddr = 0;
	bool      write = false;
};

Site      g_sites[kMaxSites];
int       g_numSites = 0;
uintptr_t g_watchLo = 0, g_watchHi = 0;
PVOID     g_veh = nullptr;

// Our own diagnostics read this array too (the watchdog dumps it every cycle),
// and those reads are not what we are looking for.
uintptr_t g_selfLo = 0, g_selfHi = 0;

bool IsOurCode(uintptr_t eip) { return eip >= g_selfLo && eip < g_selfHi; }

void Record(uintptr_t eip, uintptr_t addr, bool write) {
	for (int i = 0; i < g_numSites; i++) {
		if (g_sites[i].eip == eip) {
			g_sites[i].hits++;
			g_sites[i].lastAddr = addr;
			return;
		}
	}
	if (g_numSites >= kMaxSites) return;
	g_sites[g_numSites].eip = eip;
	g_sites[g_numSites].hits = 1;
	g_sites[g_numSites].lastAddr = addr;
	g_sites[g_numSites].write = write;
	g_numSites++;
}

// PAGE_GUARD covers a whole 4 KB page, and the 59-byte action array shares its
// page with unrelated heap data. Every guard fault on that page must therefore
// be handled here -- letting the out-of-range ones fall through to
// EXCEPTION_CONTINUE_SEARCH is an unhandled 0x80000001 and kills the game.
//
// The OS clears the guard bit when it fires, so to keep sampling we set the trap
// flag and re-arm on the following single-step.
volatile long g_needRearm = 0;
uintptr_t     g_pageLo = 0, g_pageHi = 0;
volatile long g_totalFaults = 0;
const long    kFaultBudget = 20000;   // stop instrumenting rather than crawl

void ArmGuard() {
	if (!g_pageLo) return;
	DWORD old = 0;
	VirtualProtect((void*)g_pageLo, g_pageHi - g_pageLo, PAGE_READWRITE | PAGE_GUARD, &old);
}

LONG CALLBACK GuardHandler(EXCEPTION_POINTERS* ep) {
	DWORD code = ep->ExceptionRecord->ExceptionCode;

	if (code == EXCEPTION_SINGLE_STEP && InterlockedExchange(&g_needRearm, 0)) {
		if (g_totalFaults < kFaultBudget) ArmGuard();
		return EXCEPTION_CONTINUE_EXECUTION;
	}

	if (code != STATUS_GUARD_PAGE_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;

	uintptr_t addr = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
	if (addr < g_pageLo || addr >= g_pageHi)
		return EXCEPTION_CONTINUE_SEARCH;   // a different page entirely, not ours

	InterlockedIncrement(&g_totalFaults);

	uintptr_t eip = (uintptr_t)ep->ContextRecord->Eip;
	bool write = ep->ExceptionRecord->ExceptionInformation[0] != 0;
	if (addr >= g_watchLo && addr < g_watchHi && !IsOurCode(eip))
		Record(eip, addr, write);

	// Single-step past the faulting instruction, then re-arm.
	ep->ContextRecord->EFlags |= 0x100;
	InterlockedExchange(&g_needRearm, 1);
	return EXCEPTION_CONTINUE_EXECUTION;
}

} // namespace

void GuardWatchArm() {
	if (!g_config.watchActionReads) return;

	// Which region to watch. The action array turned out to be touched only by
	// CKInput's own binding code (every hit in 0x435xxx), so the hero must be
	// reading input from somewhere further down the chain -- the command block
	// at gameManager+0x40 being the obvious candidate.
	uint8_t* values = nullptr;
	int n = 0;
	if (g_config.watchTarget == WATCH_COMMAND_BLOCK) {
		values = (uint8_t*)GetCommandBlock();
		n = (int)kCommandBlockSize;
	} else if (g_config.watchTarget == WATCH_HERO_FLAGS) {
		// The flag words the state selector (FUN_0048F720 -> vtable +0x40)
		// branches on. Whoever writes these for the player's hero, and not for
		// the companion, is the input path we have been unable to find.
		Trio t = GetTrio();
		if (!t) return;
		Hero h = t.GetHero(g_config.watchHero >= 0 ? g_config.watchHero : t.Leader());
		if (!h) return;
		values = (uint8_t*)(h.raw() + 0x380);
		n = 0x78;                       // covers +0x380 .. +0x3F7
	} else {
		InputManager im = GetInputManager();
		values = im ? im.Values() : nullptr;
		// The buffer is longer than numActions: FUN_004353C0 writes a second
		// bank at `values + numActions + i`. Watching only the first bank meant
		// every consumer of the later banks faulted and was then discarded as
		// out of range, which is why this looked like "nobody reads it".
		n = im.NumActions() * 4;
	}
	if (!values || n <= 0) return;

	if (!g_veh) {
		g_veh = AddVectoredExceptionHandler(1, GuardHandler);
		if (!g_veh) { Log("guard: AddVectoredExceptionHandler failed"); return; }

		MODULEINFO mi = {};
		HMODULE self = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                   (LPCSTR)&GuardWatchArm, &self);
		if (self && GetModuleInformation(GetCurrentProcess(), self, &mi, sizeof(mi))) {
			g_selfLo = (uintptr_t)mi.lpBaseOfDll;
			g_selfHi = g_selfLo + mi.SizeOfImage;
		}
		Log("guard: watching action array %p..%p (ignoring our own code %08X..%08X)",
			values, values + n, (unsigned)g_selfLo, (unsigned)g_selfHi);
	}
	g_watchLo = (uintptr_t)values;
	g_watchHi = (uintptr_t)values + n;

	SYSTEM_INFO si = {};
	GetSystemInfo(&si);
	g_pageLo = g_watchLo & ~(uintptr_t)(si.dwPageSize - 1);
	g_pageHi = ((g_watchHi + si.dwPageSize - 1) & ~(uintptr_t)(si.dwPageSize - 1));

	ArmGuard();
}

void GuardWatchReport() {
	if (!g_config.watchActionReads || !g_numSites) return;
	Log("guard: %d distinct code sites touched the action array:", g_numSites);
	for (int i = 0; i < g_numSites; i++)
		Log("guard:   eip=%08X  %-5s  hits=%u  lastAddr=+%u",
			(unsigned)g_sites[i].eip, g_sites[i].write ? "write" : "read",
			g_sites[i].hits, (unsigned)(g_sites[i].lastAddr - g_watchLo));
}




