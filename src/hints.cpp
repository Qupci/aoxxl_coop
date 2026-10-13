#include "hints.h"
#include "coop.h"
#include "config.h"
#include "detour.h"
#include "inputmgr.h"
#include "log.h"
#include "pads.h"

#include <cstdio>
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
float g_textWidth = 0.0f;        // the shown hint's width (a share of the screen)
float g_savedX = 0.0f;

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

void __fastcall InterfaceTickHook(char* self, void* edx, void* time) {
	CKYellowPages* yp = YellowPages();
	char* gm = yp ? (char*)yp->gameManager : nullptr;
	const Hint* h = Current();
	uint32_t* cc = gm ? (uint32_t*)(gm + 0xCC) : nullptr;
	bool use = h && cc && ((*cc >> 4) & 0xF) == kPromptNone;
	if (cc && getenv("XXL_HINTLOG")) LogKinds(self);
	uint32_t* slot = (uint32_t*)(self + 0x1A4 + kHintKind * 4);
	uint32_t savedId = *slot;
	if (use) {
		if (wcscmp(g_shown, h->text) != 0) g_textWidth = 0.0f;
		wcsncpy(g_shown, h->text, 95);
		g_shown[95] = 0;
		*slot = kHintStringId;
		*cc = (*cc & ~0xF0u) | (kHintKind << 4);
	}
	if (use != g_showing) {
		char a[96] = {};
		for (int i = 0; use && g_shown[i] && i < 95; i++) a[i] = g_shown[i] < 128 ? (char)g_shown[i] : '?';
		Log("hints: %s%s%s", use ? "showing \"" : "hint gone", use ? a : "", use ? "\"" : "");
		g_showing = use;
	}
	// The prompt's text (the object at +0x15C, laid out from +0x20 by
	// FUN_0042E590 / FUN_0042EF80) is one line of a box +0x28 wide (a share of
	// the screen; the prompt also slides in by it): sized for "Use", a hint's
	// words past it were dropped. A hint gets a wider box.
	char* text = *(char**)(self + 0x15C);
	float savedWidth = text ? *(float*)(text + 0x28) : 0.0f;
	if (use && text) {
		static bool logged = false;
		if (!logged) {
			logged = true;
			Log("hints: prompt text box %.3f x %.3f", *(float*)(text + 0x28), *(float*)(text + 0x2C));
		}
		// ... and the prompt's place on screen (+0x428, where it slides to;
		// +0x42C / +0x434 its target and position while shown, state +0x24
		// bits 21-22 == 2) moves left by the width it needs.
		float w = g_textWidth > 0.0f ? g_textWidth : 0.5f;
		float x = 1.0f - 1.15f * w - 0.04f;   // the drawn line runs a little wider than measured
		if (x < 0.02f) x = 0.02f;
		if (x > *(float*)(self + 0x428)) x = *(float*)(self + 0x428);
		*(float*)(text + 0x28) = 1.0f - x;
		g_savedX = *(float*)(self + 0x428);
		*(float*)(self + 0x428) = x;
		if (((*(uint32_t*)(self + 0x24) >> 21) & 3) == 2) *(float*)(self + 0x42C) = *(float*)(self + 0x434) = x;
	}
	g_origInterfaceTick(self, edx, time);
	if (use && text) {
		*(float*)(text + 0x28) = savedWidth;
		*(float*)(self + 0x428) = g_savedX;
		// The width of the words just laid out (as the interface measures them:
		// FUN_005FDAB0(font, word, size) + spacing, text +0x20 fields).
		char* t20 = text + 0x20;
		typedef float(__cdecl* WidthFn)(void* font, const wchar_t* word, uint32_t size);
		unsigned n = *(uint32_t*)(t20 + 0x2D4);
		void* font = *(void**)(t20 + 0x660);
		float sum = 0.0f;
		for (unsigned i = 0; font && i < n && i < 64; i++)
			sum += ((WidthFn)0x5FDAB0)(font, *(const wchar_t**)(t20 + 0x2D8 + 4 * i), *(uint32_t*)(t20 + 0x664)) +
			       *(float*)(t20 + 0x668);
		if (sum > 0.0f) g_textWidth = sum;
	}
	if (use) {
		*slot = savedId;
		if (((*cc >> 4) & 0xF) == kHintKind) *cc = (*cc & ~0xF0u) | (kPromptNone << 4);
		// The button sprite (+0x140, the prompt's dark plate with the button) is
		// sized for one word and its button means nothing here: hidden (vf17).
		char* icon = *(char**)(self + 0x140);
		if (icon) ((void(__fastcall*)(char*, void*))(*(void***)icon)[17])(icon, nullptr);
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
	static const wchar_t* const kPad[HB_COUNT] = { L"A", L"X", L"LB", L"Start", L"Back" };
	static wchar_t buf[HB_COUNT][2][32];
	if (b < 0 || b >= HB_COUNT) return L"?";
	if (player == 1) {
		if (CoopP2Pad() != kKeyboardPad) return kPad[b];
		static const int kKey[HB_COUNT] = { CoopConfig::KEY_JUMP, CoopConfig::KEY_ATTACK, CoopConfig::KEY_LB,
		                                    CoopConfig::KEY_START, CoopConfig::KEY_BACK };
		KeyName(g_config.keys[kKey[b]], buf[b][1], 32);
		return buf[b][1];
	}
	// Player 1: the device the game's binding for that button reads.
	static const int kAction[HB_COUNT] = { 4, 5, 8, 15, 12 };
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
