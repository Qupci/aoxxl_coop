// xxl_cutscenefix.asi: a cutscene crash fix for Asterix & Obelix XXL, independent of the co-op mod.
//
// A cutscene's pathfinding block (CKPathFindingCinematicBloc, vtable 0x624D04)
// walks a character along a path held in one of the pathfinding service's 20
// slots (yellow pages +0x64, 0x514 bytes each; block +0x98 = slot, -1 none).
// Its tick (vf15, 0x467660) requests a slot while the block's +0x2C bit 0x40 is
// clear (retries counted down in +0x118, as vf13 sets them) and steps the path
// (FUN_00467830 -> FUN_0045C010) once it is set. A block can be ticked with the
// bit set and no slot (it is stored set in the level, and vf14 drops the slot
// without clearing it): the step then uses slot 255, 0x50FEC bytes past the
// service, and writes a path point over whatever lies there. LV2-3's powder keg
// (by the doors, after the fight at x -55.9 .. -48.1, z -569.6) wrote it over a
// Roman's component pointer (+0x54), and the Roman's next move read address 5 or
// 6 (0x531CE5) -- every time, in single player too.
// Fix: a block ticked running without a slot asks for one first, the way its
// start does (bit 0x40 cleared, 10 tries).

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

const uintptr_t kPathBlocVtbl = 0x624D04, kPathBlocTick = 0x467660;
const int       kPathBlocTickSlot = 15;

typedef void(__fastcall* TickFn)(char* bloc, void* edx, int arg);   // thiscall, ret 4
TickFn g_origTick = nullptr;
char   g_logPath[MAX_PATH];
int    g_logged = 0;

void Log(const char* fmt, ...) {
	FILE* f = fopen(g_logPath, "a");
	if (!f) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

void __fastcall TickHook(char* bloc, void* edx, int arg) {
	uint8_t& flags = *(uint8_t*)(bloc + 0x2C);
	if (*(int8_t*)(bloc + 0x98) == -1 && (flags & 0x40)) {
		flags &= ~0x40;
		*(uint8_t*)(bloc + 0x118) = 10;
		if (g_logged < 50) {
			g_logged++;
			Log("path block %p ticked running without a path slot: asks for one first", bloc);
		}
	}
	g_origTick(bloc, edx, arg);
}

bool PatchSlot(uintptr_t vtbl, int slot, uintptr_t expected, void* hook, void** orig) {
	void** p = (void**)(vtbl + slot * 4);
	if ((uintptr_t)*p != expected) return false;
	DWORD old;
	if (!VirtualProtect(p, 4, PAGE_READWRITE, &old)) return false;
	*orig = *p;
	*p = hook;
	VirtualProtect(p, 4, old, &old);
	return true;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hinst, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(hinst);
		GetModuleFileNameA(nullptr, g_logPath, MAX_PATH);
		char* slash = strrchr(g_logPath, '\\');
		strcpy(slash ? slash + 1 : g_logPath, "xxl_cutscenefix.log");
		DeleteFileA(g_logPath);
		bool ok = PatchSlot(kPathBlocVtbl, kPathBlocTickSlot, kPathBlocTick, (void*)TickHook, (void**)&g_origTick);
		Log(ok ? "xxl_cutscenefix: path block tick hooked (0x%X)" : "xxl_cutscenefix: not this game build (0x%X) -- off",
		    (unsigned)kPathBlocTick);
	}
	return TRUE;
}
