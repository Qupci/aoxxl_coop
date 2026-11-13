#include "ctrlopts.h"
#include "config.h"
#include "coop.h"
#include "detour.h"
#include "hints.h"
#include "inputmgr.h"
#include "kal.h"
#include "log.h"
#include "p2cam.h"
#include "pads.h"
#include "trio.h"

#include <intrin.h>

#include <windows.h>
#include <cstring>

namespace {

// ---- vibration ---------------------------------------------------------------

// CKInput's rumble calls (thiscall on CKInput; pad, motor 0/1, strength 0-255,
// seconds), each forwarding to the pad device's slot 10 / 11 / 12 unless CKInput
// +0x68 is set; the device plays its DirectInput effect (+0xC4) only with the
// options' Vibration on (CKInput +0x15 = 1). FUN_00438B30(pad): does the pad
// have that effect -- the controls page skips the Vibration row without it.
typedef int(__fastcall* MotorOnFn)(char* in, void* edx, uint32_t pad, uint32_t motor, uint32_t strength);
typedef int(__fastcall* MotorTimedFn)(char* in, void* edx, uint32_t pad, uint32_t motor, uint32_t strength, float secs);
typedef int(__fastcall* MotorOffFn)(char* in, void* edx, uint32_t pad, uint32_t motor);
typedef uint32_t(__fastcall* HasRumbleFn)(char* in, void* edx, int pad);
const uintptr_t kMotorOn = 0x437E60, kMotorTimed = 0x437EA0, kMotorOff = 0x437EE0, kHasRumble = 0x438B30;
const uint8_t kMotorOnPrologue[5] = { 0x8A, 0x41, 0x68, 0x84, 0xC0 };
const uint8_t kMotorOffPrologue[7] = { 0x8B, 0x54, 0x24, 0x04, 0x8B, 0x49, 0x04 };
MotorOnFn    g_origMotorOn = nullptr;
MotorTimedFn g_origMotorTimed = nullptr;
MotorOffFn   g_origMotorOff = nullptr;
HasRumbleFn  g_origHasRumble = nullptr;

struct Motor { uint8_t strength; bool timed; DWORD until; };
Motor g_motors[2][2];      // [player][motor]
int   g_rumblePad[2] = { -1, -1 };   // the XInput pad last told, per player
int   g_rumbleLogs = 0;

// Whose rumble a call is: the hero lives' own code (Asterix FUN_004A4090 /
// FUN_004A3EF0, Obelix FUN_004A7430 / FUN_004A72A0) is that hero's; the rest
// (the trio's potion start FUN_0047DE80 / FUN_00480290, the machine gun, the
// boat, the mobile tower) the leader's. Then the player who has that hero.
struct Range { uintptr_t from, to; int hero; };
const Range kHeroCode[] = {
	{ 0x4A4090, 0x4A4090 + 0xCA7, HERO_ASTERIX }, { 0x4A3EF0, 0x4A3EF0 + 0x14E, HERO_ASTERIX },
	{ 0x4A7430, 0x4A7430 + 0xCE7, HERO_OBELIX },  { 0x4A72A0, 0x4A72A0 + 0x185, HERO_OBELIX },
};
int PlayerOfCall(uintptr_t from) {
	if (!CoopP2Joined()) return 0;
	int hero = -1;
	for (const Range& r : kHeroCode) if (from >= r.from && from < r.to) hero = r.hero;
	if (hero < 0) {
		Trio t = GetTrio();
		hero = t ? t.Leader() : CoopP1Hero();
	}
	return hero == CoopP1Hero() ? 0 : 1;
}

void NoteMotor(const char* what, int player, uintptr_t from, uint32_t motor, uint32_t strength, float secs) {
	if (g_rumbleLogs >= 30) return;
	g_rumbleLogs++;
	Log("ctrlopts: rumble %s for player %d (from %06X) motor %u strength %u%s (%.2f s)", what, player + 1, (unsigned)from, motor,
	    strength, secs >= 0.0f ? " timed" : "", secs);
}

int __fastcall MotorOnHook(char* in, void* edx, uint32_t pad, uint32_t motor, uint32_t strength) {
	if (!*(uint8_t*)(in + 0x68) && (pad & 0xFF) == 0) {
		uintptr_t from = (uintptr_t)_ReturnAddress();
		int pl = PlayerOfCall(from);
		Motor& m = g_motors[pl][(motor & 0xFF) > 1 ? 0 : motor & 1];
		m.strength = (uint8_t)strength;
		m.timed = false;
		NoteMotor("on", pl, from, motor & 0xFF, strength & 0xFF, -1.0f);
	}
	return g_origMotorOn(in, edx, pad, motor, strength);
}

// The device's own rule (FUN_00432EF0): an idle motor starts for `secs`; a
// timed one already running only has its end put later.
int __fastcall MotorTimedHook(char* in, void* edx, uint32_t pad, uint32_t motor, uint32_t strength, float secs) {
	if (!*(uint8_t*)(in + 0x68) && (pad & 0xFF) == 0) {
		uintptr_t from = (uintptr_t)_ReturnAddress();
		int pl = PlayerOfCall(from);
		Motor& m = g_motors[pl][(motor & 0xFF) > 1 ? 0 : motor & 1];
		DWORD until = GetTickCount() + (DWORD)(secs > 0.0f && secs < 60.0f ? secs * 1000.0f : 0.0f);
		if (!m.strength) {
			m.strength = (uint8_t)strength;
			m.timed = true;
			m.until = until;
			NoteMotor("on", pl, from, motor & 0xFF, strength & 0xFF, secs);
		} else if (m.timed && (int)(until - m.until) > 0) {
			m.until = until;
		}
	}
	return g_origMotorTimed(in, edx, pad, motor, strength, secs);
}

// Off for both players: a leader change between on and off must not leave one running.
int __fastcall MotorOffHook(char* in, void* edx, uint32_t pad, uint32_t motor) {
	if ((pad & 0xFF) == 0)
		for (int pl = 0; pl < 2; pl++) g_motors[pl][(motor & 0xFF) > 1 ? 0 : motor & 1] = Motor();
	return g_origMotorOff(in, edx, pad, motor);
}

// The XInput pad a player rumbles: player 1's the one the game reads (not
// identified yet: the first pad that is not player 2's), player 2's its own
// (none on the keyboard).
int RumblePad(int player) {
	if (player == 1) {
		int p = CoopP2Joined() ? CoopP2Pad() : -1;
		return p >= 0 && p < kXInputPads && PadGet(p).connected ? p : -1;
	}
	int p = Player1Pad();
	if (p >= 0 && p < kXInputPads && PadGet(p).connected) return p;
	int p2 = CoopP2Joined() ? CoopP2Pad() : -1;
	for (int i = 0; i < kXInputPads; i++) if (i != p2 && PadGet(i).connected) return i;
	return -1;
}

uint32_t __fastcall HasRumbleHook(char* in, void* edx, int pad) {
	uint32_t r = g_origHasRumble(in, edx, pad);
	if (!(r & 0xFF) && pad == 0 && RumblePad(0) >= 0) r = (r & ~0xFFu) | 1;
	return r;
}

void RumbleFrame() {
	InputManager im = GetInputManager();
	DWORD now = GetTickCount();
	for (auto& player : g_motors)
		for (Motor& m : player)
			if (m.strength && m.timed && (int)(now - m.until) >= 0) m = Motor();
	if (!CoopP2Joined()) g_motors[1][0] = g_motors[1][1] = Motor();
	CKYellowPages* yp = YellowPages();
	char* gm = yp ? (char*)yp->gameManager : nullptr;
	bool paused = gm && (*(uint8_t*)(gm + 0xCC) & 3) == 2;
	bool on = im && *(uint8_t*)(im.raw() + 0x15) == 1 && !paused;
	for (int pl = 0; pl < 2; pl++) {
		int pad = RumblePad(pl);
		if (pad != g_rumblePad[pl] && g_rumblePad[pl] >= 0 && g_rumblePad[pl] != RumblePad(1 - pl)) PadsSetRumble(g_rumblePad[pl], 0, 0);
		g_rumblePad[pl] = pad;
		if (pad < 0) continue;
		uint16_t l = on ? (uint16_t)(g_motors[pl][0].strength * 257) : 0, r = on ? (uint16_t)(g_motors[pl][1].strength * 257) : 0;
		static uint32_t last[2];
		if (((uint32_t)l << 16 | r) != last[pl] && g_rumbleLogs < 60) {
			g_rumbleLogs++;
			Log("ctrlopts: player %d's pad %d motors %u / %u (Vibration %s)", pl + 1, pad, l, r, on ? "on" : "off or paused");
		}
		last[pl] = (uint32_t)l << 16 | r;
		PadsSetRumble(pad, l, r);
	}
}

// ---- Invert Camera X / Y -------------------------------------------------------

// The controls page's tick (CKHkInterfaceOption::vf18 case 1, thiscall(arg),
// ret 4). The interface: +0x18 its input view (actions 2 / 0x12 left, 3 / 0x13
// right, 4 confirm), +0x4C the Vibration label (+0x58 its value text), +0x1CC
// the third page's element id, +0x1D8 that page's "Page 3/3" button, +0x214
// waiting for a key, +0x21C frames until input counts again, vf21 (+0x54) the
// menu's sound. Elements: 2D objects with +5 bit 2 focus, +0xC / +0x10 the
// previous / next in their container's list (container +0x24 the buttons,
// +0x28 the rest -> list +4 first, +8 last, +0xC count), +0x14 / +0x18 their
// place, the text layout
// embedded at +0x20 (set its string with FUN_0042E590, thiscall(id)). A row's
// label is a CColorTextButton2d (0x6E0 bytes), its value a CText2d (0x6DC):
// their only pointers into themselves are the links and the layout's word
// pointers, which setting the string makes again. Each also has its own colour
// object (+0x68C, 0x80 bytes: focus sets the focused colour +0x690 in it, the
// normal one +0x6DC) -- shared, the copies lit and dimmed with Vibration; so a
// copy gets its own, and then is a new row.
typedef int(__fastcall* CtrlPageFn)(char* opt, void* edx, uint32_t arg);
typedef void(__fastcall* SetTextFn)(char* layout, void* edx, uint32_t id);
typedef char(__fastcall* ViewActionFn)(char* view, void* edx, uint32_t action);
typedef char*(__fastcall* MenuElementFn)(char* menus, void* edx, uint32_t id);
const uintptr_t kCtrlPage = 0x527030;
const uint8_t   kCtrlPagePrologue[8] = { 0x83, 0xEC, 0x6C, 0xA1, 0xF4, 0x21, 0x66, 0x00 };
const uintptr_t kVtTextButton = 0x62390C, kVtText = 0x623B6C;
const uint32_t  kYes = 0x1E, kNo = 0x1F;   // the Vibration row's values
const float     kRowStep = 0.08f;          // page 3's rows closed up to fit two more
CtrlPageFn g_origCtrlPage = nullptr;
// The rows' colours after each tick (FUN_00527960, thiscall(time, focused),
// ret 8): every row it knows (labels and values) back to the normal colour
// (+0xB0), then the focused row's label and value given the pulsing one
// (FUN_00461A00 on +0xB4 -> it), the value found by a fixed list (Vibration:
// +0x4C -> +0x58) -- the copies' labels and values are done here. A text layout's colour: its vf10 (+0x28), the colour by value.
typedef void(__fastcall* RowColoursFn)(char* opt, void* edx, void* time, char* focused);
typedef void(__fastcall* SetColourFn)(char* layout, void* edx, uint32_t rgba);
const uintptr_t kRowColours = 0x527960;
const uint8_t   kRowColoursPrologue[7] = { 0x83, 0xEC, 0x10, 0x53, 0x55, 0x56, 0x57 };
RowColoursFn g_origRowColours = nullptr;

alignas(16) char g_label[2][0x6E0];
alignas(16) char g_value[2][0x6DC];
alignas(16) char g_colour[2][2][0x80];   // [row][label, value]
char* g_page = nullptr;    // the container they are in (null: out)
char* g_labels = nullptr;  // its child lists: the labels' (buttons), the values'
char* g_values = nullptr;
DWORD g_pageTick = 0;      // the controls page's last tick
uint32_t g_labelIds[2];

bool InList(char* list, char* e) {
	int k = 0;
	for (char* c = *(char**)(list + 4); c && k < 200; c = *(char**)(c + 0x10), k++) if (c == e) return true;
	return false;
}

void Unlink(char* list, char* e) {
	char* prev = *(char**)(e + 0xC);
	char* next = *(char**)(e + 0x10);
	if (prev) *(char**)(prev + 0x10) = next; else *(char**)(list + 4) = next;
	if (next) *(char**)(next + 0xC) = prev; else *(char**)(list + 8) = prev;
	*(int*)(list + 0xC) -= 1;
	*(char**)(e + 0xC) = *(char**)(e + 0x10) = nullptr;
}

void LinkAfter(char* list, char* at, char* e) {
	char* next = *(char**)(at + 0x10);
	*(char**)(e + 0xC) = at;
	*(char**)(e + 0x10) = next;
	*(char**)(at + 0x10) = e;
	if (next) *(char**)(next + 0xC) = e; else *(char**)(list + 8) = e;
	*(int*)(list + 0xC) += 1;
}

void TakeOut() {
	if (!g_page) return;
	for (int i = 1; i >= 0; i--) {
		if (InList(g_values, g_value[i])) Unlink(g_values, g_value[i]);
		if (InList(g_labels, g_label[i])) Unlink(g_labels, g_label[i]);
	}
	Log("ctrlopts: the Invert Camera rows taken out of the controls' page 3");
	g_page = g_labels = g_values = nullptr;
}

bool On(int i) { return i ? g_config.invertCamY : g_config.invertCamX; }

void ShowValue(int i) {
	((SetTextFn)0x42E590)(g_value[i] + 0x20, nullptr, On(i) ? kYes : kNo);
}

// Page 3 with the two rows: the Vibration label and value copied, linked in
// after them, and the page's rows closed up from the top one.
void PutIn(char* opt) {
	CKYellowPages* yp = YellowPages();
	char* m2d = yp ? *(char**)((char*)yp + 0x34) : nullptr;
	char* menus = m2d ? *(char**)(m2d + 0x3C) : nullptr;
	if (!menus) return;
	char* page = ((MenuElementFn)0x42A720)(menus, nullptr, *(uint32_t*)(opt + 0x1CC));
	char* vibLabel = *(char**)(opt + 0x4C);
	char* vibValue = *(char**)(opt + 0x58);
	char* pageButton = *(char**)(opt + 0x1D8);
	if (!page || !vibLabel || !vibValue || *(uintptr_t*)vibLabel != kVtTextButton || *(uintptr_t*)vibValue != kVtText) return;
	if (g_page == page && InList(g_labels, g_label[0])) return;
	g_page = nullptr;
	char* labels = nullptr;
	char* values = nullptr;
	for (int o = 0x24; o <= 0x28; o += 4) {
		char* l = *(char**)(page + o);
		if (l && !labels && InList(l, vibLabel)) labels = l;
		if (l && !values && InList(l, vibValue)) values = l;
	}
	if (!labels || !values) {
		static int logs = 0;
		if (logs++ < 3) Log("ctrlopts: the Vibration row is not in page 3's lists -- no Invert Camera rows");
		return;
	}
	g_labels = labels;
	g_values = values;
	g_page = page;
	static const wchar_t* const kNames[2] = { L"Invert Camera X", L"Invert Camera Y" };
	char* afterLabel = vibLabel;
	char* afterValue = vibValue;
	for (int i = 0; i < 2; i++) {
		memcpy(g_label[i], vibLabel, sizeof(g_label[i]));
		memcpy(g_value[i], vibValue, sizeof(g_value[i]));
		char* rows[2] = { g_label[i], g_value[i] };
		for (int k = 0; k < 2; k++) {
			char*& colour = *(char**)(rows[k] + 0x68C);
			if (!colour) continue;
			memcpy(g_colour[i][k], colour, sizeof(g_colour[i][k]));
			colour = g_colour[i][k];
		}
		g_label[i][5] &= ~4;   // not focused
		g_labelIds[i] = HintsModString(i, kNames[i]);
		((SetTextFn)0x42E590)(g_label[i] + 0x20, nullptr, g_labelIds[i]);
		ShowValue(i);
		LinkAfter(labels, afterLabel, g_label[i]);
		LinkAfter(values, afterValue, g_value[i]);
		afterLabel = g_label[i];
		afterValue = g_value[i];
	}
	// Labels and values, each in list order, from the top row down.
	float top = -1.0f;
	int nl = 0, nv = 0;
	for (char* c = *(char**)(labels + 4); c; c = *(char**)(c + 0x10)) {
		if (*(uintptr_t*)c != kVtTextButton || c == pageButton) continue;
		float* y = (float*)(c + 0x18);
		if (top < 0.0f) top = *y;
		*y = top - kRowStep * nl++;
	}
	for (char* c = *(char**)(values + 4); c && top >= 0.0f; c = *(char**)(c + 0x10))
		if (*(uintptr_t*)c == kVtText) *(float*)(c + 0x18) = top - kRowStep * nv++;
	Log("ctrlopts: Invert Camera X / Y rows put in the controls' page 3 (%d labels, %d values from %.3f; lists %s)", nl, nv, top,
	    labels == values ? "one" : "two");
}

int __fastcall CtrlPageHook(char* opt, void* edx, uint32_t arg) {
	g_pageTick = GetTickCount();
	PutIn(opt);
	int focused = -1;
	for (int i = 0; i < 2 && g_page; i++) if (g_label[i][5] & 4) focused = i;
	bool ready = !*(uint8_t*)(opt + 0x214) && !*(uint32_t*)(opt + 0x21C);
	int r = g_origCtrlPage(opt, edx, arg);
	if (focused < 0 || !ready || !(g_label[focused][5] & 4)) return r;
	char* view = *(char**)(opt + 0x18);
	ViewActionFn act = (ViewActionFn)0x4358B0;
	bool flip = false;
	static const uint32_t kFlip[5] = { 2, 0x12, 3, 0x13, 4 };   // left, right, confirm
	for (uint32_t a : kFlip) flip |= view && act(view, nullptr, a) != 0;
	if (!flip) return r;
	((void(__fastcall*)(char*, void*, int))(*(void***)opt)[0x54 / 4])(opt, nullptr, 0);   // the menu's sound
	bool& v = focused ? g_config.invertCamY : g_config.invertCamX;
	v = !v;
	ShowValue(focused);
	if (g_config.iniPath[0])
		WritePrivateProfileStringA("controls", focused ? "invert_camera_y" : "invert_camera_x", v ? "on" : "off", g_config.iniPath);
	Log("ctrlopts: Invert Camera %c %s", focused ? 'Y' : 'X', v ? "on" : "off");
	return r;
}

void __fastcall RowColoursHook(char* opt, void* edx, void* time, char* focused) {
	g_origRowColours(opt, edx, time, focused);
	if (!g_page || !focused) return;
	uint32_t pulse = *((uint32_t*(__fastcall*)(char*, void*))0x461A00)(opt + 0xB4, nullptr);
	uint32_t normal = *(uint32_t*)(opt + 0xB0);
	for (int i = 0; i < 2; i++) {
		char* layout = g_value[i] + 0x20;
		((SetColourFn)(*(void***)layout)[0x28 / 4])(layout, nullptr, focused == g_label[i] ? pulse : normal);
		// The label: the game resets only the rows it knows (FUN_00527D40) -- a
		// copy kept the pulse it had when the focus left it, brighter than the rest.
		if (focused != g_label[i]) {
			layout = g_label[i] + 0x20;
			((SetColourFn)(*(void***)layout)[0x28 / 4])(layout, nullptr, normal);
		}
	}
}

// The camera's actions (the game's bindings 46 / 47 the right stick's up /
// down, 48 / 49 right / left) swapped for player 1.
void InvertFrame() {
	InputManager im = GetInputManager();
	uint8_t* v = im ? im.Values() : nullptr;
	if (!v || im.NumActions() < 50) return;
	if (g_config.invertCamY) { uint8_t t = v[46]; v[46] = v[47]; v[47] = t; }
	if (g_config.invertCamX) { uint8_t t = v[48]; v[48] = v[49]; v[49] = t; }
}

} // namespace

float CtrlOptsCamX() { return g_config.invertCamX ? -1.0f : 1.0f; }
float CtrlOptsCamY() { return g_config.invertCamY ? -1.0f : 1.0f; }

void CtrlOptsFrame() {
	InvertFrame();
	RumbleFrame();
	if (g_page && GetTickCount() - g_pageTick > 100) TakeOut();
}

bool CtrlOptsInstallHooks() {
	bool ok = true;
	ok &= InstallDetour(kMotorOn, (void*)MotorOnHook, (void**)&g_origMotorOn, kMotorOnPrologue, sizeof(kMotorOnPrologue),
	                    "rumble on (XInput vibration)");
	ok &= InstallDetour(kMotorTimed, (void*)MotorTimedHook, (void**)&g_origMotorTimed, kMotorOnPrologue,
	                    sizeof(kMotorOnPrologue), "rumble timed (XInput vibration)");
	ok &= InstallDetour(kMotorOff, (void*)MotorOffHook, (void**)&g_origMotorOff, kMotorOffPrologue, sizeof(kMotorOffPrologue),
	                    "rumble off (XInput vibration)");
	ok &= InstallDetour(kHasRumble, (void*)HasRumbleHook, (void**)&g_origHasRumble, kMotorOffPrologue,
	                    sizeof(kMotorOffPrologue), "pad rumble check (Vibration row with XInput)");
	ok &= InstallDetour(kCtrlPage, (void*)CtrlPageHook, (void**)&g_origCtrlPage, kCtrlPagePrologue, sizeof(kCtrlPagePrologue),
	                    "controls page (Invert Camera rows)");
	ok &= InstallDetour(kRowColours, (void*)RowColoursHook, (void**)&g_origRowColours, kRowColoursPrologue,
	                    sizeof(kRowColoursPrologue), "options rows' colours (Invert Camera values)");
	return ok;
}
