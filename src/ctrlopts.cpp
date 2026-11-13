#include "ctrlopts.h"
#include "config.h"
#include "coop.h"
#include "detour.h"
#include "hints.h"
#include "icons.h"
#include "inputmgr.h"
#include "kal.h"
#include "log.h"
#include "p2cam.h"
#include "pads.h"
#include "trio.h"

#include <intrin.h>

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>

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

// The mod's rows. Page 3 (the game's third): Switch hero before Vibration,
// Invert Camera X / Y after it; page 4 (the mod's, in page 3's container, its
// rows shown instead of page 3's): Deadzone. Kinds: a toggle (YES / NO: left,
// right, confirm), a number (left / right step it), a binding (confirm, then the
// next button or key -- the controller or the keyboard, as the page's Current
// Configuration says; Escape / B keeps the old one).
enum RowKind { RK_TOGGLE, RK_NUMBER, RK_BIND };
enum RowId { R_SWITCH, R_INVX, R_INVY, R_DEADZONE, kNumRows };
struct RowDef { const wchar_t* name; RowKind kind; bool page4; };
const RowDef kRows[kNumRows] = {
	{ L"Switch hero", RK_BIND, false },
	{ L"Invert Camera X", RK_TOGGLE, false },
	{ L"Invert Camera Y", RK_TOGGLE, false },
	{ L"Deadzone", RK_NUMBER, true },
};
const float kPage3Step = 0.075f;    // page 3's nine rows, 0.842 down to 0.242
const float kPage4Step = 0.075f;
const uint32_t kLocPageCur = 0x39;   // "Page %CtrlPage_Cur%/%CtrlPage_Nb%" (string 0x390; the count is the data's: CtrlOptsLocString)

alignas(16) char g_label[kNumRows][0x6E0];
alignas(16) char g_value[kNumRows][0x6DC];
alignas(16) char g_colour[kNumRows][2][0x80];   // [row][label, value]
char* g_page = nullptr;    // the container they are in (null: out)
char* g_labels = nullptr;  // its child lists: the labels' (buttons), the values'
char* g_values = nullptr;
char* g_pageButton = nullptr;
DWORD g_pageTick = 0;      // the controls page's last tick
bool  g_page4 = false;     // page 4 shown (the game is on its page 3)
int   g_binding = -1;      // the row waiting for a button / key
uint16_t g_bindPrev = 0;   // the pad's buttons when the wait began
bool  g_keysPrev[256];
uint32_t g_ids[kNumRows][2];   // [row][label, value] string ids (HintsModString slots)

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

void SetLoc(uint32_t var, int value) {
	CKYellowPages* yp = YellowPages();
	char* loc = yp ? *(char**)((char*)yp + 0x94) : nullptr;
	if (loc) ((void(__fastcall*)(char*, void*, uint32_t, int))(*(void***)loc)[0x30 / 4])(loc, nullptr, var, value);
}

// Shown (+5 bit 1): drawn, and the focus stops on it.
void Show(char* e, bool on) {
	if (on) e[5] |= 2; else e[5] &= ~6;
}

bool IsPage4(char* e) {
	for (int i = 0; i < kNumRows; i++) if ((e == g_label[i] || e == g_value[i]) && kRows[i].page4) return true;
	return false;
}

// Page 3's rows or page 4's shown (the page button and the backing stay).
void ShowPage(bool page4) {
	if (!g_page) return;
	char* lists[2] = { g_labels, g_values };
	for (char* list : lists)
		for (char* c = *(char**)(list + 4); c; c = *(char**)(c + 0x10)) {
			uintptr_t vt = *(uintptr_t*)c;
			if (c == g_pageButton || (vt != kVtTextButton && vt != kVtText)) continue;
			Show(c, IsPage4(c) == page4);
		}
}

void TakeOut() {
	if (!g_page) return;
	ShowPage(false);
	for (int i = kNumRows - 1; i >= 0; i--) {
		if (InList(g_values, g_value[i])) Unlink(g_values, g_value[i]);
		if (InList(g_labels, g_label[i])) Unlink(g_labels, g_label[i]);
	}
	Log("ctrlopts: the mod's rows taken out of the controls");
	g_page = g_labels = g_values = g_pageButton = nullptr;
	g_page4 = false;
	g_binding = -1;
}

// The page's Current Configuration: 1 the controller, 0 the keyboard (+0x208).
bool OnPad(char* opt) { return *(uint8_t*)(opt + 0x208) == 1; }

const char* PadButtonName(uint16_t bit) {
	for (int i = 0; i < CoopConfig::kNumPadButtons; i++)
		if (CoopConfig::kPadButtons[i].bit == bit) return CoopConfig::kPadButtons[i].name;
	return "none";
}
const wchar_t* PadButtonTitle(uint16_t bit) {
	switch (bit) {
	case 0x0001: return L"D-pad Up";    case 0x0002: return L"D-pad Down";
	case 0x0004: return L"D-pad Left";  case 0x0008: return L"D-pad Right";
	case 0x0010: return L"Start";       case 0x0020: return L"Back";
	case 0x0040: return L"L3";          case 0x0080: return L"R3";
	case 0x0100: return L"LB";          case 0x0200: return L"RB";
	case 0x1000: return L"A";           case 0x2000: return L"B";
	case 0x4000: return L"X";           case 0x8000: return L"Y";
	}
	return L"None";
}
// The icon of a pad button (coop_icons pad_a, dpad_up...).
wchar_t PadButtonGlyph(uint16_t bit) {
	static const struct { uint16_t bit; const char* icon; } kIcons[] = {
		{ 0x0001, "dpad_up" }, { 0x0002, "dpad_down" }, { 0x0004, "dpad_left" }, { 0x0008, "dpad_right" },
		{ 0x0010, "pad_start" }, { 0x0020, "pad_back" }, { 0x0040, "pad_l3" }, { 0x0080, "pad_r3" },
		{ 0x0100, "pad_lb" }, { 0x0200, "pad_rb" }, { 0x1000, "pad_a" }, { 0x2000, "pad_b" },
		{ 0x4000, "pad_x" }, { 0x8000, "pad_y" },
	};
	for (const auto& k : kIcons) if (k.bit == bit) return IconGlyph(k.icon);
	return 0;
}

// A row's label and value texts (the label with its button's icon, as the game's rows).
void RowTexts(char* opt, int i, wchar_t* label, wchar_t* value, int n) {
	wcsncpy(label, kRows[i].name, n - 1);
	label[n - 1] = 0;
	value[0] = 0;
	switch (i) {
	case R_SWITCH: {
		bool pad = OnPad(opt);
		wchar_t g = pad ? PadButtonGlyph(g_config.spSwitchButton) : IconKeyGlyph(g_config.spSwitchKey);
		if (g) swprintf(label, n, L"%s %c", kRows[i].name, g);
		if (g_binding == i) wcsncpy(value, L"...", n - 1);
		else if (pad) wcsncpy(value, PadButtonTitle(g_config.spSwitchButton), n - 1);
		else if (g_config.spSwitchKey) IconsKeyName(g_config.spSwitchKey, value, n);
		else wcsncpy(value, L"None", n - 1);
		break;
	}
	case R_DEADZONE:
		swprintf(value, n, L"%.2f", g_config.p1Deadzone);
		break;
	}
}

void ShowRow(char* opt, int i) {
	wchar_t label[100], value[100];
	RowTexts(opt, i, label, value, 100);
	g_ids[i][0] = HintsModString(i * 2, label);
	((SetTextFn)0x42E590)(g_label[i] + 0x20, nullptr, g_ids[i][0]);
	uint32_t vid;
	if (kRows[i].kind == RK_TOGGLE) vid = (i == R_INVX ? g_config.invertCamX : g_config.invertCamY) ? kYes : kNo;
	else vid = g_ids[i][1] = HintsModString(i * 2 + 1, value);
	((SetTextFn)0x42E590)(g_value[i] + 0x20, nullptr, vid);
}

void WriteIni(const char* section, const char* key, const char* value) {
	if (g_config.iniPath[0]) WritePrivateProfileStringA(section, key, value, g_config.iniPath);
}

// A key's ini name: letters / digits / F-keys as such, the rest in hex.
void KeyIniName(int vk, char* out, size_t n) {
	if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) snprintf(out, n, "%c", vk);
	else if (vk >= VK_F1 && vk <= VK_F12) snprintf(out, n, "F%d", vk - VK_F1 + 1);
	else if (vk == VK_TAB) snprintf(out, n, "TAB");
	else if (vk == VK_SPACE) snprintf(out, n, "SPACE");
	else snprintf(out, n, "0x%02X", vk);
}

void MenuSound(char* opt) {
	((void(__fastcall*)(char*, void*, int))(*(void***)opt)[0x54 / 4])(opt, nullptr, 0);
}

// Left (-1), right (+1) or confirm (0) on row `i`.
void ChangeRow(char* opt, int i, int dir) {
	switch (kRows[i].kind) {
	case RK_TOGGLE: {
		bool& v = i == R_INVX ? g_config.invertCamX : g_config.invertCamY;
		v = !v;
		WriteIni("controls", i == R_INVX ? "invert_camera_x" : "invert_camera_y", v ? "on" : "off");
		Log("ctrlopts: %ls %s", kRows[i].name, v ? "on" : "off");
		break;
	}
	case RK_NUMBER: {
		if (!dir) return;
		float v = g_config.p1Deadzone + 0.02f * dir;
		v = v < 0.0f ? 0.0f : v > 0.9f ? 0.9f : v;
		v = floorf(v * 50.0f + 0.5f) / 50.0f;
		if (v == g_config.p1Deadzone) return;
		g_config.p1Deadzone = v;
		char b[16];
		snprintf(b, sizeof(b), "%.2f", v);
		WriteIni("controls", "p1_deadzone", b);
		Log("ctrlopts: %ls %s", kRows[i].name, b);
		break;
	}
	case RK_BIND: {
		if (dir) return;
		g_binding = i;
		int p = RumblePad(0);
		g_bindPrev = p >= 0 ? PadGet(p).buttons : 0xFFFF;
		for (int vk = 0; vk < 256; vk++) g_keysPrev[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
		Log("ctrlopts: %ls waits for a %s", kRows[i].name, OnPad(opt) ? "button" : "key");
		break;
	}
	}
	MenuSound(opt);
	ShowRow(opt, i);
}

// The wait for a button or key: the first new press after it began.
void BindFrame(char* opt) {
	if (g_binding < 0) return;
	int i = g_binding;
	bool done = false;
	if (OnPad(opt)) {
		int p = RumblePad(0);
		uint16_t now = p >= 0 ? PadGet(p).buttons : 0;
		uint16_t fresh = (uint16_t)(now & ~g_bindPrev);
		g_bindPrev &= now;   // released buttons count again
		if (fresh & PAD_B) done = true;   // B: keep the old one
		else if (fresh) {
			uint16_t bit = (uint16_t)(fresh & (0x10000 - fresh));
			g_config.spSwitchButton = bit;
			WriteIni("single_player", "sp_hero_switch_button", PadButtonName(bit));
			Log("ctrlopts: %ls -> %s", kRows[i].name, PadButtonName(bit));
			done = true;
		}
	} else {
		for (int vk = 8; vk < 256 && !done; vk++) {
			if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;
			bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
			bool fresh = down && !g_keysPrev[vk];
			g_keysPrev[vk] = down;
			if (!fresh) continue;
			if (vk != VK_ESCAPE) {
				g_config.spSwitchKey = vk;
				char b[16];
				KeyIniName(vk, b, sizeof(b));
				WriteIni("single_player", "sp_hero_switch_key", b);
				Log("ctrlopts: %ls -> key %s", kRows[i].name, b);
			}
			done = true;
		}
	}
	*(uint32_t*)(opt + 0x21C) = 2;   // the menu does nothing with this press
	if (!done) return;
	g_binding = -1;
	MenuSound(opt);
	ShowRow(opt, i);
}

// Page 3 with the mod's rows: copies of the Vibration label and value, linked
// in (Switch hero before Vibration, the rest after it), page 3's rows closed up
// from the top one, page 4's put at the top and hidden.
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
	char* beforeLabel = *(char**)(vibLabel + 0xC);
	char* beforeValue = *(char**)(vibValue + 0xC);
	if (!labels || !values || !beforeLabel || !beforeValue) {
		static int logs = 0;
		if (logs++ < 3) Log("ctrlopts: the Vibration row is not in page 3's lists -- no mod rows");
		return;
	}
	g_labels = labels;
	g_values = values;
	g_pageButton = pageButton;
	g_page = page;
	g_page4 = false;
	g_binding = -1;
	char* afterLabel = vibLabel;
	char* afterValue = vibValue;
	for (int i = 0; i < kNumRows; i++) {
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
		ShowRow(opt, i);
		if (i == R_SWITCH) {
			LinkAfter(labels, beforeLabel, g_label[i]);
			LinkAfter(values, beforeValue, g_value[i]);
		} else {
			LinkAfter(labels, afterLabel, g_label[i]);
			LinkAfter(values, afterValue, g_value[i]);
			afterLabel = g_label[i];
			afterValue = g_value[i];
		}
	}
	// Labels and values, each in list order: page 3's from the top row down, page 4's from the top too.
	float top = -1.0f;
	int n3 = 0, n4 = 0, v3 = 0, v4 = 0;
	for (char* c = *(char**)(labels + 4); c; c = *(char**)(c + 0x10)) {
		if (*(uintptr_t*)c != kVtTextButton || c == pageButton) continue;
		float* y = (float*)(c + 0x18);
		if (top < 0.0f) top = *y;
		*y = IsPage4(c) ? top - kPage4Step * n4++ : top - kPage3Step * n3++;
	}
	for (char* c = *(char**)(values + 4); c && top >= 0.0f; c = *(char**)(c + 0x10))
		if (*(uintptr_t*)c == kVtText) *(float*)(c + 0x18) = IsPage4(c) ? top - kPage4Step * v4++ : top - kPage3Step * v3++;
	SetLoc(kLocPageCur, *(int*)(opt + 0x20C) + 1);   // the counter's text again, with 4 pages
	Log("ctrlopts: the mod's rows put in the controls (page 3: %d rows, page 4: %d, from %.3f)", n3, n4, top);
}

// Page 4 in or out, the counter with it.
void SetPage4(char* opt, bool on) {
	g_page4 = on;
	ShowPage(on);
	SetLoc(kLocPageCur, on ? 4 : *(int*)(opt + 0x20C) + 1);
	Log("ctrlopts: page %d", on ? 4 : *(int*)(opt + 0x20C) + 1);
}

int __fastcall CtrlPageHook(char* opt, void* edx, uint32_t arg) {
	g_pageTick = GetTickCount();
	PutIn(opt);
	if (!g_page) return g_origCtrlPage(opt, edx, arg);
	BindFrame(opt);
	char* view = *(char**)(opt + 0x18);
	ViewActionFn act = (ViewActionFn)0x4358B0;
	auto pressed = [&](uint32_t a, uint32_t b) { return view && (act(view, nullptr, a) || act(view, nullptr, b)); };
	bool ready = !*(uint8_t*)(opt + 0x214) && !*(uint32_t*)(opt + 0x21C) && g_binding < 0;
	bool left = ready && pressed(2, 0x12), right = ready && pressed(3, 0x13), confirm = ready && view && act(view, nullptr, 4);
	int page = *(int*)(opt + 0x20C);
	char* pageButtonNow = page >= 0 && page < 3 ? *(char**)(opt + 0x1D0 + page * 4) : nullptr;
	bool onPageButton = pageButtonNow && (pageButtonNow[5] & 4);
	// The page button: 3 -> 4 -> 1 rightwards, 1 -> 4 -> 3 leftwards.
	bool toPage4After = false;
	if (onPageButton && page == 2 && !g_page4 && right) { SetPage4(opt, true); *(uint32_t*)(opt + 0x21C) = 1; }
	else if (onPageButton && page == 2 && g_page4 && left) { SetPage4(opt, false); *(uint32_t*)(opt + 0x21C) = 1; }
	else if (onPageButton && page == 2 && g_page4 && right) { g_page4 = false; ShowPage(false); }   // the game goes on to page 1
	else if (onPageButton && page == 0 && left) toPage4After = true;   // the game goes to page 3: page 4 instead
	int focused = -1;
	for (int i = 0; i < kNumRows; i++) if (g_label[i][5] & 4) focused = i;
	int r = g_origCtrlPage(opt, edx, arg);
	if (toPage4After && *(int*)(opt + 0x20C) == 2) SetPage4(opt, true);
	// The game shows a page's every child when it goes to it: keep page 3's or page 4's.
	if (*(int*)(opt + 0x20C) == 2) ShowPage(g_page4);
	else g_page4 = false;
	if (focused >= 0 && (g_label[focused][5] & 4) && (left || right || confirm))
		ChangeRow(opt, focused, left ? -1 : right ? 1 : 0);
	return r;
}

void __fastcall RowColoursHook(char* opt, void* edx, void* time, char* focused) {
	g_origRowColours(opt, edx, time, focused);
	if (!g_page || !focused) return;
	uint32_t pulse = *((uint32_t*(__fastcall*)(char*, void*))0x461A00)(opt + 0xB4, nullptr);
	uint32_t normal = *(uint32_t*)(opt + 0xB0);
	for (int i = 0; i < kNumRows; i++) {
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

// Player 1's sticks through the deadzone (p1_deadzone; the movement's actions
// 0-3 up / down / left / right, the camera's 46-49), then the camera's actions
// (the game's bindings 46 / 47 the right stick's up / down, 48 / 49 right /
// left) swapped for the Invert rows.
void Deadzone(uint8_t* v, int up, int down, int left, int right) {
	float x = (v[right] - v[left]) / 255.0f, y = (v[up] - v[down]) / 255.0f;
	float m = sqrtf(x * x + y * y);
	float dz = g_config.p1Deadzone;
	if (m <= 0.0f) return;
	float k = m <= dz ? 0.0f : ((m > 1.0f ? 1.0f : m) - dz) / (1.0f - dz) / m;
	x *= k; y *= k;
	v[right] = x > 0 ? (uint8_t)(x * 255.0f + 0.5f) : 0; v[left] = x < 0 ? (uint8_t)(-x * 255.0f + 0.5f) : 0;
	v[up] = y > 0 ? (uint8_t)(y * 255.0f + 0.5f) : 0;   v[down] = y < 0 ? (uint8_t)(-y * 255.0f + 0.5f) : 0;
}

void InvertFrame() {
	InputManager im = GetInputManager();
	uint8_t* v = im ? im.Values() : nullptr;
	if (!v || im.NumActions() < 50) return;
	if (g_config.p1Deadzone > 0.0f) {
		Deadzone(v, 0, 1, 2, 3);
		Deadzone(v, 46, 47, 49, 48);
	}
	if (g_config.invertCamY) { uint8_t t = v[46]; v[46] = v[47]; v[47] = t; }
	if (g_config.invertCamX) { uint8_t t = v[48]; v[48] = v[49]; v[49] = t; }
}

} // namespace

const wchar_t* CtrlOptsLocString(uint32_t id, const wchar_t* s) {
	static const wchar_t kVar[] = L"%CtrlPage_Nb%";
	if (id != 0x390 || !g_page || !s) return s;
	const wchar_t* at = wcsstr(s, kVar);
	if (!at) return s;
	static wchar_t buf[200];
	swprintf(buf, 200, L"%.*s4%s", (int)(at - s), s, at + wcslen(kVar));
	return buf;
}

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
