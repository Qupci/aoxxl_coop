#include "combohints.h"
#include "config.h"
#include "coop.h"
#include "detour.h"
#include "hints.h"
#include "icons.h"
#include "individual.h"
#include "inputmgr.h"
#include "kal.h"
#include "life.h"
#include "log.h"
#include "shared.h"
#include "trio.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace {

enum { C_CLUB, C_HAMMER, C_TWISTER, C_FUSION, C_MOLE, C_RAGE, C_COUNT };
const wchar_t* const kName[C_COUNT] = { L"Club", L"Power Hammer", L"Twister", L"Twister Fusion", L"Mole", L"Rage" };
struct Seq { int n; HintButton b[4]; };
const Seq kSeq[C_COUNT] = {
	{ 4, { HB_A, HB_A, HB_X, HB_B } },   // Club
	{ 3, { HB_A, HB_B, HB_X } },         // Power Hammer
	{ 4, { HB_A, HB_X, HB_B, HB_Y } },   // Twister
	{ 1, { HB_X } },                     // Twister Fusion: X with both spinning
	{ 4, { HB_A, HB_A, HB_Y, HB_B } },   // Mole, then X X X...
	{ 1, { HB_RB } },                    // Rage
};
// How near a Roman has to be for the hint (units): Club and Power Hammer take
// one in hand, the Mole comes up under them, Idefix runs at them.
const float kNear[C_COUNT] = { 5.0f, 5.0f, 8.0f, 0.0f, 8.0f, 12.0f };
const DWORD kAfterMs = 8000;      // a combo's hint after its Roman is gone
const DWORD kCrowdMs = 12000;     // the crowd's Twister
const DWORD kCrowdMoreMs = 8000;  // ... two buttons pressed in order
const DWORD kDoneMs = 1200;       // all lit, the combo done
const DWORD kSlackMs = 2500;      // buttons pressed in order: lit this long after the last
const int   kCrowd = 100;         // Romans still to beat: a crowd

// The game's action values on a button's press frame (0xFF): A 4 (and 44), X 5
// (45), B 6, Y 7, LB 8, RB 10.
const int kEdge[HB_COUNT][2] = { { 4, 44 }, { 5, 45 }, { 8, -1 }, { -1, -1 }, { -1, -1 }, { 6, -1 }, { 7, -1 }, { 10, -1 } };

struct Player {
	int   queue[C_COUNT] = {};    // bought this level, still to be tried
	int   n = 0;
	bool  crowdTold = false;      // the crowd's Twister hint, this level
	int   shown = -1;             // the combo of the hint up (-1: none)
	bool  crowd = false;          // ... the crowd's Twister
	DWORD until = 0;
	bool  more = false;           // the crowd's Twister: lengthened once
	int   lit = 0;                // buttons pressed in order
	DWORD lastPress = 0;
	DWORD doneAt = 0;             // done: all lit until doneAt + kDoneMs
	bool  wasFull = false;
	int   fuseLit = 0;
	bool  air = false;            // a jump-begun combo under way: the hero has left the ground
};
Player   g_p[2];
uint32_t g_bits = 0;
bool     g_bitsKnown = false;
int      g_level = -2;
char*    g_itf = nullptr;
bool     g_fused = false;         // a Twister Fusion this level
bool     g_fuseUp = false;
DWORD    g_spent[2] = {};          // FUN_00493810: a combo done, by hero
int      g_spentKind[2] = { -1, -1 };
int      g_logs = 0;
const char* g_skipped[16];         // no-combos pools logged this level (squad + pool)
int      g_skipN = 0;

CKYellowPages* YP() { return YellowPages(); }
char* GM() { CKYellowPages* yp = YP(); return yp ? (char*)yp->gameManager : nullptr; }
uint32_t Learned() {
	char* gm = GM();
	char* sg = gm ? *(char**)(gm + 0x40) : nullptr;
	return sg ? *(uint32_t*)(sg + 0x10) : 0;
}
// Single player ([single_player] sp_combo_hints): player 1 alone, its hero the
// trio's leader; a combo is queued for whichever hero leads when it comes up.
bool Solo() { return !CoopP2Joined(); }
int Players() { return Solo() ? 1 : 2; }
int HeroOf(int p) {
	if (Solo()) { Trio t = GetTrio(); return p == 0 && t ? t.Leader() : -1; }
	return p == 0 ? CoopP1Hero() : CoopP2Hero();
}

bool Full(int hero) {
	int hp, sh, hits;
	float gauge;
	if (IndivStats(hero, &hp, &sh, &hits, &gauge)) return gauge >= 0.0f;
	return (Learned() & 0x4000) != 0;
}

bool CanDo(int hero, int c) { return c != C_FUSION && (c != C_MOLE || hero == HERO_ASTERIX); }
// Queued for a player: what its hero can do (single player: either hero may lead
// later, the hint waits for one that can).
bool Queues(int hero, int c) { return c != C_FUSION && (Solo() || CanDo(hero, c)); }

// Combos switched off by the level (the boss runs' trio 0x48EE / 0x48F1): game
// manager +0x14 (the trio) vf53 / vf54 (thiscall, no arguments), asked by the
// heroes' combo actions and Idefix's rage alike.
bool CombosOff(Trio) {
	char* gm = GM();
	char* tr = gm ? *(char**)(gm + 0x14) : nullptr;
	if (!tr) return true;
	typedef char(__fastcall* AskFn)(void* trio, void* edx);
	void** vt = *(void***)tr;
	return ((AskFn)vt[53])(tr, nullptr) != 0 || ((AskFn)vt[54])(tr, nullptr) != 0;
}

void Push(Player& pl, int hero, int c) {
	if (!Queues(hero, c)) return;
	for (int i = 0; i < pl.n; i++) if (pl.queue[i] == c) return;
	if (pl.n < C_COUNT) pl.queue[pl.n++] = c;
}

void Pop(Player& pl, int c) {
	for (int i = 0; i < pl.n; i++)
		if (pl.queue[i] == c) {
			for (int k = i; k + 1 < pl.n; k++) pl.queue[k] = pl.queue[k + 1];
			pl.n--;
			return;
		}
}

// The nearest Roman of an awake squad (4,26: +0x152 bit 2; member lists per pool
// record +0x144, 0x1C each, +0x14C of them; members chained from record +8 by
// +0x50; done +8 bit 0, knocked out +0x24 bit 1; place: node +0x10 -> frame +4,
// LTM position +0x80), squared, within 3 units up or down. Not the turtles or the
// mobile tower (pool record +4, the enemy type's component, by vtable): no combo
// works on them.
bool NoCombos(const char* rec) {
	const char* cpnt = *(char* const*)(rec + 4);
	if (!cpnt) return false;
	switch (*(const uintptr_t*)cpnt) {
	case 0x627D20:   // CKMobileTowerCpnt
	case 0x627D50:   // CKTriangularTurtleCpnt
	case 0x627DB8:   // CKSquareTurtleCpnt
	case 0x627DEC:   // CKDonutTurtleCpnt
	case 0x627E20:   // CKPyramidalTurtleCpnt
		return true;
	}
	return false;
}

float NearestRoman2(const Vector3& at) {
	float best = 1e30f;
	for (unsigned i = 0, n = GetNumLvlClassInstances(4, 26); i < n; i++) {
		char* sq = (char*)GetLvlObject(4, 26, i);
		if (!sq || !(*(uint8_t*)(sq + 0x152) & 2)) continue;
		char* recs = *(char**)(sq + 0x144);
		unsigned nr = *(unsigned*)(sq + 0x14C);
		for (unsigned r = 0; recs && r < nr && r < 8; r++) {
			const char* rec = recs + 0x1C * r;
			if (NoCombos(rec)) {
				bool told = false;
				for (int k = 0; k < g_skipN && !told; k++) told = g_skipped[k] == rec;
				if (!told && g_skipN < 16) {
					g_skipped[g_skipN++] = rec;
					Log("combohints: squad %p pool %u: no combos on its enemies (cpnt vtable %08X), never a Roman near", sq, r,
					    (unsigned)**(uintptr_t**)(rec + 4));
				}
				continue;
			}
			int guard = 0;
			for (char* m = *(char**)(recs + 0x1C * r + 8); m && guard++ < 256; m = *(char**)(m + 0x50)) {
				if ((*(uint8_t*)(m + 8) & 1) || (*(uint32_t*)(m + 0x24) & 2)) continue;
				char* node = *(char**)(m + 0x10);
				char* fr = node ? *(char**)(node + 4) : nullptr;
				if (!fr) continue;
				const float* p = (const float*)(fr + 0x80);
				float dx = p[0] - at.x, dy = p[1] - at.y, dz = p[2] - at.z;
				if (fabsf(dy) > 3.0f) continue;
				float d = dx * dx + dz * dz;
				if (d < best) best = d;
			}
		}
	}
	return best;
}

// Romans still to beat in the fight a gate holds (what its counter shows): the
// squads it has taken (+0x154 >= -1; -1 every pool, else that pool) of both
// squad classes (4,26 / 4,64), each pool (+0x144, 0x1C each, +0x14C of them; to
// defeat int16 +0x18, < 0 endless; sent uint16 +0x16) its Romans to send plus
// those alive (squad vf42, thiscall(pool), 0 while the squad sleeps). -1: none.
int GateRomansLeft() {
	int left = 0, gated = 0;
	const int kClasses[2] = { 26, 64 };
	for (int c : kClasses)
		for (unsigned i = 0, n = GetNumLvlClassInstances(4, c); i < n; i++) {
			char* sq = (char*)GetLvlObject(4, c, i);
			if (!sq) continue;
			int sel = *(int8_t*)(sq + 0x154);
			if (sel < -1) continue;
			gated++;
			char* recs = *(char**)(sq + 0x144);
			unsigned np = *(unsigned*)(sq + 0x14C);
			bool active = (*(uint8_t*)(sq + 0x152) & 2) != 0;
			for (unsigned p = 0; recs && p < np && p < 16; p++) {
				if (sel >= 0 && sel != (int)p) continue;
				const char* rec = recs + 0x1C * p;
				if (*(const int16_t*)(rec + 0x18) < 0) continue;
				int r = *(const int16_t*)(rec + 0x18) - *(const uint16_t*)(rec + 0x16);
				if (active) r += ((int(__fastcall*)(char*, void*, int))(*(void***)sq)[42])(sq, nullptr, (int)p);
				if (r > 0) left += r;
			}
		}
	return gated ? left : -1;
}

// The hint's text: the combo's icon and what to do, then its buttons, those
// pressed in order lit (the rest the unpressed icons).
void Compose(wchar_t* out, int n, int p, int c, bool crowd, int lit, bool fuse) {
	wchar_t icon = IconComboGlyph(c);
	wchar_t ic[3] = { icon, (wchar_t)(icon ? L' ' : 0), 0 };
	wchar_t keys[160] = {};
	const Seq& s = kSeq[c];
	for (int i = 0; i < s.n; i++) {
		if (i) wcscat(keys, HintNext());
		wcscat(keys, HintButtonName(p, s.b[i], i >= lit));
	}
	if (fuse) {
		swprintf(out, n, L"%sTwister Fusion:\n%s fuses the tornadoes", ic, keys);
	} else if (crowd) {
		swprintf(out, n, L"%sSo many Romans: Twister!\n%s", ic, keys);
	} else if (c == C_MOLE) {
		wchar_t x[32];
		wcscpy(x, HintButtonName(p, HB_X));
		swprintf(out, n, L"%sTry your new combo: %s\n%s then %s %s %s...", ic, kName[c], keys, x, x, x);
	} else if (c == C_RAGE) {
		swprintf(out, n, L"%sTry your new combo: %s\n%s sends Idefix in", ic, kName[c], keys);
	} else {
		swprintf(out, n, L"%sTry your new combo: %s\n%s", ic, kName[c], keys);
	}
}

// Buttons player `p` pressed this frame (bits of HintButton).
unsigned Pressed(int p) {
	const uint8_t* v = nullptr;
	int nv = 0;
	if (p == 0) {
		InputManager im = GetInputManager();
		v = im ? im.Values() : nullptr;
		nv = im ? im.NumActions() : 0;
	} else {
		v = CoopP2Values();
		nv = 48;
	}
	unsigned m = 0;
	for (int b = 0; v && b < HB_COUNT; b++)
		for (int k = 0; k < 2; k++)
			if (kEdge[b][k] >= 0 && kEdge[b][k] < nv && v[kEdge[b][k]] == 0xFF) m |= 1u << b;
	return m;
}

// A press against the sequence: the next button in order lights up (a sparkle;
// the second and third with the combo bar's level sound); another starts it
// again (the first button: lit as the first).
void Press(Player& pl, int p, const Seq& s, unsigned pressed, int key) {
	DWORD now = GetTickCount();
	if (pl.lit > 0 && pl.lit < s.n && now - pl.lastPress > kSlackMs) pl.lit = 0;
	if (pl.lit >= s.n && now - pl.lastPress > kDoneMs) pl.lit = 0;
	for (int b = 0; b < HB_COUNT && pressed; b++) {
		if (!(pressed & (1u << b))) continue;
		pressed &= ~(1u << b);
		if (pl.lit >= s.n) continue;
		pl.lastPress = now;
		if (s.b[pl.lit] == (HintButton)b) {
			pl.lit++;
			HintSpark(key, p, pl.lit - 1, pl.lit == 2 || pl.lit == 3);
		} else {
			pl.lit = s.b[0] == (HintButton)b ? 1 : 0;
			if (pl.lit) HintSpark(key, p, 0, false);
		}
	}
}

void ResetLevel(int level) {
	g_level = level;
	g_skipN = 0;
	for (Player& pl : g_p) pl = Player();
	g_fused = false;
	g_bitsKnown = false;
	HintHide(HINT_COMBO);
	HintHide(HINT_FUSE);
}

// FUN_00493810 (thiscall(game manager, kind), ret 4): a combo done spends the
// bar -- the hero whose Life is ticking (Idefix's: the hero whose RB called him).
typedef void(__fastcall* SpendFn)(char* gm, void* edx, int kind);
const uintptr_t kSpend = 0x493810;
const uint8_t   kSpendPrologue[6] = { 0x8B, 0x41, 0x40, 0x8B, 0x50, 0x10 };
SpendFn g_origSpend = nullptr;

void __fastcall SpendHook(char* gm, void* edx, int kind) {
	char* sg = gm ? *(char**)(gm + 0x40) : nullptr;
	bool full = sg && (*(uint32_t*)(sg + 0x10) & 0x4000);
	int hero = LifeTickingHero();
	if (hero < 0) { Trio t = GetTrio(); hero = t ? t.Leader() : -1; }
	if (full && hero >= 0 && hero < 2 && kind >= 0 && kind < C_COUNT) {
		g_spent[hero] = GetTickCount();
		g_spentKind[hero] = kind;
		if (g_logs++ < 60) Log("combohints: %s did the %S (combo %d)", HeroName(hero), kName[kind], kind);
	}
	g_origSpend(gm, edx, kind);
}

void PlayerFrame(int p, Trio t) {
	Player& pl = g_p[p];
	int hero = HeroOf(p);
	Hero h = t ? t.GetHero(hero) : Hero();
	DWORD now = GetTickCount();
	if (!h || hero < 0 || hero > 1) { HintHide(HINT_COMBO, p); pl.shown = -1; return; }
	bool full = Full(hero) && !IndivFallen(hero) && !CombosOff(t);
	bool filled = full && !pl.wasFull;
	pl.wasFull = full;
	uint32_t learned = Learned();
	// A combo done by this player's hero.
	if (g_spentKind[hero] >= 0) {
		int c = g_spentKind[hero];
		g_spentKind[hero] = -1;
		if (pl.shown == c && !pl.doneAt) {
			pl.doneAt = now;
			pl.lit = kSeq[c].n;
			Log("combohints: player %d tried the %S: its hint goes", p + 1, kName[c]);
		}
		Pop(pl, c);
		if (c == C_TWISTER) pl.crowdTold = true;
	}
	if (pl.doneAt) {
		if (now - pl.doneAt < kDoneMs) {
			wchar_t text[200];
			Compose(text, 200, p, pl.shown, pl.crowd, kSeq[pl.shown].n, false);
			HintShow(HINT_COMBO, p, text, 300);
			return;
		}
		pl.doneAt = 0;
		pl.shown = -1;
		pl.crowd = false;
		pl.lit = 0;
		HintHide(HINT_COMBO, p);
	}
	// The crowd's Twister: the bar just filled in a fight with more than 100 to beat.
	int toBeat = filled && (learned & 4) && !pl.crowdTold ? GateRomansLeft() : -1;
	if (filled && (learned & 4) && !pl.crowdTold && g_logs < 60)
		Log("combohints: player %d's bar full, the Twister learned: %d Romans to beat (a crowd: more than %d)", p + 1, toBeat,
		    kCrowd), g_logs++;
	if (filled && (learned & 4) && toBeat > kCrowd && !pl.crowdTold && !(pl.n && pl.queue[0] == C_TWISTER)) {
		pl.crowdTold = true;
		pl.crowd = true;
		pl.shown = C_TWISTER;
		pl.until = now + kCrowdMs;
		pl.more = false;
		pl.lit = 0;
		Log("combohints: player %d's bar full with %d Romans to beat: the Twister's hint, 12 s", p + 1, toBeat);
	}
	if (pl.crowd) {
		if ((int)(pl.until - now) <= 0) { pl.crowd = false; pl.shown = -1; pl.lit = 0; HintHide(HINT_COMBO, p); }
	}
	// A combo bought this level: its hint while the bar is full and a Roman near.
	if (!pl.crowd) {
		int c = -1;
		for (int i = 0; i < pl.n && c < 0; i++) if (CanDo(hero, pl.queue[i])) c = pl.queue[i];
		if (c != pl.shown) { pl.shown = -1; pl.lit = 0; HintHide(HINT_COMBO, p); }
		if (c >= 0 && full && NearestRoman2(h.Pos()) <= kNear[c] * kNear[c]) {
			if (pl.shown != c) {
				Log("combohints: player %d's bar full by a Roman: the %S's hint", p + 1, kName[c]);
				pl.lit = 0;
			}
			pl.shown = c;
			pl.until = now + kAfterMs;
		}
		if (pl.shown >= 0 && (int)(pl.until - now) <= 0) { pl.shown = -1; pl.lit = 0; HintHide(HINT_COMBO, p); }
	}
	if (pl.shown < 0) return;
	const Seq& s = kSeq[pl.shown];
	// A combo begun with a jump (A) is pressed in the air: landing before its
	// last button starts it again (hero +0x384 bits 0-2: off its feet).
	bool inAir = (*(uint32_t*)(h.raw() + 0x384) & 7) != 0;
	if (s.b[0] == HB_A && pl.lit > 0 && pl.lit < s.n) {
		if (inAir) pl.air = true;
		else if (pl.air) {
			if (g_logs++ < 60) Log("combohints: player %d landed with %d of the %S's buttons: from the start", p + 1, pl.lit, kName[pl.shown]);
			pl.lit = 0;
			pl.air = false;
		}
	} else {
		pl.air = false;
	}
	int before = pl.lit;
	if (HintOnScreen(HINT_COMBO, p)) Press(pl, p, s, Pressed(p), HINT_COMBO);
	if (pl.crowd && !pl.more && before < 2 && pl.lit >= 2) {
		pl.more = true;
		pl.until += kCrowdMoreMs;
		Log("combohints: player %d pressed two of the Twister's buttons: 8 s more", p + 1);
	}
	wchar_t text[200];
	Compose(text, 200, p, pl.shown, pl.crowd, pl.lit, false);
	HintShow(HINT_COMBO, p, text, (DWORD)((int)(pl.until - now) > 0 ? pl.until - now : 1));
}

// Twister Fusion: both heroes spinning (attack state +0x135 9 / 10, its step
// +0x38E & 0x3F 0x23) within the trio's fusion range (+0x3D8), and the fusion
// can start (both Fusion-ready, the leader's bar not run out: CoopFusionAllowed).
void FuseFrame(Trio t) {
	bool on = false;
	if (t && (Learned() & 8) && !g_fused && !CoopOneScreen() && !CoopSuspended()) {
		int l = t.Leader();
		Hero a = Solo() ? t.GetHero(l) : t.GetHero(CoopP1Hero());
		Hero b = Solo() ? (l == HERO_ASTERIX || l == HERO_OBELIX ? t.GetHero(l ^ 1) : Hero()) : t.GetHero(CoopP2Hero());
		auto spin = [](Hero h) {
			uint8_t s = *(uint8_t*)(h.raw() + 0x135), st = *(uint8_t*)(h.raw() + 0x38E) & 0x3F;
			return (s == 9 || s == 10) && st == 0x23;
		};
		if (a && b && spin(a) && spin(b) && CoopFusionAllowed(t.Leader())) {
			Vector3 d = a.Pos() - b.Pos();
			float r = *(float*)(t.raw() + 0x3D8);
			on = d.len2() < r * r;
		}
	}
	if (on != g_fuseUp) {
		g_fuseUp = on;
		if (!on) { HintHide(HINT_FUSE); g_p[0].fuseLit = g_p[1].fuseLit = 0; }
		else Log("combohints: both Twisters near: X fuses them (both players' hints)");
	}
	if (!on) return;
	for (int p = 0; p < Players(); p++) {
		Player& pl = g_p[p];
		if (HintOnScreen(HINT_FUSE, p) && (Pressed(p) & (1u << HB_X)) && !pl.fuseLit) {
			pl.fuseLit = 1;
			HintSpark(HINT_FUSE, p, 0, false);
		}
		wchar_t text[200];
		Compose(text, 200, p, C_FUSION, false, pl.fuseLit, true);
		HintShow(HINT_FUSE, p, text, 500);
	}
}

} // namespace

void ComboHintsText(wchar_t* out, int n, int player, int kind, bool crowd, int lit) {
	if (kind < 0 || kind >= C_COUNT) { out[0] = 0; return; }
	Compose(out, n, player, kind, crowd, lit, kind == C_FUSION);
}

void ComboHintsQueue(int kind) {
	if (kind == -2) {   // the Twister learned quietly (no line): the crowd's hint alone
		char* gm = GM();
		char* sg = gm ? *(char**)(gm + 0x40) : nullptr;
		if (sg) *(uint32_t*)(sg + 0x10) |= 4;
		g_bits |= 4;
		Log("combohints: the Twister learned, no line (autotest)");
		return;
	}
	if (kind < 0) {
		for (Player& pl : g_p) pl.crowdTold = false;
		Log("combohints: the crowd's Twister hint may come again (autotest)");
		return;
	}
	bool first = kind >= 100;   // 100 + k: combo k first in line
	if (first) kind -= 100;
	if (kind >= C_COUNT) return;
	for (int p = 0; p < Players(); p++) {
		Player& pl = g_p[p];
		Push(pl, HeroOf(p), kind);
		for (int i = 1; first && i < pl.n; i++)
			if (pl.queue[i] == kind) { for (int k = i; k > 0; k--) pl.queue[k] = pl.queue[k - 1]; pl.queue[0] = kind; }
	}
	Log("combohints: the %S queued for both players%s (autotest)", kName[kind], first ? ", first in line" : "");
}

void ComboHintsFrame() {
	char* gm = GM();
	char* itf = (char*)GetLvlObject(2, 132, 0);
	int level = gm ? *(int*)(gm + ax::kGM_currentLevel) : -1;
	if (level != g_level || itf != g_itf) { g_itf = itf; ResetLevel(level); }
	Trio t = GetTrio();
	// ([hints] hint_combos off: none; single player only with [single_player] sp_combo_hints)
	if (!gm || !t || (Solo() && !g_config.spComboHints) || !itf || !g_config.hintCombos) {
		HintHide(HINT_COMBO);
		HintHide(HINT_FUSE);
		g_bitsKnown = false;
		return;
	}
	// Combos learned this level (bought, or the cheat): each player's line.
	uint32_t bits = Learned() & 0x3F;
	if (!g_bitsKnown) { g_bits = bits; g_bitsKnown = true; }
	uint32_t added = bits & ~g_bits;
	g_bits = bits;
	for (int c = 0; c < C_COUNT && added; c++) {
		if (!(added & (1u << c))) continue;
		for (int p = 0; p < Players(); p++) Push(g_p[p], HeroOf(p), c);
		Log("combohints: the %S learned (bought this level: %d) -- its hint waits for a full bar by a Roman", kName[c],
		    *(uint8_t*)(gm + 0x400 + c));
	}
	// (Single player: the leader's Twister merging or fused, +0x38E & 0x3F 0x25 / 0x26.)
	Hero lh = t.GetHero(t.Leader());
	uint8_t lstep = lh ? *(uint8_t*)(lh.raw() + 0x38E) & 0x3F : 0;
	bool soloFused = Solo() && lh && *(uint8_t*)(lh.raw() + 0x135) == 10 && (lstep == 0x25 || lstep == 0x26);
	if ((SharedCurrentMode() == SHARED_FUSION || soloFused) && !g_fused) {
		g_fused = true;
		Log("combohints: Twister Fusion: its X hint is done for this level");
	}
	if (CoopSuspended() || CoopOneScreen()) {
		HintHide(HINT_COMBO);
		HintHide(HINT_FUSE);
		g_fuseUp = false;
		return;
	}
	for (int p = 0; p < Players(); p++) PlayerFrame(p, t);
	FuseFrame(t);
}

bool ComboHintsInstallHooks() {
	return InstallDetour(kSpend, (void*)SpendHook, (void**)&g_origSpend, kSpendPrologue, sizeof(kSpendPrologue),
	                     "combo bar spent (a combo done: combo hints)");
}
