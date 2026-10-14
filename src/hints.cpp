#include "hints.h"
#include "coop.h"
#include "config.h"
#include "detour.h"
#include "hud2.h"
#include "individual.h"
#include "inputmgr.h"
#include "log.h"
#include "pads.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace {

// The localisation manager's string lookup (thiscall(loc, id), ret 4).
typedef const wchar_t*(__fastcall* LocStringFn)(void* loc, void* edx, uint32_t id);
const uintptr_t kLocString = 0x411780;
const uint8_t   kLocStringPrologue[8] = { 0x8B, 0x54, 0x24, 0x04, 0x56, 0x83, 0xFA, 0xFF };
LocStringFn g_origLocString = nullptr;
const uint32_t kHintStringId = 0x3F00C0DE;   // no table of the game's goes that far

// The in-game interface's tick (thiscall(this, time), ret 4).
typedef void(__fastcall* InterfaceTickFn)(char* self, void* edx, void* time);
const uintptr_t kInGameVtbl = 0x6285F0;
const size_t    kInGameTickSlot = 18;
const uintptr_t kInGameTick = 0x5180E0;
InterfaceTickFn g_origInterfaceTick = nullptr;
const unsigned  kPromptNone = 0xE;
const unsigned  kHintKind = 0;   // the prompt kind a hint borrows

struct Hint { int key; wchar_t text[96]; DWORD until; DWORD since; };
Hint g_hints[8];
int  g_nHints = 0;
wchar_t g_shown[96];             // what the lookup answers for kHintStringId
bool g_showing = false;
bool g_loggedKinds = false;
wchar_t g_source[96];            // the hint being shown, as given
int     g_sourceKey = 0;          // ... and its key
int     g_nLines = 1;
unsigned g_lineWord[8];          // the first word of each line
float   g_longest = 0.0f;        // its longest line's width (a share of the screen)
float   g_lineDir = 1.0f;        // XXL_HINTDIR=-1: lines go up
float   g_unitsPerScreen = 1.75f; // text layout units across the screen (XXL_HINTUNITS)
float   g_boxShift = -0.39f;      // where the text starts, from the prompt's place (XXL_HINTSHIFT)

const wchar_t* __fastcall LocStringHook(void* loc, void* edx, uint32_t id) {
	if (id == kHintStringId) return g_shown;
	return g_origLocString(loc, edx, id);
}

const Hint* Current() {
	DWORD now = GetTickCount();
	const Hint* best = nullptr;
	for (int i = 0; i < g_nHints; i++)
		if ((int)(g_hints[i].until - now) > 0 && (!best || (int)(g_hints[i].since - best->since) > 0)) best = &g_hints[i];
	return best;
}

void LogKinds(char* self) {
	if (g_loggedKinds) return;
	g_loggedKinds = true;
	char* text = *(char**)(self + 0x15C);
	void* loc = text ? *(void**)(text + 4) : nullptr;
	if (!loc) return;
	for (int k = 0; k < 15; k++) {
		uint32_t id = *(uint32_t*)(self + 0x1A4 + k * 4);
		const wchar_t* s = g_origLocString(loc, nullptr, id);
		char a[64] = {};
		for (int i = 0; s && s[i] && i < 63; i++) a[i] = s[i] < 128 ? (char)s[i] : '?';
		Log("hints: prompt kind %d = string %08X \"%s\"", k, id, a);
	}
}

// The prompt's text object (+0x15C) is laid out from +0x20 (FUN_0042E590:
// words split at spaces, +0x2D4 count, +0x2D8 pointers; FUN_0042EF80: lines of a
// box +0x28 wide, a share of the screen, each word's place at +0x3C8 + 8i, x / y)
// and the interface puts it at +0x428 (where it slides to; +0x42C / +0x434 its
// target and place while shown, state +0x24 bits 21-22 == 2). Sized for "Use":
// a hint is laid out on one line of a wide box, then its words are moved onto
// the hint's own lines ('\n'), right-aligned, and the prompt moved left by the
// longest line.
char*   g_plate = nullptr;         // the plate while a hint has it, and its own height and place
float   g_plateY = 0.0f, g_plateH = 0.0f;

void PlateBack(char* self) {
	if (!g_plate) return;
	if (g_plate != *(char**)(self + 0x140)) { g_plate = nullptr; return; }   // another level's interface
	((float*)g_plate)[6] = g_plateY;
	((float*)g_plate)[14] = g_plateH;
	g_plate = nullptr;
}

void Relayout(char* t20) {
	unsigned n = *(uint32_t*)(t20 + 0x2D4);
	if (n == 0 || n > 64 || g_nLines < 2) return;
	float* pos = (float*)(t20 + 0x3C8);
	CKYellowPages* yp = YellowPages();
	char* screen = yp ? *(char**)((char*)yp + 0x34) : nullptr;
	float scale = screen ? *(float*)(screen + 4) * 0.75f : 0.75f;
	typedef float(__cdecl* WidthFn)(void* font, const wchar_t* word, uint32_t size);
	void* font = *(void**)(t20 + 0x660);
	if (!font) return;
	float x0[64], x1[64];
	for (unsigned i = 0; i < n; i++) {
		x0[i] = pos[2 * i];
		x1[i] = x0[i] + scale * ((WidthFn)0x5FDAB0)(font, *(const wchar_t**)(t20 + 0x2D8 + 4 * i), *(uint32_t*)(t20 + 0x664));
	}
	float start[8], width[8], longest = 0.0f;
	for (int l = 0; l < g_nLines; l++) {
		unsigned f = g_lineWord[l], e = l + 1 < g_nLines ? g_lineWord[l + 1] : n;
		if (f >= n || e <= f) { start[l] = 0; width[l] = 0; continue; }
		start[l] = x0[f];
		width[l] = x1[e - 1 < n ? e - 1 : n - 1] - x0[f];
		if (width[l] > longest) longest = width[l];
	}
	float y0 = pos[1];
	float step = *(float*)(t20 + 0x664) * (1.0f + *(float*)(t20 + 0x6A8));
	for (int l = 0; l < g_nLines; l++) {
		unsigned f = g_lineWord[l], e = l + 1 < g_nLines ? g_lineWord[l + 1] : n;
		for (unsigned i = f; i < e && i < n; i++) {
			pos[2 * i] = (x0[i] - start[l]) + (longest - width[l]);
			pos[2 * i + 1] = y0 - step * l * g_lineDir;
		}
	}
	g_longest = longest;
}

void __fastcall InterfaceTickHook(char* self, void* edx, void* time) {
	CKYellowPages* yp = YellowPages();
	char* gm = yp ? (char*)yp->gameManager : nullptr;
	const Hint* h = Current();
	uint32_t* cc = gm ? (uint32_t*)(gm + 0xCC) : nullptr;
	char* text = *(char**)(self + 0x15C);
	bool use = h && cc && text && ((*cc >> 4) & 0xF) == kPromptNone;
	if (cc && getenv("XXL_HINTLOG")) LogKinds(self);
	uint32_t* slot = (uint32_t*)(self + 0x1A4 + kHintKind * 4);
	uint32_t savedId = *slot;
	if (use && wcscmp(g_source, h->text) != 0) {
		// A new hint: its lines become one ('\n' -> ' '), the first word of each
		// line noted.
		bool sameKey = g_showing && g_sourceKey == h->key;   // the same hint, updated (a progress bar)
		g_sourceKey = h->key;
		wcsncpy(g_source, h->text, 95);
		g_source[95] = 0;
		g_nLines = 1;
		g_lineWord[0] = 0;
		unsigned words = 0, k = 0;
		for (const wchar_t* c = g_source; *c && k < 95; c++) {
			if (*c == L'\n') {
				if (g_nLines < 8) g_lineWord[g_nLines++] = words + 1;
				g_shown[k++] = L' ';
				words++;
			} else {
				if (*c == L' ') words++;
				g_shown[k++] = *c;
			}
		}
		g_shown[k] = 0;
		g_longest = 0.0f;
		// A new hint over a prompt already on screen (state 2: the gun's "Use"
		// when Asterix takes it) keeps that prompt's place: shown afresh (state
		// 0), it slides in to the hint's.
		uint32_t& st = *(uint32_t*)(self + 0x24);
		if (((st >> 21) & 3) != 0 && !sameKey) st &= ~(3u << 21);
	}
	if (use) {
		*slot = kHintStringId;
		*cc = (*cc & ~0xF0u) | (kHintKind << 4);
	}
	if (use != g_showing) {
		char a[96] = {};
		for (int i = 0; use && g_shown[i] && i < 95; i++) a[i] = g_shown[i] < 128 ? (char)g_shown[i] : '?';
		Log("hints: %s%s%s", use ? "showing \"" : "hint gone", use ? a : "", use ? "\"" : "");
		g_showing = use;
	}
	float savedWidth = text ? *(float*)(text + 0x28) : 0.0f;
	float savedX = *(float*)(self + 0x428);
	if (use) {
		// Text layout units: the screen's width is the aspect ratio (yellow
		// pages +0x34 -> +4: 1.778 at 16:9).
		char* screen = yp ? *(char**)((char*)yp + 0x34) : nullptr;
		if (screen && *(float*)(screen + 4) > 0.5f && !getenv("XXL_HINTUNITS")) g_unitsPerScreen = *(float*)(screen + 4);
		*(float*)(text + 0x28) = 2.0f;   // one line
		float w = (g_longest > 0.0f ? g_longest : 0.9f) / g_unitsPerScreen;
		// The text starts g_boxShift off the prompt's place with the 2.0 box.
		float x = 0.95f - w - g_boxShift;
		*(float*)(self + 0x428) = x;
		uint32_t state = (*(uint32_t*)(self + 0x24) >> 21) & 3;
		if (state == 2) *(float*)(self + 0x42C) = *(float*)(self + 0x434) = x;
		else if (state == 1) *(float*)(self + 0x42C) = x;   // sliding in: to the measured place
	}
	if (!use) PlateBack(self);
	{
		// Individual heroes: the HUD (top left, player 1's half) is player 1's
		// hero's health, shields and combo bar.
		Hud2BeforeInterfaceTick(self);
		int shopper = CoopShopper();   // at the merchant: the shopper's own shields
		int prev = IndivEnter(shopper >= 0 ? shopper : CoopP1Hero());
		g_origInterfaceTick(self, edx, time);
		IndivLeave(prev);
		Hud2AfterInterfaceTick(self, time);   // player 2's own health and combo bar
	}
	if (!use) return;
	*(float*)(text + 0x28) = savedWidth;
	*(float*)(self + 0x428) = savedX;
	char* t20 = text + 0x20;
	if (g_nLines >= 2) {
		Relayout(t20);
	} else {   // one line: its width for the placement
		unsigned n = *(uint32_t*)(t20 + 0x2D4);
		float* pos = (float*)(t20 + 0x3C8);
		CKYellowPages* y2 = YellowPages();
		char* screen = y2 ? *(char**)((char*)y2 + 0x34) : nullptr;
		float scale = screen ? *(float*)(screen + 4) * 0.75f : 0.75f;
		typedef float(__cdecl* WidthFn)(void* font, const wchar_t* word, uint32_t size);
		void* font = *(void**)(t20 + 0x660);
		if (n && n <= 64 && font) {
			float first = pos[0];
			g_longest = pos[2 * (n - 1)] - first +
			            scale * ((WidthFn)0x5FDAB0)(font, *(const wchar_t**)(t20 + 0x2D8 + 4 * (n - 1)), *(uint32_t*)(t20 + 0x664));
			for (unsigned i = 0; i < n; i++) pos[2 * i] -= first;
		}
	}
	static DWORD lastLog = 0;
	if (getenv("XXL_HINTLOG") && GetTickCount() - lastLog > 1000) {
		lastLog = GetTickCount();
		float* pos = (float*)(t20 + 0x3C8);
		char* screen0 = yp ? *(char**)((char*)yp + 0x34) : nullptr;
		Log("hints: screen scale %.3f (yellow pages +0x34 -> +4)", screen0 ? *(float*)(screen0 + 4) : -1.0f);
		Log("hints: words %u lines %d longest %.3f, word 0 at %.3f %.3f, last at %.3f %.3f, size %.3f spacing %.3f, state %d",
		    *(uint32_t*)(t20 + 0x2D4), g_nLines, g_longest, pos[0], pos[1],
		    pos[2 * (*(uint32_t*)(t20 + 0x2D4) - 1)], pos[2 * (*(uint32_t*)(t20 + 0x2D4) - 1) + 1],
		    *(float*)(t20 + 0x664), *(float*)(t20 + 0x6A8), (*(uint32_t*)(self + 0x24) >> 21) & 3);
	}
	*slot = savedId;
	if (((*cc >> 4) & 0xF) == kHintKind) *cc = (*cc & ~0xF0u) | (kPromptNone << 4);
	char* icon = *(char**)(self + 0x140);
	// The prompt's dark plate (+0x140, a CBillboard2d) is drawn from its place
	// (+0x14 / +0x18) leftwards by +0x34, +0x38 high; the game puts it at the
	// text's place (sized for one word, its button past +0x34). For a hint it
	// covers the hint's lines: its right end just past the text's (which starts
	// g_boxShift off the text's place), as wide as the longest line, a line's
	// height each, its middle moved down to theirs. The game carries its height
	// and place over from frame to frame: put back when the hint goes
	// (PlateBack).
	if (icon) {
		float* f = (float*)icon;
		if (g_plate != icon) { g_plate = icon; g_plateY = f[6]; g_plateH = f[14]; }
		const float m = 0.02f;
		float w = (g_longest > 0.0f ? g_longest : 0.9f) / g_unitsPerScreen;
		f[5] = *(float*)(text + 0x14) + g_boxShift + w + m;
		f[13] = w + 2.0f * m;
		f[14] = g_plateH * g_nLines + 0.015f;
		f[6] = g_plateY - 0.012f * (g_nLines - 1);
	}
	if (icon && getenv("XXL_HINTLOG")) {
		static DWORD lastIconLog = 0;
		if (GetTickCount() - lastIconLog > 2000) {
			lastIconLog = GetTickCount();
			float* f = (float*)icon;
			Log("hints: plate %p vt %p: +14 %.3f +18 %.3f +1C %.3f +20 %.3f +24 %.3f +28 %.3f +2C %.3f +30 %.3f +34 %.3f +38 %.3f +3C %.3f +40 %.3f",
			    icon, *(void**)icon, f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15], f[16]);
		}
	}
}

// Keyboard keys: the game's keyboard device (CKInput +0x0C, 0x3FC bytes) keeps
// one byte per key, written by its window-message handler (vtable slot 5,
// thiscall(vk, lparam, down), ret 0xC). Which byte a virtual key lands on is
// found by pressing each key once on the device with its bytes saved and put
// back.
uint8_t g_vkOfByte[0x400];
bool    g_vkMapped = false;

void MapKeyboard(char* kb) {
	g_vkMapped = true;
	static uint8_t saved[0x3FC];
	typedef void(__fastcall* KeyFn)(char* dev, void* edx, uint32_t vk, uint32_t lparam, uint32_t down);
	KeyFn key = (KeyFn)(*(void***)kb)[5];
	for (uint32_t vk = 1; vk < 255; vk++) {
		memcpy(saved, kb, sizeof(saved));
		key(kb, nullptr, vk, 0, 1);
		for (int i = 4; i < 0x3FC; i++)   // past the vtable
			if (kb[i] != (char)saved[i] && !g_vkOfByte[i]) { g_vkOfByte[i] = (uint8_t)vk; break; }
		memcpy(kb, saved, sizeof(saved));
	}
}

void KeyName(int vk, wchar_t* out, int n) {
	UINT sc = MapVirtualKeyW(vk, 0 /* MAPVK_VK_TO_VSC */);
	LONG lp = (LONG)(sc << 16);
	if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT ||
	    vk == VK_HOME || vk == VK_END || vk == VK_INSERT || vk == VK_DELETE)
		lp |= 1 << 24;
	if (!sc || !GetKeyNameTextW(lp, out, n)) swprintf(out, n, L"key %02X", vk);
}

} // namespace

const wchar_t* HintButtonName(int player, HintButton b) {
	static const wchar_t* const kPad[HB_COUNT] = { L"A", L"X", L"LB", L"Start", L"Back", L"B" };
	static wchar_t buf[HB_COUNT][2][32];
	if (b < 0 || b >= HB_COUNT) return L"?";
	if (player == 1) {
		if (CoopP2Pad() != kKeyboardPad) return kPad[b];
		static const int kKey[HB_COUNT] = { CoopConfig::KEY_JUMP, CoopConfig::KEY_ATTACK, CoopConfig::KEY_LB,
		                                    CoopConfig::KEY_START, CoopConfig::KEY_BACK, CoopConfig::KEY_GRAB };
		KeyName(g_config.keys[kKey[b]], buf[b][1], 32);
		return buf[b][1];
	}
	// Player 1: the device the game's binding for that button reads.
	static const int kAction[HB_COUNT] = { 4, 5, 8, 15, 12, 6 };
	InputManager im = GetInputManager();
	char* in = im ? im.raw() : nullptr;
	if (!in || kAction[b] >= im.NumActions()) return kPad[b];
	char* src = *(char**)(im.Binding(kAction[b]) + 0xC);
	char* cur = src ? *(char**)(src + 0x14) : nullptr;
	char* kb = *(char**)(in + 0x0C);
	unsigned nkb = *(uint8_t*)(in + 0x12);
	if (!cur || !kb || !nkb || cur < kb || cur >= kb + 0x3FC) return kPad[b];
	if (!g_vkMapped) MapKeyboard(kb);
	int vk = g_vkOfByte[cur - kb];
	if (!vk) return kPad[b];
	KeyName(vk, buf[b][0], 32);
	return buf[b][0];
}

const wchar_t* HintKeyName(int vk) {
	static wchar_t buf[32];
	KeyName(vk, buf, 32);
	return buf;
}

void HintShow(int key, const wchar_t* text, DWORD ms) {
	DWORD now = GetTickCount();
	Hint* h = nullptr;
	for (int i = 0; i < g_nHints && !h; i++) if (g_hints[i].key == key) h = &g_hints[i];
	if (!h) {
		if (g_nHints == 8) {   // the oldest goes
			int o = 0;
			for (int i = 1; i < 8; i++) if ((int)(g_hints[i].since - g_hints[o].since) < 0) o = i;
			h = &g_hints[o];
		} else {
			h = &g_hints[g_nHints++];
		}
		h->since = now;
	} else if (wcscmp(h->text, text) != 0 || (int)(h->until - now) <= 0) {
		h->since = now;
	}
	h->key = key;
	wcsncpy(h->text, text, 95);
	h->text[95] = 0;
	h->until = now + ms;
}

void HintHide(int key) {
	for (int i = 0; i < g_nHints; i++)
		if (g_hints[i].key == key) g_hints[i] = g_hints[--g_nHints];
}

bool HintShown(int key) {
	DWORD now = GetTickCount();
	for (int i = 0; i < g_nHints; i++)
		if (g_hints[i].key == key && (int)(g_hints[i].until - now) > 0) return true;
	return false;
}

void HintsFrame() {
	DWORD now = GetTickCount();
	static bool env = false;
	if (!env) {
		env = true;
		if (getenv("XXL_HINTDIR")) g_lineDir = (float)atof(getenv("XXL_HINTDIR"));
		if (getenv("XXL_HINTUNITS")) g_unitsPerScreen = (float)atof(getenv("XXL_HINTUNITS"));
		if (getenv("XXL_HINTSHIFT")) g_boxShift = (float)atof(getenv("XXL_HINTSHIFT"));
	}
	for (int i = 0; i < g_nHints;)
		if ((int)(g_hints[i].until - now) <= 0) g_hints[i] = g_hints[--g_nHints];
		else i++;
}

bool HintsInstallHooks() {
	bool ok = InstallDetour(kLocString, (void*)LocStringHook, (void**)&g_origLocString, kLocStringPrologue,
	                        sizeof(kLocStringPrologue), "localised strings (co-op hints)");
	ok &= PatchVtableSlot(kInGameVtbl, kInGameTickSlot, kInGameTick, (void*)InterfaceTickHook,
	                      (void**)&g_origInterfaceTick, "CKHkInterfaceInGame::tick (co-op hints)");
	return ok;
}
