#include "pads.h"
#include "config.h"
#include "log.h"
#include "kal.h"

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

static bool GameHasFocus() {
	HWND fg = GetForegroundWindow();
	DWORD pid = 0;
	if (fg) GetWindowThreadProcessId(fg, &pid);
	return pid == GetCurrentProcessId();
}

bool PadsKeyPressed(int vk) {
	static bool s_down[256];
	if (vk <= 0 || vk > 255) return false;
	bool down = GameHasFocus() && (GetAsyncKeyState(vk) & 0x8000) != 0;
	bool edge = down && !s_down[vk];
	s_down[vk] = down;
	return edge;
}

// While player 2 plays on the keyboard, their keys must not also reach the game,
// which reads the keyboard from WM_KEYDOWN/WM_KEYUP in its window procedure
// (0x479C10 -> FUN_00438990): with player 1 on the keyboard too, one key would
// drive both heroes. Key-downs of player 2's keys are swallowed; key-ups always
// pass, so a key held at the moment of joining (Start) is released in the game.
static WNDPROC g_prevProc = nullptr;
static HWND    g_hwnd = nullptr;
static bool    g_keysOwnedByP2 = false;

static bool IsP2Key(WPARAM vk, LPARAM lp) {
	const int* k = g_config.keys;
	bool ext = (lp & (1 << 24)) != 0;
	for (int i = 0; i < CoopConfig::KEY_COUNT; i++) {
		int key = k[i];
		if (key <= 0) continue;
		if ((WPARAM)key == vk) return true;
		// The window procedure gets the generic shift/control/alt codes.
		if (vk == VK_SHIFT && (key == VK_LSHIFT || key == VK_RSHIFT)) {
			UINT sc = (UINT)((lp >> 16) & 0xFF);
			UINT lsc = MapVirtualKeyA(VK_LSHIFT, 0 /* MAPVK_VK_TO_VSC */);
			if ((key == VK_LSHIFT) == (sc == lsc)) return true;
		}
		if (vk == VK_CONTROL && (key == (ext ? VK_RCONTROL : VK_LCONTROL))) return true;
		if (vk == VK_MENU && (key == (ext ? VK_RMENU : VK_LMENU))) return true;
	}
	return false;
}

static LRESULT CALLBACK KeyFilterProc(HWND h, UINT m, WPARAM w, LPARAM l) {
	if (g_keysOwnedByP2 && (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && IsP2Key(w, l)) return 0;
	return CallWindowProcA(g_prevProc, h, m, w, l);
}

static void InstallKeyFilter() {
	if (g_prevProc || !g_config.keyboard) return;
	HWND hwnd = GameStartInfo()->hwnd;
	if (!hwnd || !IsWindow(hwnd)) return;
	g_hwnd = hwnd;
	g_prevProc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)KeyFilterProc);
	Log("pads: keyboard filter installed on window %p", hwnd);
}

void PadsSetKeyboardOwnedByP2(bool owned) {
	if (owned != g_keysOwnedByP2)
		Log("pads: player 2's keys %s", owned ? "now drive only player 2 (kept from the game)"
		                                       : "go to the game again");
	g_keysOwnedByP2 = owned;
}

static bool KeyDown(int vk) { return vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0; }

// XXL Inspector (behind the QoL patch as apd3d9.dll, or as the game's d3d9.dll)
// exports XXLInspectorWantsKeyboard while one of its text fields has the
// keyboard: typing coordinates there must not press player 2's keys (Enter
// was Start: player 2 joined on the keyboard).
static bool InspectorTyping() {
	typedef int(__cdecl* WantsFn)();
	static WantsFn fn = nullptr;
	static DWORD lookedAt = 0;
	DWORD now = GetTickCount();
	if (!fn && (lookedAt == 0 || now - lookedAt > 5000)) {
		lookedAt = now;
		const char* names[] = { "apd3d9.dll", "d3d9.dll" };
		for (const char* n : names) {
			HMODULE m = GetModuleHandleA(n);
			if (m && (fn = (WantsFn)GetProcAddress(m, "XXLInspectorWantsKeyboard")) != nullptr) {
				Log("pads: XXL Inspector found (%s): its text fields keep the keyboard", n);
				break;
			}
		}
	}
	return fn && fn() != 0;
}

static void PollKeyboard(PadState& p) {
	uint16_t prev = p.buttons;
	if (!g_config.keyboard) {
		p = PadState();
		p.released = prev;
		return;
	}
	if (!p.connected) Log("pads: keyboard available as a controller (slot %d)", kKeyboardPad);
	p.connected = true;
	const int* k = g_config.keys;
	// Enter ends an Inspector edit on its key-down: the field lets go of the
	// keyboard while the key is still held, and it read as player 2's Start.
	// After typing, keys count again only once every one has been let go.
	static bool afterTyping = false;
	bool typing = InspectorTyping();
	if (typing) afterTyping = true;
	else if (afterTyping) {
		bool any = false;
		for (int i = 0; i < CoopConfig::KEY_COUNT; i++) any |= KeyDown(k[i]);
		if (!any && !KeyDown(VK_RETURN) && !KeyDown(VK_ESCAPE) && !KeyDown(VK_TAB)) afterTyping = false;
	}
	bool focus = GameHasFocus() && !typing && !afterTyping;
	auto down = [&](int idx) { return focus && KeyDown(k[idx]); };
	uint16_t b = 0;
	if (down(CoopConfig::KEY_JUMP))   b |= PAD_A;
	if (down(CoopConfig::KEY_ATTACK)) b |= PAD_X;
	if (down(CoopConfig::KEY_GRAB))   b |= PAD_B;
	if (down(CoopConfig::KEY_DASH))   b |= PAD_Y;
	if (down(CoopConfig::KEY_LB))     b |= PAD_LB;
	if (down(CoopConfig::KEY_RB))     b |= PAD_RB;
	if (down(CoopConfig::KEY_START))  b |= PAD_START;
	if (down(CoopConfig::KEY_BACK))   b |= PAD_BACK;
	if (down(CoopConfig::KEY_CENTER_CAMERA)) b |= PAD_RTHUMB;
	p.buttons  = b;
	p.pressed  = (uint16_t)(b & ~prev);
	p.released = (uint16_t)(prev & ~b);
	float x = (down(CoopConfig::KEY_RIGHT) ? 1.0f : 0.0f) - (down(CoopConfig::KEY_LEFT) ? 1.0f : 0.0f);
	float y = (down(CoopConfig::KEY_UP) ? 1.0f : 0.0f) - (down(CoopConfig::KEY_DOWN) ? 1.0f : 0.0f);
	float m = sqrtf(x * x + y * y);
	p.lx = m > 0 ? x / m : 0.0f;
	p.ly = m > 0 ? y / m : 0.0f;
	p.rx = (down(CoopConfig::KEY_LOOK_RIGHT) ? 1.0f : 0.0f) - (down(CoopConfig::KEY_LOOK_LEFT) ? 1.0f : 0.0f);
	p.ry = (down(CoopConfig::KEY_LOOK_UP) ? 1.0f : 0.0f) - (down(CoopConfig::KEY_LOOK_DOWN) ? 1.0f : 0.0f);
	p.lt = p.rt = 0.0f;
}

const char* PadName(int i) {
	static char buf[4][16];
	static int slot = 0;
	if (i == kKeyboardPad) return "the keyboard";
	char* out = buf[slot++ & 3];
	wsprintfA(out, "pad %d", i);
	return out;
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
		if (i == kKeyboardPad) { InstallKeyFilter(); PollKeyboard(p); continue; }
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
