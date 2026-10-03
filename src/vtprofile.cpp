#include "vtprofile.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

namespace {

// Counts the call and records who made it, then tail-jumps to the original.
// EAX is caller-saved and never an argument under thiscall/fastcall/cdecl, and
// it is restored anyway; flags are dead on entry to a function.
//
//   FF 05 <&counter>     inc  dword ptr [counter]
//   50                   push eax
//   8B 44 24 04          mov  eax, [esp+4]        ; return address
//   A3 <&caller>         mov  [caller], eax
//   58                   pop  eax
//   FF 25 <&original>    jmp  dword ptr [original]
#pragma pack(push, 1)
struct Thunk {
	uint8_t  incOp[2];
	uint32_t counterAddr;
	uint8_t  pushEax;
	uint8_t  movEaxRet[4];
	uint8_t  movCallerOp;
	uint32_t callerAddr;
	uint8_t  popEax;
	uint8_t  jmpOp[2];
	uint32_t origSlotAddr;
};
#pragma pack(pop)

struct Profile {
	bool       active = false;
	char       name[64] = {};
	uintptr_t  vtable = 0;
	int        numSlots = 0;
	Thunk*     thunks = nullptr;
	uintptr_t  original[kMaxProfiledSlots] = {};
	uint32_t   counter[kMaxProfiledSlots] = {};
	uint32_t   lastCounter[kMaxProfiledSlots] = {};
	uint32_t   caller[kMaxProfiledSlots] = {};   // return address of the last call
};

// Several vtables can be profiled at once; test sessions are expensive, so it
// is worth covering both the hero and its Life object in one run.
const int kMaxProfiles = 4;
Profile g_profiles[kMaxProfiles];

} // namespace

bool VtProfileInstall(uintptr_t vtable, int numSlots, const char* name) {
	Profile* slot = nullptr;
	for (Profile& p : g_profiles)
		if (!p.active) { slot = &p; break; }
	if (!slot) { Log("vtprof: no free profile slot for %s", name); return false; }
	Profile& g_profile = *slot;

	if (numSlots <= 0 || numSlots > kMaxProfiledSlots) {
		Log("vtprof: bad slot count %d", numSlots);
		return false;
	}

	g_profile.thunks = (Thunk*)VirtualAlloc(nullptr, sizeof(Thunk) * numSlots,
	                                        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!g_profile.thunks) {
		Log("vtprof: VirtualAlloc failed (%lu)", GetLastError());
		return false;
	}

	uintptr_t* slots = (uintptr_t*)vtable;
	DWORD oldProtect = 0;
	if (!VirtualProtect(slots, numSlots * sizeof(uintptr_t), PAGE_READWRITE, &oldProtect)) {
		Log("vtprof: cannot unprotect vtable %08X (%lu)", (unsigned)vtable, GetLastError());
		return false;
	}

	for (int i = 0; i < numSlots; i++) {
		g_profile.original[i] = slots[i];

		Thunk& t = g_profile.thunks[i];
		t.incOp[0] = 0xFF; t.incOp[1] = 0x05;
		t.counterAddr = (uint32_t)(uintptr_t)&g_profile.counter[i];
		t.pushEax = 0x50;
		t.movEaxRet[0] = 0x8B; t.movEaxRet[1] = 0x44;
		t.movEaxRet[2] = 0x24; t.movEaxRet[3] = 0x04;
		t.movCallerOp = 0xA3;
		t.callerAddr = (uint32_t)(uintptr_t)&g_profile.caller[i];
		t.popEax = 0x58;
		t.jmpOp[0] = 0xFF; t.jmpOp[1] = 0x25;
		t.origSlotAddr = (uint32_t)(uintptr_t)&g_profile.original[i];

		slots[i] = (uintptr_t)&t;
	}

	VirtualProtect(slots, numSlots * sizeof(uintptr_t), oldProtect, &oldProtect);
	FlushInstructionCache(GetCurrentProcess(), g_profile.thunks, sizeof(Thunk) * numSlots);

	strncpy(g_profile.name, name, sizeof(g_profile.name) - 1);
	g_profile.vtable = vtable;
	g_profile.numSlots = numSlots;
	g_profile.active = true;
	Log("vtprof: profiling %s vtable %08X, %d slots", name, (unsigned)vtable, numSlots);
	return true;
}

void VtProfileReport() {
	for (Profile& p : g_profiles) {
		if (!p.active) continue;
		char storage[512];
		LineBuf line(storage);
		line.Add("vtprof %s:", p.name);
		int shown = 0;
		for (int i = 0; i < p.numSlots; i++) {
			uint32_t now = p.counter[i];
			uint32_t delta = now - p.lastCounter[i];
			p.lastCounter[i] = now;
			if (!delta) continue;
			if (shown < 10)
				line.Add(" [%d]+%u fn=%08X from=%08X",
				         i, delta, (unsigned)p.original[i], p.caller[i]);
			shown++;
		}
		if (!shown) line.Add(" no slot called");
		Log("%s", line.c_str());
	}
}
