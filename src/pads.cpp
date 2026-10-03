#include "pads.h"
#include "config.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cmath>

// Layout of XINPUT_STATE / XINPUT_GAMEPAD, redeclared so we do not need the
// XInput headers (the VS Build Tools SDK has them, but binding at runtime keeps
// the mod working on machines with only xinput9_1_0.dll).
struct XI_GAMEPAD {
	uint16_t wButtons;
	uint8_t  bLeftTrigger, bRightTrigger;
	int16_t  sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
struct XI_STATE {
	uint32_t   dwPacketNumber;
	XI_GAMEPAD Gamepad;
};

typedef uint32_t(WINAPI* PFN_XInputGetState)(uint32_t, XI_STATE*);

static HMODULE            g_xinput = nullptr;
static PFN_XInputGetState g_getState = nullptr;
static PadState           g_pads[kMaxPads];
static PadState           g_virtual[kMaxPads];
static bool               g_virtualActive[kMaxPads];

bool PadsInit() {
	static const char* const kCandidates[] = {
		"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", "xinput1_2.dll", "xinput1_1.dll"
	};
	for (const char* name : kCandidates) {
		g_xinput = LoadLibraryA(name);
		if (g_xinput) {
			g_getState = (PFN_XInputGetState)GetProcAddress(g_xinput, "XInputGetState");
			if (g_getState) {
				Log("pads: using %s", name);
				return true;
			}
			FreeLibrary(g_xinput);
			g_xinput = nullptr;
		}
	}
	Log("pads: no usable XInput DLL found -- player 2 cannot join");
	return false;
}

void PadsShutdown() {
	if (g_xinput) { FreeLibrary(g_xinput); g_xinput = nullptr; }
	g_getState = nullptr;
}

// Radial deadzone: scales the surviving magnitude back over the full 0..1 range
// so the stick does not jump when it crosses the threshold.
static void ApplyDeadzone(int16_t rawX, int16_t rawY, float dz, float* outX, float* outY) {
	float x = rawX / 32767.0f;
	float y = rawY / 32767.0f;
	if (x < -1.0f) x = -1.0f;
	if (y < -1.0f) y = -1.0f;
	float mag = sqrtf(x * x + y * y);
	if (mag <= dz || mag <= 0.0f) { *outX = 0.0f; *outY = 0.0f; return; }
	float scaled = (mag - dz) / (1.0f - dz);
	if (scaled > 1.0f) scaled = 1.0f;
	*outX = x / mag * scaled;
	*outY = y / mag * scaled;
}

void PadsSetVirtual(int i, const PadState* st) {
	if (i < 0 || i >= kMaxPads) return;
	g_virtualActive[i] = st != nullptr;
	if (st) g_virtual[i] = *st;
}

void PadsPoll() {
	for (int i = 0; i < kMaxPads; i++) {
		PadState& p = g_pads[i];
		if (g_virtualActive[i]) {
			const PadState& v = g_virtual[i];
			uint16_t prevButtons = p.buttons;
			if (!p.connected) Log("pads: pad %d connected (virtual)", i);
			p.connected = true;
			p.buttons   = v.buttons;
			p.pressed   = (uint16_t)(p.buttons & ~prevButtons);
			p.released  = (uint16_t)(prevButtons & ~p.buttons);
			p.lx = v.lx; p.ly = v.ly; p.rx = v.rx; p.ry = v.ry;
			p.lt = v.lt; p.rt = v.rt;
			continue;
		}
		if (!g_getState) continue;
		XI_STATE st = {};
		uint32_t res = g_getState((uint32_t)i, &st);
		uint16_t prev = p.buttons;
		if (res != 0 /* ERROR_SUCCESS */) {
			if (p.connected) Log("pads: pad %d disconnected", i);
			p = PadState();
			// Report the release edge so a held button does not stick.
			p.released = prev;
			continue;
		}
		if (!p.connected) Log("pads: pad %d connected", i);
		p.connected = true;
		p.buttons   = st.Gamepad.wButtons;
		p.pressed   = (uint16_t)(p.buttons & ~prev);
		p.released  = (uint16_t)(prev & ~p.buttons);
		p.lt = st.Gamepad.bLeftTrigger / 255.0f;
		p.rt = st.Gamepad.bRightTrigger / 255.0f;
		ApplyDeadzone(st.Gamepad.sThumbLX, st.Gamepad.sThumbLY, g_config.deadzone, &p.lx, &p.ly);
		ApplyDeadzone(st.Gamepad.sThumbRX, st.Gamepad.sThumbRY, g_config.deadzone, &p.rx, &p.ry);
	}
}

const PadState& PadGet(int i) {
	static const PadState empty;
	if (i < 0 || i >= kMaxPads) return empty;
	return g_pads[i];
}

int PadFindPressed(uint16_t button, int excludeIndex) {
	for (int i = 0; i < kMaxPads; i++) {
		if (i == excludeIndex) continue;
		if (g_pads[i].connected && (g_pads[i].pressed & button)) return i;
	}
	return -1;
}
