#include "detour.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>

bool InstallDetour(uintptr_t target, void* hook, void** outTrampoline,
                   const uint8_t* expected, size_t prologueLen, const char* name) {
	if (prologueLen < 5 || prologueLen > 32) {
		Log("detour: %s bad prologue length %u", name, (unsigned)prologueLen);
		return false;
	}

	uint8_t* code = (uint8_t*)target;
	if (memcmp(code, expected, prologueLen) != 0) {
		char got[128] = {}, want[128] = {};
		for (size_t i = 0; i < prologueLen; i++) {
			sprintf(got + i * 3, "%02X ", code[i]);
			sprintf(want + i * 3, "%02X ", expected[i]);
		}
		Log("detour: %s prologue mismatch at %08X -- not patching\n"
			"        got  %s\n        want %s", name, (unsigned)target, got, want);
		return false;
	}

	// Trampoline: displaced bytes, then jmp back past the patch.
	uint8_t* tramp = (uint8_t*)VirtualAlloc(nullptr, prologueLen + 5,
	                                        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!tramp) {
		Log("detour: %s VirtualAlloc failed (%lu)", name, GetLastError());
		return false;
	}
	memcpy(tramp, code, prologueLen);
	tramp[prologueLen] = 0xE9;
	*(uint32_t*)(tramp + prologueLen + 1) =
		(uint32_t)((target + prologueLen) - ((uintptr_t)tramp + prologueLen + 5));

	DWORD oldProtect = 0;
	if (!VirtualProtect(code, prologueLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
		Log("detour: %s cannot unprotect %08X (%lu)", name, (unsigned)target, GetLastError());
		VirtualFree(tramp, 0, MEM_RELEASE);
		return false;
	}
	code[0] = 0xE9;
	*(uint32_t*)(code + 1) = (uint32_t)((uintptr_t)hook - (target + 5));
	for (size_t i = 5; i < prologueLen; i++) code[i] = 0x90;   // pad with NOPs
	VirtualProtect(code, prologueLen, oldProtect, &oldProtect);

	FlushInstructionCache(GetCurrentProcess(), code, prologueLen);
	FlushInstructionCache(GetCurrentProcess(), tramp, prologueLen + 5);

	*outTrampoline = tramp;
	Log("detour: %s %08X -> %p (trampoline %p)", name, (unsigned)target, hook, tramp);
	return true;
}
