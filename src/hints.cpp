#include "hints.h"
#include "coop.h"
#include "config.h"
#include "detour.h"
#include "hud2.h"
#include "icons.h"
#include "individual.h"
#include "inputmgr.h"
#include "log.h"
#include "pads.h"
#include "splitscreen.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <intrin.h>
#include <cstring>
#include <cwchar>

namespace {

// The localisation manager's string lookup (thiscall(loc, id), ret 4).
typedef const wchar_t*(__fastcall* LocStringFn)(void* loc, void* edx, uint32_t id);
const uintptr_t kLocString = 0x411780;
const uint8_t   kLocStringPrologue[8] = { 0x8B, 0x54, 0x24, 0x04, 0x56, 0x83, 0xFA, 0xFF };
LocStringFn g_origLocString = nullptr;
// The mod's own strings (no table of the game's goes that far): the players'
// boxes (0, 1), then the sparkles' glyphs (2..9).
const uint32_t kStringId = 0x3F00C0DE;
const int      kTextLen = 200;
const int      kNumStrings = 10;
wchar_t        g_strings[kNumStrings][kTextLen];

// The in-game interface's tick (thiscall(this, time), ret 4); time +0xC = dt.
typedef void(__fastcall* InterfaceTickFn)(char* self, void* edx, void* time);
const uintptr_t kInGameVtbl = 0x6285F0;
const size_t    kInGameTickSlot = 18;
const uintptr_t kInGameTick = 0x5180E0;
InterfaceTickFn g_origInterfaceTick = nullptr;
const unsigned  kPromptNone = 0xE;

struct Hint { int key; int player; wchar_t text[kTextLen]; DWORD until; DWORD since; };
Hint g_hints[24];
int  g_nHints = 0;
bool g_loggedKinds = false;

uint32_t g_promptIds[15];          // the prompt kinds' strings (interface +0x1A4)

// Who reads a text, by the code that asks for it: text objects take their
// strings through FUN_0042E590 (thiscall(id), push esi / push edi, its call
// returns to 0x42E5A6), so its own caller's return address is 0x10 past ours.
const uintptr_t kTextSetReturn = 0x42E5A6;
bool In(uintptr_t a, uintptr_t from, uintptr_t to) { return a >= from && a < to; }

int PlayerOfHero(int hero) { return hero == CoopP1Hero() ? 0 : 1; }

// With player 2 in, the game's action icons in a string become the buttons
// each player presses: a prompt's those of the player whose hero it is for, a
// dialogue's (CKSekens, either player moves it on) both players', a menu's
// (only player 1 drives them) player 1's. At the merchant everything else is
// the shopper's: the shop's buttons ("Back", "Buy") are refreshed from shared
// 2D code (FUN_0042E680) with the pause menu's string ids, so only the time
// tells them apart: the game's state (pause menu or not).
const wchar_t* __fastcall LocStringHook(void* loc, void* edx, uint32_t id) {
	if (id >= kStringId && id < kStringId + kNumStrings) return g_strings[id - kStringId];
	const wchar_t* s = g_origLocString(loc, edx, id);
	if (!s) return s;
	// Single player ([single_player] sp_coop_icons): every text player 1's.
	if (!CoopP2Joined()) return g_config.spCoopIcons ? IconsRewrite(s, 0) : s;
	void** ra = (void**)_AddressOfReturnAddress();
	uintptr_t from = (uintptr_t)ra[0];
	if (from == kTextSetReturn) from = (uintptr_t)ra[4];
	// The pause menu and the pages under it (game manager +0xCC bits 0-1 = 2)
	// are player 1's, a shopper or not.
	CKYellowPages* yp = YellowPages();
	char* gm = yp ? (char*)yp->gameManager : nullptr;
	bool paused = gm && (*(uint8_t*)(gm + 0xCC) & 3) == 2;
	int shopper = paused ? -1 : CoopShopper();
	int who = shopper >= 0 ? PlayerOfHero(shopper) : 0;
	bool prompt = false;
	for (int k = 0; k < 15 && !prompt; k++)
		if (g_promptIds[k] == id && id) { who = CoopPromptPlayer(); prompt = true; }
	if (!prompt && shopper < 0 && In(from, 0x46E940, 0x46ECF0))   // CKSekens vf17-vf21
		who = -1;
	static int logs = 0;
	if (getenv("XXL_ICONLOG") && logs < 60 && IconsRewrite(s, who) != s) {
		static uint64_t seen[60];
		uint64_t key = (uint64_t)id << 32 | from ^ (uint32_t)(who + 1) << 30 ^ (uint32_t)paused << 29;
		bool known = false;
		for (int i = 0; i < logs; i++) known |= seen[i] == key;
		if (!known) {
			seen[logs++] = key;
			Log("icons: string %X asked for from %08X (shopper %d, paused %d) -> %s", id, (unsigned)from, shopper, paused,
			    who < 0 ? "both" : who ? "player 2" : "player 1");
		}
	}
	return IconsRewrite(s, who);
}

// The localisation manager: the text object's layout (+0x20) keeps it at +4.
void* LocOf(char* text) { return text ? *(void**)(text + 0x24) : nullptr; }

void LogKinds(char* self) {
	if (g_loggedKinds) return;
	g_loggedKinds = true;
	void* loc = LocOf(*(char**)(self + 0x15C));
	if (!loc) return;
	for (int k = 0; k < 15; k++) {
		uint32_t id = *(uint32_t*)(self + 0x1A4 + k * 4);
		const wchar_t* s = g_origLocString(loc, nullptr, id);
		char a[64] = {};
		for (int i = 0; s && s[i] && i < 63; i++) a[i] = s[i] < 128 ? (char)s[i] : '?';
		Log("hints: prompt kind %d = string %08X \"%s\"", k, id, a);
	}
}

// The newest live hint for player `p`'s box (both players' hints included --
// not in single player's left box: only its own).
const Hint* NewestFor(int p) {
	DWORD now = GetTickCount();
	const Hint* best = nullptr;
	bool own = p == HINT_LEFT_BOX && !CoopP2Joined();
	for (int i = 0; i < g_nHints; i++) {
		const Hint& h = g_hints[i];
		if ((int)(h.until - now) <= 0 || (h.player != p && (h.player != HINT_BOTH || own))) continue;
		if (!best || (int)(h.since - best->since) > 0 || (h.since == best->since && h.key > best->key)) best = &h;
	}
	return best;
}

// ---- the players' boxes ----------------------------------------------------
//
// A box is a copy of the game's prompt text object (CText2d, 0x6DC bytes: +5
// flags, bit 2 shown; +6 / +7 alignment; +0x14 / +0x18 its place, x a share of
// the screen's width from the left, y of its height from the bottom; its layout
// at +0x20: +4 the localisation manager, +0x14 string id, +0x18 the string,
// +0x2D4 word count, +0x2D8 word pointers into it, +0x3C8 + 8i each word's x,
// y in text units -- the screen's width is the aspect ratio (yellow pages +0x34
// -> +4) wide, its height 1 high --, +0x660 font, +0x664 size, +0x6A8 line
// spacing). Laid out by the game's FUN_0042E590 (thiscall(id), ret 4) with its
// own string id on a wide box (every word on one line), then its words are put
// on the text's own lines ('\n'), left- or right-aligned. Drawn by its vf23
// (thiscall(arg), ret 4: from its place, at alignment +6 = 0 rightwards), on the
// game's plate (+0x140, a CBillboard2d drawn from its place by +0x34 wide, +0x38
// high; +6 = 2 leftwards, 0 rightwards), set for the box and put back.
const size_t kText2dSize = 0x6DC;
const int    kMaxWords = 60;
const DWORD  kHoldMs = 120;           // a change of what a box shows counts once it lasts
const DWORD  kPromptHoldMs = 300;     // ... away from a prompt
const float  kInTime = 0.18f, kOutTime = 0.12f;
const float  kEdge = 0.05f, kMargin = 0.02f;

struct Burst { int button; DWORD at; };   // the n-th button of the last line, when
struct Slot {
	int      kind = 0;               // 0 nothing, 1 the player's prompt, 2 a hint
	uint32_t ident = 0;              // the prompt's string id / the hint's key
	wchar_t  text[kTextLen] = {};
	int      pendKind = 0;
	uint32_t pendIdent = 0;
	DWORD    pendSince = 0;
	bool     leaving = false;        // sliding out, then the new content in
	float    in = 0.0f;              // 0 out of sight .. 1 in place
	bool     rightAligned = false;
	bool     fromBelow = false;      // slides in and out at the bottom (else at its side)
	alignas(16) char obj[kText2dSize];
	bool     laid = false;
	int      nLines = 1, lastLineWord = 0, nWords = 0;
	float    longest = 0.0f, step = 0.0f, size = 0.0f;
	float    wx[kMaxWords] = {}, wy[kMaxWords] = {}, ww[kMaxWords] = {};
	bool     button[kMaxWords] = {};  // a button of the last line: any word but ">"
	Burst    bursts[8] = {};
	int      nBursts = 0;
};
Slot   g_slots[2];
char*  g_itf = nullptr;
alignas(16) char g_glyphObj[8][kText2dSize];   // spark0..3, glow0..3, each one glyph
bool   g_glyphLaid[8] = {};
float  g_glyphW[8] = {}, g_glyphY[8] = {}, g_glyphSize[8] = {};

float Aspect() {
	CKYellowPages* yp = YellowPages();
	char* screen = yp ? *(char**)((char*)yp + 0x34) : nullptr;
	float a = screen ? *(float*)(screen + 4) : 0.0f;
	return a > 0.5f && a < 8.0f ? a : 4.0f / 3.0f;
}

// Text glyphs are laid out and drawn this much wider than the font: the aspect
// times the game's constant at 0x623C64 -- 0.75, or 1 / aspect under
// xxl_widescreen2d.asi (true letter shapes), so it is read, not assumed.
float GlyphWidthScale() { return Aspect() * *(const float*)0x623C64; }

// xxl_widescreen2d.asi (seen by that constant no longer being 0.75) also draws
// stretched plates (4/3) / aspect as wide; the plate is sized for the text's
// true width, so it is given that much more.
float PlateDrawScale() {
	if (*(const float*)0x623C64 == 0.75f) return 1.0f;
	return (4.0f / 3.0f) / Aspect();
}

typedef float(__cdecl* WidthFn)(void* font, const wchar_t* word, uint32_t size);
typedef char(__fastcall* SetTextFn)(char* layout, void* edx, uint32_t id);
typedef char(__fastcall* DrawFn)(char* self, void* edx, void* arg);

float WordWidth(char* t20, unsigned i) {
	void* font = *(void**)(t20 + 0x660);
	const wchar_t* w = *(const wchar_t**)(t20 + 0x2D8 + 4 * i);
	if (!font || !w) return 0.0f;
	return GlyphWidthScale() * ((WidthFn)0x5FDAB0)(font, w, *(uint32_t*)(t20 + 0x664));
}

// `text` into string `si` ('\n' -> ' ', spaces single), laid out in `obj` (a
// copy of `game`); the first word of each line in `lineWord`.
bool LayOut(char* obj, const char* game, int si, const wchar_t* text, unsigned lineWord[8], int* nLines) {
	wchar_t* out = g_strings[si];
	int k = 0, words = 0;   // words finished so far: the next word's index
	*nLines = 1;
	lineWord[0] = 0;
	bool inWord = false;
	for (const wchar_t* c = text; *c && k < kTextLen - 2; c++) {
		if (*c == L'\n' || *c == L' ') {
			if (inWord) { out[k++] = L' '; words++; inWord = false; }
			if (*c == L'\n' && *nLines < 8 && lineWord[*nLines - 1] != (unsigned)words) lineWord[(*nLines)++] = words;
			continue;
		}
		if (words >= kMaxWords - 1) break;
		out[k++] = *c;
		inWord = true;
	}
	while (k > 0 && out[k - 1] == L' ') k--;
	out[k] = 0;
	while (*nLines > 1 && lineWord[*nLines - 1] >= (unsigned)words + (inWord ? 1 : 0)) (*nLines)--;   // no words after a last '\n'
	memcpy(obj, game, kText2dSize);
	*(float*)(obj + 0x28) = 8.0f;   // every word on one line
	((SetTextFn)0x42E590)(obj + 0x20, nullptr, kStringId + si);
	return *(uint32_t*)(obj + 0x20 + 0x2D4) > 0;
}

void LayOutSlot(Slot& s, int si, const char* game) {
	unsigned lineWord[8];
	s.laid = LayOut(s.obj, game, si, s.text, lineWord, &s.nLines);
	if (!s.laid) return;
	char* t20 = s.obj + 0x20;
	unsigned n = *(uint32_t*)(t20 + 0x2D4);
	if (n > kMaxWords) n = kMaxWords;
	s.nWords = (int)n;
	float* pos = (float*)(t20 + 0x3C8);
	float x0[kMaxWords], x1[kMaxWords];
	for (unsigned i = 0; i < n; i++) {
		x0[i] = pos[2 * i];
		s.ww[i] = WordWidth(t20, i);
		x1[i] = x0[i] + s.ww[i];
		const wchar_t* w = *(const wchar_t**)(t20 + 0x2D8 + 4 * i);
		wchar_t next = IconGlyph("next");
		s.button[i] = w && w[0] && !((w[0] == L'>' || (next && w[0] == next)) && !w[1]);
	}
	float start[8], width[8], longest = 0.0f;
	for (int l = 0; l < s.nLines; l++) {
		unsigned f = lineWord[l], e = l + 1 < s.nLines ? lineWord[l + 1] : n;
		if (f >= n || e <= f) { start[l] = 0; width[l] = 0; continue; }
		start[l] = x0[f];
		width[l] = x1[e - 1] - x0[f];
		if (width[l] > longest) longest = width[l];
	}
	s.size = *(float*)(t20 + 0x664);
	s.step = s.size * (1.0f + *(float*)(t20 + 0x6A8));
	float y0 = pos[1];
	for (int l = 0; l < s.nLines; l++) {
		unsigned f = lineWord[l], e = l + 1 < s.nLines ? lineWord[l + 1] : n;
		for (unsigned i = f; i < e && i < n; i++) {
			pos[2 * i] = (x0[i] - start[l]) + (s.rightAligned ? longest - width[l] : 0.0f);
			pos[2 * i + 1] = y0 - s.step * l;
			s.wx[i] = pos[2 * i];
			s.wy[i] = pos[2 * i + 1];
		}
	}
	s.lastLineWord = (int)lineWord[s.nLines - 1];
	s.longest = longest;
}

// The sparkles' glyphs, each laid out once per interface.
void LayOutGlyphs(const char* game) {
	for (int i = 0; i < 8; i++) {
		if (g_glyphLaid[i]) continue;
		wchar_t c = i < 4 ? IconSparkGlyph(i) : IconGlowGlyph(i - 4);
		if (!c) continue;
		wchar_t t[2] = { c, 0 };
		unsigned lw[8];
		int nl;
		if (!LayOut(g_glyphObj[i], game, 2 + i, t, lw, &nl)) continue;
		char* t20 = g_glyphObj[i] + 0x20;
		g_glyphSize[i] = *(float*)(t20 + 0x664);
		g_glyphW[i] = WordWidth(t20, 0);
		g_glyphY[i] = ((float*)(t20 + 0x3C8))[1];
		((float*)(t20 + 0x3C8))[0] = 0.0f;
		g_glyphLaid[i] = true;
	}
}

// Where a box is: its side (true: the left edge) and how far up (the stacked
// split's top half). Single player: player 1's at the game's place, the left
// box (HINT_LEFT_BOX) where player 1's is side by side.
struct Place { bool left; float up; };
// Single player's sidekick view (splitscreen.cpp, bottom left): the left box
// stands right of it -- moved there at the game's speed for the potion's flask
// when the view comes with the box up, and back when it goes. A box coming in
// while the view is up slides in from below the screen (and out the same way);
// else from the left edge.
float g_leftAside = 0.0f;   // how far the left box is moved right now (a share of the screen's width)
Place PlaceOf(int p) {
	if (!CoopP2Joined()) return { p == HINT_LEFT_BOX, 0.0f };
	if (SplitActive() && g_config.splitMode == SPLIT_HORIZONTAL) return { false, p == 0 ? 0.5f : 0.0f };
	return { p == 0, 0.0f };
}

// What a box should show: kind (0 nothing, 1 the prompt, 2 a hint), ident, text.
void Choose(int p, char* itf, int* kind, uint32_t* ident, const wchar_t** text) {
	static wchar_t prompt[2][kTextLen];
	*kind = 0; *ident = 0; *text = L"";
	CKYellowPages* yp = YellowPages();
	char* gm = yp ? (char*)yp->gameManager : nullptr;
	unsigned gameKind = gm ? (*(uint32_t*)(gm + 0xCC) >> 4) & 0xF : kPromptNone;
	if (CoopP2Joined()) {
		unsigned k = CoopPromptKindFor(p);
		void* loc = LocOf(*(char**)(itf + 0x15C));
		if (k < 15 && k != kPromptNone && loc) {
			uint32_t id = *(uint32_t*)(itf + 0x1A4 + k * 4);
			const wchar_t* s = g_origLocString(loc, nullptr, id);
			if (s) {
				wcsncpy(prompt[p], IconsRewrite(s, p), kTextLen - 1);
				prompt[p][kTextLen - 1] = 0;
				*kind = 1; *ident = id; *text = prompt[p];
				return;
			}
		}
	} else if (p == 0 && gameKind != kPromptNone) {
		return;   // player 1 alone: the game's own prompt comes first (the left box has no prompt)
	}
	if (const Hint* h = NewestFor(p)) { *kind = 2; *ident = (uint32_t)h->key; *text = h->text; }
}

void UpdateSlot(int p, char* itf, float dt) {
	Slot& s = g_slots[p];
	char* game = *(char**)(itf + 0x15C);
	if (!game || *(uintptr_t*)game != 0x623B6C) return;
	int kind;
	uint32_t ident;
	const wchar_t* text;
	Choose(p, itf, &kind, &ident, &text);
	Place pl = PlaceOf(p);
	bool right = false;   // every box's lines left-aligned (the left box's right-aligned lines read oddly: user)
	DWORD now = GetTickCount();
	if (kind == s.kind && ident == s.ident) {
		s.pendKind = -1;
		s.leaving = false;
		if (kind && (wcscmp(text, s.text) != 0 || right != s.rightAligned || !s.laid)) {
			wcsncpy(s.text, text, kTextLen - 1);
			s.rightAligned = right;
			LayOutSlot(s, p, game);
		}
	} else {
		if (s.kind && s.in > 0.0f) {
			if (s.pendKind != kind || s.pendIdent != ident) { s.pendKind = kind; s.pendIdent = ident; s.pendSince = now; }
			// (A prompt longer: the game's own goes off for a few frames at its range's edge.)
			if (now - s.pendSince >= (s.kind == 1 ? kPromptHoldMs : kHoldMs)) s.leaving = true;
		}
		if (!s.kind || s.in <= 0.0f) {
			s.kind = kind;
			s.ident = ident;
			s.leaving = false;
			s.pendKind = -1;
			s.nBursts = 0;
			wcsncpy(s.text, text, kTextLen - 1);
			s.rightAligned = right;
			s.in = 0.0f;
			float vl, vr, vb, vt;
			s.fromBelow = p == HINT_LEFT_BOX && !CoopP2Joined() && SidekickViewRect(&vl, &vr, &vb, &vt);
			if (kind) LayOutSlot(s, p, game);
			char a[kTextLen] = {};
			for (int i = 0; kind && s.text[i] && i < kTextLen - 1; i++)
				a[i] = s.text[i] == L'\n' ? '|' : s.text[i] < 128 ? (char)s.text[i] : s.text[i] >= 0xE000 ? '#' : '?';
			static int logs = 0;
			if (logs < 400) {
				logs++;
				Log("hints: player %d's box (%s): %s%s%s", p + 1, pl.left ? "left" : pl.up > 0.0f ? "top right" : "right",
				    kind == 1 ? "prompt \"" : kind == 2 ? "hint \"" : "empty", a, kind ? "\"" : "");
			}
		}
	}
	if (s.kind && !s.leaving) s.in = s.in + dt / kInTime > 1.0f ? 1.0f : s.in + dt / kInTime;
	else s.in = s.in - dt / kOutTime < 0.0f ? 0.0f : s.in - dt / kOutTime;
	if (p == HINT_LEFT_BOX && !CoopP2Joined()) {
		float l, r, b, t, want = 0.0f;
		if (SidekickViewRect(&l, &r, &b, &t)) want = r + 0.01f - (kEdge - kMargin);
		float speed = *(float*)(itf + 0x278);   // (the flask's, a share of the screen a second)
		if (!(speed > 0.1f && speed < 10.0f)) speed = 1.0f;
		bool entering = s.kind && !s.leaving && s.in < 1.0f;
		float was = g_leftAside;
		if (!s.kind || s.in <= 0.0f || entering) g_leftAside = want;
		else if (g_leftAside < want) g_leftAside = g_leftAside + speed * dt > want ? want : g_leftAside + speed * dt;
		else if (g_leftAside > want) g_leftAside = g_leftAside - speed * dt < want ? want : g_leftAside - speed * dt;
		static int s_told = 0;
		if ((was == 0.0f) != (g_leftAside == 0.0f) && s_told++ < 60)
			Log("hints: the left box %s the sidekick view (%.3f)%s", g_leftAside > 0.0f ? "right of" : "back from", want,
			    s.fromBelow ? ", in from below" : "");
	}
}

// The game's interface is the in-game one, its HUD up (not a menu's, a pause's
// or a cutscene's).
bool HudUp(char* itf) {
	CKYellowPages* yp = YellowPages();
	char* gm = yp ? (char*)yp->gameManager : nullptr;
	if (!gm || !itf || *(char**)(gm + 0xF8) != itf) return false;
	uint32_t f = *(uint32_t*)(gm + 0x5C);
	return !(f & 0x8000000) && !(f & 0x100000) && (*(uint8_t*)(gm + 0xCC) & 3) != 2;
}

// xxl_widescreen2d.asi draws a text whose place is on a stretched plate (in its
// span as drawn this frame or the last, its width taken unsqueezed) nearer the
// plate's anchor; both boxes' plates, so taken, can reach past the screen's
// middle into each other. The boxes' texts and sparkles are drawn from a place
// above the screen -- on no plate -- with their words' y lowered by as much
// (a word is drawn at its text's place y - 1 + its own y): where they are meant.
const float kAboveScreen = 1.5f;

// A box's place as drawn (its text's x, how far up), its slide in or out
// included -- sideways from its edge, or up from below the screen (fromBelow:
// as far down as its top). `plate`: the game's prompt plate, as the game left it.
void BoxPlace(int p, const Slot& s, const float* plate, float* x, float* up) {
	Place pl = PlaceOf(p);
	float w = s.longest / Aspect();
	float e = 1.0f - (1.0f - s.in) * (1.0f - s.in);
	float aside = p == HINT_LEFT_BOX && !CoopP2Joined() ? g_leftAside : 0.0f;
	*up = pl.up + (s.nLines > 2 ? s.step * (s.nLines - 2) : 0.0f);
	if (s.fromBelow && plate) {
		float y = plate[6] - 0.012f * (s.nLines - 1) + *up, h = plate[14] * s.nLines + 0.015f;
		uint8_t ay = ((const uint8_t*)plate)[7];
		float top = (ay == 0 ? y - h : ay == 2 ? y : y - h * 0.5f) + h;
		*up -= (1.0f - e) * (top + 0.01f);
		*x = (pl.left ? kEdge : 1.0f - kEdge - w) + aside;
		return;
	}
	float off = (1.0f - e) * (w + 2.0f * kMargin + kEdge + aside);
	*x = pl.left ? kEdge + aside - off : 1.0f - kEdge - w + off;
}

void DrawGlyph(int gi, float cx, float cy, float size, void* arg) {
	if (gi < 0 || gi >= 8 || !g_glyphLaid[gi]) return;
	char* o = g_glyphObj[gi];
	float a = Aspect();
	float s0 = g_glyphSize[gi] > 0.0f ? g_glyphSize[gi] : size;
	*(float*)(o + 0x20 + 0x664) = size;
	float w = g_glyphW[gi] * size / s0 / a;
	float* f = (float*)o;
	f[5] = cx - w * 0.5f;
	f[6] = kAboveScreen;
	((float*)(o + 0x20 + 0x3C8))[1] = cy - size * 0.5f + 1.0f - kAboveScreen;   // its bottom at cy - size / 2
	*(uint8_t*)(o + 5) |= 2;
	*(uint8_t*)(o + 6) = 0;
	((DrawFn)(*(void***)o)[23])(o, nullptr, arg);
}

const DWORD kBurstMs = 500;

void DrawSlot(int p, char* itf, void* arg) {
	Slot& s = g_slots[p];
	if (!s.kind || !s.laid || s.in <= 0.0f) return;
	char* game = *(char**)(itf + 0x15C);
	float* plate = *(float**)(itf + 0x140);
	if (!game) return;
	Place pl = PlaceOf(p);
	float a = Aspect();
	float w = s.longest / a;
	// Lines go down from the prompt's line; more than two: the box goes up.
	float x, up;
	BoxPlace(p, s, plate && *(uintptr_t*)plate == 0x6235C0 ? plate : nullptr, &x, &up);
	float y = ((float*)game)[6] + up;
	if (plate && *(uintptr_t*)plate == 0x6235C0) {
		uint8_t* pb = (uint8_t*)plate;
		uint8_t f5 = pb[5], f6 = pb[6];
		float sv[4] = { plate[5], plate[6], plate[13], plate[14] };
		pb[5] |= 2;
		pb[6] = pl.left ? 0 : 2;
		plate[5] = pl.left ? x - kMargin : x + w + kMargin;
		plate[13] = (w + 2.0f * kMargin) / PlateDrawScale();
		plate[14] = sv[3] * s.nLines + 0.015f;
		plate[6] = sv[1] - 0.012f * (s.nLines - 1) + up;
		// Not one of xxl_widescreen2d.asi's plates (a HUD model over it -- the
		// potion's flask rising over the box -- was pulled towards its anchor, off
		// the screen as the box slid out); an older asi without the switch: as before.
		typedef void(__cdecl* RecordFn)(int on);
		static RecordFn record = nullptr;
		static bool looked = false;
		if (!looked) {
			HMODULE ws = GetModuleHandleA("xxl_widescreen2d.asi");
			record = ws ? (RecordFn)GetProcAddress(ws, "Widescreen2dRecordPlates") : nullptr;
			looked = true;   // (the ASI loader loads every asi before the game's first frame)
			Log("hints: xxl_widescreen2d.asi %s", !ws ? "not loaded" : record ? "takes the boxes' plates out of its plates" : "has no plate switch (older build)");
		}
		if (record) record(0);
		((DrawFn)(*(void***)plate)[23])((char*)plate, nullptr, arg);
		if (record) record(1);
		pb[5] = f5;
		pb[6] = f6;
		plate[5] = sv[0]; plate[6] = sv[1]; plate[13] = sv[2]; plate[14] = sv[3];
	}
	float* f = (float*)s.obj;
	f[5] = x;
	f[6] = kAboveScreen;
	float* pos = (float*)(s.obj + 0x20 + 0x3C8);
	for (int i = 0; i < s.nWords; i++) pos[2 * i + 1] = s.wy[i] + y - kAboveScreen;
	*(uint8_t*)(s.obj + 5) |= 2;
	*(uint8_t*)(s.obj + 6) = 0;
	((DrawFn)(*(void***)s.obj)[23])(s.obj, nullptr, arg);
	// Sparkles: a glow shrinking and six sparks flying out, over the button.
	DWORD now = GetTickCount();
	for (int b = 0; b < s.nBursts;) {
		DWORD age = now - s.bursts[b].at;
		int wi = -1;
		for (int i = s.lastLineWord, k = 0; i < s.nWords && wi < 0; i++)
			if (s.button[i] && k++ == s.bursts[b].button) wi = i;
		if (age >= kBurstMs || wi < 0) { s.bursts[b] = s.bursts[--s.nBursts]; continue; }
		float t = age / (float)kBurstMs;
		float cx = x + (s.wx[wi] + s.ww[wi] * 0.5f) / a;
		float cy = y - 1.0f + s.wy[wi] + s.size * 0.5f;
		int fade = (int)(t * 4.0f);
		DrawGlyph(4 + fade, cx, cy, s.size * (1.9f - 0.9f * t), arg);
		for (int k = 0; k < 6; k++) {
			float ang = 6.2831853f * k / 6.0f + 0.26f;
			float d = s.size * (0.35f + 1.1f * t);
			DrawGlyph(fade, cx + cosf(ang) * d / a, cy + sinf(ang) * d, s.size * (0.75f - 0.35f * t), arg);
		}
		b++;
	}
}

// The HUD's 2D layer (FUN_0042A470, thiscall(the 2D manager, arg), ret 4,
// called from FUN_00414070 at 0x4140F4): with player 2 in, the game's prompt
// box is left out (its text and plate shown flags cleared for the draw); the
// players' boxes after it.
typedef void(__fastcall* Draw2dFn)(char* m2d, void* edx, void* arg);
const uintptr_t kDraw2dCall = 0x4140F4;
const uint8_t   kDraw2dCallBytes[5] = { 0xE8, 0x77, 0x63, 0x01, 0x00 };
const uintptr_t kDraw2d = 0x42A470;

// The players' boxes are drawn with player 2 in -- not while a hero shops: the
// HUD's pass is then drawn in the shopper's half (splitscreen.cpp), the game's
// prompt as before.
bool OwnBoxes() { return CoopP2Joined() && CoopShopper() < 0; }

// The game's prompt box, left out of a 2D pass with player 2 in (its text's
// and plate's shown flags, +5 bit 1, cleared for the pass).
struct HideGamePrompt {
	uint8_t* tf = nullptr;
	uint8_t* pf = nullptr;
	uint8_t t5 = 0, p5 = 0;
	explicit HideGamePrompt(char* itf) {
		if (!itf || !OwnBoxes()) return;
		tf = *(uint8_t**)(itf + 0x15C);
		pf = *(uint8_t**)(itf + 0x140);
		if (tf) { t5 = tf[5]; tf[5] &= ~2; }
		if (pf) { p5 = pf[5]; pf[5] &= ~2; }
	}
	~HideGamePrompt() {
		if (tf) tf[5] = t5;
		if (pf) pf[5] = p5;
	}
};

// FUN_0042A420 (thiscall(the 2D manager, arg), ret 4, called at 0x4140B3) draws
// the menu manager's element lists (its vf12) -- the HUD's prompt among them.
typedef char(__fastcall* Draw2dListsFn)(char* m2d, void* edx, void* arg);
const uintptr_t kDraw2dListsCall = 0x4140B3;
const uint8_t   kDraw2dListsCallBytes[5] = { 0xE8, 0x68, 0x63, 0x01, 0x00 };
const uintptr_t kDraw2dLists = 0x42A420;

char __fastcall Draw2dListsHook(char* m2d, void* edx, void* arg) {
	char* itf = g_itf;
	bool up = itf && itf == (char*)GetLvlObject(2, 132, 0) && HudUp(itf);
	HideGamePrompt hide(up ? itf : nullptr);
	return ((Draw2dListsFn)kDraw2dLists)(m2d, edx, arg);
}

void __fastcall Draw2dHook(char* m2d, void* edx, void* arg) {
	char* itf = g_itf;
	bool up = itf && itf == (char*)GetLvlObject(2, 132, 0) && HudUp(itf);
	{
		HideGamePrompt hide(up ? itf : nullptr);
		((Draw2dFn)kDraw2d)(m2d, edx, arg);
	}
	if (!up || (CoopP2Joined() && !OwnBoxes())) return;
	char* game = *(char**)(itf + 0x15C);
	if (game && *(uintptr_t*)game == 0x623B6C) LayOutGlyphs(game);
	for (int p = 0; p < 2; p++) DrawSlot(p, itf, arg);
}

void __fastcall InterfaceTickHook(char* self, void* edx, void* time) {
	CKYellowPages* yp = YellowPages();
	char* gm = yp ? (char*)yp->gameManager : nullptr;
	if (gm && getenv("XXL_HINTLOG")) LogKinds(self);
	memcpy(g_promptIds, self + 0x1A4, sizeof(g_promptIds));
	if (self != g_itf) {   // another level's interface: the boxes laid out afresh
		g_itf = self;
		for (Slot& s : g_slots) { s.laid = false; s.kind = 0; s.in = 0.0f; s.nBursts = 0; }
		for (bool& b : g_glyphLaid) b = false;
	}
	{
		// Individual heroes: the HUD (top left, player 1's half) is player 1's
		// hero's health, shields and combo bar (at the merchant: the shopper's own
		// shields; single player's second chance: the leading hero's).
		Hud2BeforeInterfaceTick(self);
		int prev = IndivEnter(Hud2HudHero());
		g_origInterfaceTick(self, edx, time);
		IndivLeave(prev);
		Hud2AfterInterfaceTick(self, time);   // player 2's own health and combo bar
	}
	float dt = time ? *(float*)((char*)time + 0xC) : 0.016f;
	if (!(dt >= 0.0f && dt < 0.5f)) dt = 0.016f;
	for (int p = 0; p < 2; p++) UpdateSlot(p, self, dt);
}

} // namespace

const wchar_t* HintButtonName(int player, HintButton b, bool unpressed) {
	static const wchar_t* const kPad[HB_COUNT] = { L"A", L"X", L"LB", L"Start", L"Back", L"B", L"Y", L"RB" };
	static wchar_t buf[HB_COUNT][2][2][32];
	if (b < 0 || b >= HB_COUNT) return L"?";
	wchar_t* out = buf[b][player == 1][unpressed];
	wchar_t g = 0;
	int vk = 0;
	if (player == 1) {
		static const int kKey[HB_COUNT] = { CoopConfig::KEY_JUMP, CoopConfig::KEY_ATTACK, CoopConfig::KEY_LB,
		                                    CoopConfig::KEY_START, CoopConfig::KEY_BACK, CoopConfig::KEY_GRAB,
		                                    CoopConfig::KEY_DASH, CoopConfig::KEY_RB };
		static const int kAction[HB_COUNT] = { 4, 5, 8, 15, -1, 6, 7, 10 };
		if (CoopP2Pad() != kKeyboardPad && !getenv("XXL_ICONP2KB")) {
			static const char* const kPadIcon[HB_COUNT] = { "pad_a", "pad_x", "pad_lb", "pad_start", "pad_back", "pad_b", "pad_y", "pad_rb" };
			g = kAction[b] >= 0 ? IconActionGlyph(1, kAction[b], unpressed) : IconGlyph(kPadIcon[b]);
		} else {
			g = IconKeyGlyph(vk = g_config.keys[kKey[b]], unpressed);
		}
	} else {
		// Player 1: the device the game's binding for that button reads.
		static const int kAction[HB_COUNT] = { 4, 5, 8, 15, 12, 6, 7, 10 };
		g = IconActionGlyph(0, kAction[b], unpressed);
		if (!g) vk = IconsP1Vk(kAction[b]);
	}
	if (g) { out[0] = g; out[1] = 0; return out; }
	if (!vk) return kPad[b];
	IconsKeyName(vk, out, 32);
	return out;
}

const wchar_t* HintNext() {
	static wchar_t buf[4];
	wchar_t g = IconGlyph("next");
	buf[0] = L' '; buf[1] = g ? g : L'-'; buf[2] = L' '; buf[3] = 0;
	return buf;
}

const wchar_t* HintKeyName(int vk) {
	static wchar_t buf[32];
	wchar_t g = IconKeyGlyph(vk);
	if (g) { buf[0] = g; buf[1] = 0; return buf; }
	IconsKeyName(vk, buf, 32);
	return buf;
}

const wchar_t* HintHeroName(int hero) {
	static wchar_t buf[4][24];
	static int n = 0;
	wchar_t* out = buf[n++ & 3];
	wchar_t g = IconGlyph(hero == HERO_ASTERIX ? "asterix" : "obelix");
	const wchar_t* name = hero == HERO_ASTERIX ? L"Asterix" : L"Obelix";
	if (g) swprintf(out, 24, L"%c%s", g, name);
	else swprintf(out, 24, L"%s", name);
	return out;
}

const wchar_t* HintHero(int hero, int player) {
	static wchar_t buf[4][32];
	static int n = 0;
	wchar_t* out = buf[n++ & 3];
	wchar_t g = IconGlyph(player ? "p2" : "p1");
	if (g) swprintf(out, 32, L"%s %c", HintHeroName(hero), g);
	else swprintf(out, 32, L"%s (P%d)", HintHeroName(hero), player + 1);
	return out;
}

const wchar_t* HintPlayer(int player) {
	static wchar_t buf[2][16];
	wchar_t g = IconGlyph(player ? "p2" : "p1");
	if (g) swprintf(buf[player != 0], 16, L"%c", g);
	else swprintf(buf[player != 0], 16, L"Player %d:", player + 1);
	return buf[player != 0];
}

void HintShow(int key, int player, const wchar_t* text, DWORD ms) {
	DWORD now = GetTickCount();
	Hint* h = nullptr;
	for (int i = 0; i < g_nHints && !h; i++) if (g_hints[i].key == key && g_hints[i].player == player) h = &g_hints[i];
	if (!h) {
		if (g_nHints == 24) {   // the oldest goes
			int o = 0;
			for (int i = 1; i < 24; i++) if ((int)(g_hints[i].since - g_hints[o].since) < 0) o = i;
			h = &g_hints[o];
		} else {
			h = &g_hints[g_nHints++];
		}
		h->since = now;
	} else if ((int)(h->until - now) <= 0) {
		h->since = now;
	}
	h->key = key;
	h->player = player;
	wcsncpy(h->text, text, kTextLen - 1);
	h->text[kTextLen - 1] = 0;
	h->until = now + ms;
}

void HintHide(int key) {
	for (int i = 0; i < g_nHints;)
		if (g_hints[i].key == key) g_hints[i] = g_hints[--g_nHints];
		else i++;
}

void HintHide(int key, int player) {
	for (int i = 0; i < g_nHints;)
		if (g_hints[i].key == key && g_hints[i].player == player) g_hints[i] = g_hints[--g_nHints];
		else i++;
}

bool HintShown(int key) {
	DWORD now = GetTickCount();
	for (int i = 0; i < g_nHints; i++)
		if (g_hints[i].key == key && (int)(g_hints[i].until - now) > 0) return true;
	return false;
}

bool HintShown(int key, int player) {
	DWORD now = GetTickCount();
	for (int i = 0; i < g_nHints; i++)
		if (g_hints[i].key == key && g_hints[i].player == player && (int)(g_hints[i].until - now) > 0) return true;
	return false;
}

bool HintOnScreen(int key, int player) {
	if (player < 0 || player > 1) return false;
	const Slot& s = g_slots[player];
	return s.kind == 2 && s.ident == (uint32_t)key && !s.leaving && s.in > 0.0f;
}

bool HintBoxRect(int player, float* left, float* right, float* bottom, float* top) {
	if (player < 0 || player > 1 || !g_itf || g_itf != (char*)GetLvlObject(2, 132, 0)) return false;
	const Slot& s = g_slots[player];
	if (!s.kind || !s.laid || s.in <= 0.0f || (CoopP2Joined() && !OwnBoxes()) || !HudUp(g_itf)) return false;
	float* plate = *(float**)(g_itf + 0x140);
	if (!plate || *(uintptr_t*)plate != 0x6235C0) return false;
	// As DrawSlot places it.
	float w = s.longest / Aspect();
	float x, up;
	BoxPlace(player, s, plate, &x, &up);
	float y = plate[6] - 0.012f * (s.nLines - 1) + up, h = plate[14] * s.nLines + 0.015f;
	uint8_t ay = ((uint8_t*)plate)[7];
	*left = x - kMargin;
	*right = x + w + kMargin;
	*bottom = ay == 0 ? y - h : ay == 2 ? y : y - h * 0.5f;
	*top = *bottom + h;
	return true;
}

void HintSpark(int key, int player, int n, bool sound) {
	if (player < 0 || player > 1) return;
	Slot& s = g_slots[player];
	if (s.kind != 2 || s.ident != (uint32_t)key || !s.laid || n < 0) return;
	if (s.nBursts < 8) s.bursts[s.nBursts++] = { n, GetTickCount() };
	if (sound && g_itf && g_itf == (char*)GetLvlObject(2, 132, 0))   // the combo bar's level sound (interface sound 7)
		((void(__fastcall*)(char*, void*, uint32_t, uint32_t))0x5153F0)(g_itf, nullptr, 7, 0);
}

void HintsFrame() {
	DWORD now = GetTickCount();
	for (int i = 0; i < g_nHints;)
		if ((int)(g_hints[i].until - now) <= 0) g_hints[i] = g_hints[--g_nHints];
		else i++;
	// A text keeps a copy of its string from its last refresh, and the pause
	// menu's are not refreshed when it opens (the game refreshes all texts when
	// a loc variable changes: CManager2d FUN_0042A220, thiscall(3, 0), ret 8 --
	// the merchant's opening did, with whoever's buttons were due then). When
	// whose buttons are due changes (player 2 in or out, the pause menu, a
	// shopper), all texts are refreshed the same way.
	CKYellowPages* yp = YellowPages();
	char* gm = yp ? (char*)yp->gameManager : nullptr;
	char* m2d = yp ? *(char**)((char*)yp + 0x34) : nullptr;
	if (!gm || !m2d || *(uintptr_t*)m2d != 0x623A28) return;
	bool paused = (*(uint8_t*)(gm + 0xCC) & 3) == 2;
	int ctx = !CoopP2Joined() ? 0 : 1 | (paused ? 2 : 0) | (paused ? 0 : (CoopShopper() + 1) << 2) |
	          (CoopP2Pad() == kKeyboardPad ? 0x10 : 0);
	static int lastCtx = 0;
	if (ctx == lastCtx) return;
	lastCtx = ctx;
	((void(__fastcall*)(char*, void*, int, int))0x42A220)(m2d, nullptr, 3, 0);
	LogV("hints: texts refreshed for whose buttons are due (%X)", ctx);
}

bool HintsInstallHooks() {
	bool ok = InstallDetour(kLocString, (void*)LocStringHook, (void**)&g_origLocString, kLocStringPrologue,
	                        sizeof(kLocStringPrologue), "localised strings (co-op hints)");
	ok &= PatchVtableSlot(kInGameVtbl, kInGameTickSlot, kInGameTick, (void*)InterfaceTickHook,
	                      (void**)&g_origInterfaceTick, "CKHkInterfaceInGame::tick (co-op hints)");
	ok &= PatchCallSite(kDraw2dCall, kDraw2dCallBytes, sizeof(kDraw2dCallBytes), (void*)Draw2dHook,
	                    "the HUD's 2D layer (the players' prompt boxes)");
	ok &= PatchCallSite(kDraw2dListsCall, kDraw2dListsCallBytes, sizeof(kDraw2dListsCallBytes), (void*)Draw2dListsHook,
	                    "the HUD's 2D lists (the game's prompt box left out in co-op)");
	return ok;
}
