#include "hud2.h"
#include "coop.h"
#include "config.h"
#include "detour.h"
#include "individual.h"
#include "kal.h"
#include "log.h"
#include "render.h"
#include "trio.h"

#include <cstdlib>
#include <cstring>

namespace {

// The interface's pieces that are player 2's in the second HUD: the 30 shield
// models, their sparkles, the combo bar's models.
const int kP2Elems[] = {
	0x2C, 0x30, 0x34, 0x38, 0x3C, 0x40, 0x44, 0x48, 0x4C, 0x50, 0x54, 0x58, 0x5C, 0x60, 0x64,
	0x68, 0x6C, 0x70, 0x74, 0x78, 0x7C, 0x80, 0x84, 0x88, 0x8C, 0x90, 0x94, 0x98, 0x9C, 0xA0,
	0xA4, 0xB4, 0xB8, 0xBC, 0xC0, 0xC4, 0xCC,
};
const int kNumP2Elems = sizeof(kP2Elems) / sizeof(kP2Elems[0]);
// Their state in the interface: +0x24 bits, and three ranges. Bits 27-28 are
// the combo bar's bounce at a new level (FUN_00519B00, counted down in +0x2EC);
// left shared, a bar going full (bounce 3) bounced the other player's bar too
// -- held at its top, red, with its own count run past 0 (255 bounces), and
// no "Combo" when it did fill.
const uint32_t kStateBits = 0x30 | 0x7000 | 0x1800000 | 0x18000000;
struct Range { int from, to; };
const Range kRanges[] = { { 0x1E4, 0x278 }, { 0x2A0, 0x310 }, { 0x494, 0x4A0 } };

const uintptr_t kVtNode = 0x6231D0, kVtParticles = 0x622FCC, kVtAnimNode = 0x623128;

bool IsNodeVt(uintptr_t vt) { return vt == kVtNode || vt == kVtParticles || vt == kVtAnimNode || vt == 0x62317C || vt == 0x6230D4; }

struct Elem {
	uint32_t f10 = 0, f20 = 0, atom4 = 0;
	uint8_t  anim[0x10] = {};
	float    frame[32] = {};   // frame +0x10..+0x8F: modelling and LTM matrices
	// The shield's paleness (an empty one): FUN_0051B550 sets the alpha byte (+7)
	// of each colour in the node's list (+0x1C: item +4 next, +0xC the colour) --
	// 0x50 empty, 0xFF otherwise; one HUD's update left its paleness on the other's.
	uint8_t  alpha[4] = {};
	uint8_t  nAlpha = 0;
};
struct HudState {
	bool     valid = false;
	uint32_t bits = 0;
	uint8_t  r[3][0x94] = {};
	Elem     e[kNumP2Elems];
};

struct State {
	char*    itf = nullptr;
	HudState s1, s2;           // player 1's (the game's) and player 2's
	int      lastHp[2] = { -1, -1 }, lastShields[2] = { -1, -1 };
	// A health change waiting for that player's HUD to show it: the health part
	// takes the flag (game manager +0x5C bit 0x4000) only in its hidden state --
	// a change while it slides away is shown once it is hidden (as the game's
	// own flag stays up for it).
	bool     pending[2] = {};
	// Render: what was moved or hidden for a pass.
	struct Moved { char* p; float x, y; } moved2d[8];
	int      nMoved2d = 0;
	struct MovedFrame { float* f; float lt[3], md[3]; } movedFrames[4];
	int      nMovedFrames = 0;
	struct Hidden { char* node; uint32_t f10; } hidden[64];
	int      nHidden = 0;
	bool     p2Pass = false;
	bool     inP2Tick = false;         // player 2's copy is being ticked
	float*   mirrorCam = nullptr;      // the HUD camera's LTM / modelling, right axis turned for the pass
	float*   mirrorCamModelling = nullptr;
	void**   renderStateSlot = nullptr;   // engine +0x20 while the mirrored render state function is in
	float*   flipped[8] = {};          // frames turned round on their own for the pass (the combo bar's)
	// Player 1's shield models scaled down for the game's pass (ScaleHealth), to put back after it.
	struct SavedLtm { float* f; float m[16]; } scaled[31];
	int      nScaled = 0;
	int      nFlipped = 0;
	bool     loggedP2 = false;
} g;

char* GM() {
	CKYellowPages* yp = YellowPages();
	return yp ? (char*)yp->gameManager : nullptr;
}

bool On() {
	return IndivActive() && g_config.splitScreen && CoopP2Joined() && !CoopOneScreen() &&
	       CoopShopper() < 0 && CoopP2Hero() >= 0;
}

char* ElemPtr(char* itf, int i) {
	char* n = *(char**)(itf + kP2Elems[i]);
	if (!n || IsBadReadPtr(n, 0x50) || !IsNodeVt(*(uintptr_t*)n)) return nullptr;
	return n;
}
float* FrameOf(char* n) {
	char* fr = *(char**)(n + 4);
	return fr && !IsBadReadPtr(fr, 0x90) ? (float*)fr : nullptr;
}

void Capture(char* itf, HudState& s) {
	s.bits = *(uint32_t*)(itf + 0x24) & kStateBits;
	for (int r = 0; r < 3; r++) memcpy(s.r[r], itf + kRanges[r].from, kRanges[r].to - kRanges[r].from);
	for (int i = 0; i < kNumP2Elems; i++) {
		char* n = ElemPtr(itf, i);
		if (!n) continue;
		Elem& e = s.e[i];
		uintptr_t vt = *(uintptr_t*)n;
		e.f10 = *(uint32_t*)(n + 0x10);
		if (vt == kVtParticles) {
			e.f20 = *(uint32_t*)(n + 0x20);
			char* at = *(char**)(n + 0x48);
			if (at && !IsBadReadPtr(at, 8)) e.atom4 = *(uint32_t*)(at + 4);
		}
		if (vt == kVtAnimNode) memcpy(e.anim, n + 0x30, sizeof(e.anim));
		if (float* f = FrameOf(n)) memcpy(e.frame, (char*)f + 0x10, sizeof(e.frame));
		e.nAlpha = 0;
		if (vt == kVtNode)
			for (char* it = *(char**)(n + 0x1C); it && e.nAlpha < 4 && !IsBadReadPtr(it, 0x10); it = *(char**)(it + 4)) {
				char* c = *(char**)(it + 0xC);
				if (!c || IsBadReadPtr(c, 8)) break;
				e.alpha[e.nAlpha++] = *(uint8_t*)(c + 7);
			}
	}
	s.valid = true;
}

// A frame's LTM (+0x50) from its modelling matrix (+0x10) and its parent's
// LTM (RwFrame parent at +4): the interface writes the modelling matrices and
// leaves the LTMs to the next frame sync, so a copy taken just after its tick
// has stale ones.
void SyncLtm(float* fr) {
	const float* m = (const float*)((char*)fr + 0x10);
	float* l = (float*)((char*)fr + 0x50);
	char* parent = *(char**)((char*)fr + 4);
	if (!parent || IsBadReadPtr(parent, 0x90)) { memcpy(l, m, 16 * sizeof(float)); return; }
	const float* p = (const float*)(parent + 0x50);
	float out[16];
	memcpy(out, m, sizeof(out));
	for (int r = 0; r < 4; r++) {
		const float* a = m + 4 * r;
		for (int k = 0; k < 3; k++) out[4 * r + k] = a[0] * p[k] + a[1] * p[4 + k] + a[2] * p[8 + k] + (r == 3 ? p[12 + k] : 0.0f);
	}
	memcpy(l, out, sizeof(out));
}

void Apply(char* itf, const HudState& s) {
	if (!s.valid) return;
	*(uint32_t*)(itf + 0x24) = (*(uint32_t*)(itf + 0x24) & ~kStateBits) | s.bits;
	for (int r = 0; r < 3; r++) memcpy(itf + kRanges[r].from, s.r[r], kRanges[r].to - kRanges[r].from);
	for (int i = 0; i < kNumP2Elems; i++) {
		char* n = ElemPtr(itf, i);
		if (!n) continue;
		const Elem& e = s.e[i];
		uintptr_t vt = *(uintptr_t*)n;
		*(uint32_t*)(n + 0x10) = e.f10;
		if (vt == kVtParticles) {
			*(uint32_t*)(n + 0x20) = e.f20;
			char* at = *(char**)(n + 0x48);
			if (at && !IsBadReadPtr(at, 8)) *(uint32_t*)(at + 4) = e.atom4;
		}
		if (vt == kVtAnimNode) memcpy(n + 0x30, e.anim, sizeof(e.anim));
		if (vt == kVtNode) {
			int k = 0;
			for (char* it = *(char**)(n + 0x1C); it && k < e.nAlpha && !IsBadReadPtr(it, 0x10); it = *(char**)(it + 4)) {
				char* c = *(char**)(it + 0xC);
				if (!c || IsBadReadPtr(c, 8)) break;
				*(uint8_t*)(c + 7) = e.alpha[k++];
			}
		}
		if (float* f = FrameOf(n)) {
			memcpy((char*)f + 0x10, e.frame, sizeof(e.frame));
			SyncLtm(f);
		}
	}
}

// Did hero `hero`'s health or shields change since its HUD last looked?
bool StatsChanged(int player, int hero) {
	int hp, sh;
	if (!IndivStats(hero, &hp, &sh)) return false;
	bool changed = g.lastHp[player] >= 0 && (hp != g.lastHp[player] || sh != g.lastShields[player]);
	g.lastHp[player] = hp;
	g.lastShields[player] = sh;
	return changed;
}

typedef void(__fastcall* PartFn)(char* itf, void* edx, void* time);
const PartFn kHealthPart = (PartFn)0x51A140, kComboPart = (PartFn)0x518190, kComboGlowPart = (PartFn)0x519E70;

const uint32_t kShowFlags = 0x4000 | 0x1000000;   // game manager +0x5C: health changed, HUD button held

// A frame's place moved by `d` along the HUD camera's right axis (LTM only:
// what the render reads; the next sync or Apply puts it back).
void MoveAlong(float* frame, const float* right, float d, bool remember) {
	float* lt = (float*)((char*)frame + 0x80);
	if (remember && g.nMovedFrames < 4) {
		State::MovedFrame& m = g.movedFrames[g.nMovedFrames++];
		m.f = frame;
		memcpy(m.lt, lt, sizeof(m.lt));
		memcpy(m.md, (char*)frame + 0x40, sizeof(m.md));
	}
	for (int k = 0; k < 3; k++) lt[k] += d * right[k];
}

// The interface's own health readers outside its tick: FUN_0051B550 (which
// shield models show, from the health; thiscall, no args) is also run by the
// game manager's tick through FUN_005154A0 (pause menu, cutscenes) and the
// interface's vf15 -- where the health in the game's place is the standing
// hero's. The HUD's owner's instead.
int HudOwner() {
	if (g.inP2Tick) return CoopP2Hero();
	int shopper = CoopShopper();
	return shopper >= 0 ? shopper : CoopP1Hero();
}
typedef void(__fastcall* VoidFn)(char* self, void* edx);
VoidFn g_origShieldsShown = nullptr, g_origItfVf15 = nullptr;
const uint8_t kShieldsShownPrologue[8] = { 0x83, 0xEC, 0x10, 0xA1, 0xF4, 0x21, 0x66, 0x00 };
void __fastcall ShieldsShownHook(char* self, void* edx) {
	int prev = IndivEnter(HudOwner());
	g_origShieldsShown(self, edx);
	IndivLeave(prev);
}
void __fastcall ItfVf15Hook(char* self, void* edx) {
	int prev = IndivEnter(HudOwner());
	g_origItfVf15(self, edx);
	IndivLeave(prev);
}

// More than four shields (up to six: the merchant's) at the single player HUD's
// size reached past the screen's middle -- into the other player's HUD and the
// helmet counter. The row is scaled down from its first shield, sizes and
// spacing alike (0.08 apart), to fit 0.28 across between the first and the
// last. LTMs only (what the draw reads); `save` keeps them to put back.
void ScaleHealth(char* itf, const float* cl, int shields, bool save) {
	if (shields <= 4) return;
	if (shields > 6) shields = 6;
	float sc = 0.28f / (0.08f * (float)(shields - 1));
	auto camX = [&](float* fr) {
		const float* lt = (const float*)((char*)fr + 0x50);
		return (lt[12] - cl[12]) * cl[0] + (lt[13] - cl[13]) * cl[1] + (lt[14] - cl[14]) * cl[2];
	};
	char* n0 = ElemPtr(itf, 0);
	float* f0 = n0 ? FrameOf(n0) : nullptr;
	if (!f0) return;
	float x0 = camX(f0);
	for (int i = 0; i <= 30; i++) {
		char* n = ElemPtr(itf, i);
		float* fr = n ? FrameOf(n) : nullptr;
		if (!fr) continue;
		float* lt = (float*)((char*)fr + 0x50);
		if (save && g.nScaled < 31) {
			g.scaled[g.nScaled].f = fr;
			memcpy(g.scaled[g.nScaled].m, lt, sizeof(g.scaled[0].m));
			g.nScaled++;
		}
		float x = camX(fr);
		float d = x0 + (x - x0) * sc - x;
		for (int k = 0; k < 3; k++) {
			lt[12 + k] += d * cl[k];
			lt[k] *= sc; lt[4 + k] *= sc; lt[8 + k] *= sc;
		}
	}
}

// The health part's hidden state (+0x24 bits 4-5 == 0) with shield models
// still showing: FUN_005154A0 (the game manager's, on its HUD toggles -- a
// hero going down) puts the part in that state and shows the models for the
// pause menu; outside the pause menu nothing hides them again, and a fallen
// hero's HUD stayed up for good. Hidden, as the part's own hiding leaves them.
void HideIdleShields(char* itf) {
	char* gm = GM();
	if (!gm || (*(uint32_t*)(gm + 0x5C) & 0x100000) || (*(uint32_t*)(itf + 0x24) & 0x30)) return;
	for (int i = 0; i < 30; i++)
		if (char* n = ElemPtr(itf, i)) *(uint32_t*)(n + 0x10) |= 2;
}

typedef int(__cdecl* RenderStateFn)(int state, int value);
RenderStateFn g_origRenderState = nullptr;
bool g_swapCull = true;   // the health part's pass only
int __cdecl MirroredRenderState(int state, int value) {
	if (g_swapCull && state == 0x14 && (value == 2 || value == 3)) value = 5 - value;
	return g_origRenderState(state, value);
}

} // namespace

bool Hud2InstallHooks() {
	bool ok = InstallDetour(0x51B550, (void*)ShieldsShownHook, (void**)&g_origShieldsShown, kShieldsShownPrologue,
	                        sizeof(kShieldsShownPrologue), "HUD shields shown (the HUD owner's health)");
	ok &= PatchVtableSlot(0x6285F0, 15, 0x5162A0, (void*)ItfVf15Hook, (void**)&g_origItfVf15,
	                      "CKHkInterfaceInGame::vf15 (the HUD owner's health)");
	return ok;
}

void Hud2BeforeInterfaceTick(char* itf) {
	(void)itf;
	if (!On()) return;
	// Player 1's HUD shows for player 1's own health changes (the game's flag
	// goes up for either hero's).
	char* gm = GM();
	if (!gm) return;
	if (StatsChanged(0, CoopP1Hero())) g.pending[0] = true;
	*(uint32_t*)(gm + 0x5C) = (*(uint32_t*)(gm + 0x5C) & ~0x4000u) | (g.pending[0] ? 0x4000u : 0u);
}

void Hud2AfterInterfaceTick(char* itf, void* time) {
	if (!On() || !itf) return;
	char* gm = GM();
	if (!gm) return;
	if (itf != g.itf) {   // another level's interface
		g.itf = itf;
		g.s2.valid = false;
		g.lastHp[1] = g.lastShields[1] = -1;
	}
	HideIdleShields(itf);   // player 1's
	// Player 1's change shown (the health part took the flag)?
	if (g.pending[0] && !(*(uint32_t*)(gm + 0x5C) & 0x4000)) g.pending[0] = false;
	Capture(itf, g.s1);
	static const bool posLog = getenv("XXL_HUDPOS") != nullptr;
	static DWORD lastP1 = 0;
	if (posLog && GetTickCount() - lastP1 > 1000) {
		lastP1 = GetTickCount();
		char vis[40] = {};
		for (int i = 0; i < 30; i++) vis[i] = ElemPtr(itf, i) && !(*(uint16_t*)(ElemPtr(itf, i) + 0x10) & 2) ? '1' : '0';
		int hp = -1, sh = -1;
		IndivStats(CoopP1Hero(), &hp, &sh);
		Log("hud2: p1 bits %08X 270 %u 274 %u/%u (p1 hp %d) shown %s", *(uint32_t*)(itf + 0x24), *(uint32_t*)(itf + 0x270),
		    *(uint8_t*)(itf + 0x274), *(uint8_t*)(itf + 0x275), hp, vis);
	}
	{   // the combo bars' states (bits 12-14 the bar, 23-24 the "Combo" word, 27-28 a level's bounce) and the pause's HUD flags
		static uint32_t lastKey = ~0u;
		static int logged = 0;
		uint32_t key = (g.s1.bits & 0x19807000) | (g.s2.valid ? (g.s2.bits & 0x19807000) >> 1 : 0) | (*(uint32_t*)(gm + 0x5C) & 0x1C000000) >> 26;
		if (key != lastKey && logged < 60) {
			lastKey = key;
			logged++;
			Log("hud2: bars p1 %08X p2 %08X, gm+5C %08X", *(uint32_t*)(itf + 0x24) & 0x19807000,
			    g.s2.valid ? g.s2.bits & 0x19807000 : 0, *(uint32_t*)(gm + 0x5C));
		}
	}
	if (!g.s2.valid) {
		g.s2 = g.s1;
		Log("hud2: player 2's HUD (health, combo bar) set up from the interface %p", itf);
	}
	Apply(itf, g.s2);
	uint32_t& f = *(uint32_t*)(gm + 0x5C);
	uint32_t saved = f;
	f &= ~kShowFlags;
	if (StatsChanged(1, CoopP2Hero())) g.pending[1] = true;
	if (g.pending[1]) f |= 0x4000;
	if (CoopP2ShowsHud()) f |= 0x1000000;
	int prev = IndivEnter(CoopP2Hero());
	g.inP2Tick = true;
	if (!(f & 0x100000)) {
		kHealthPart(itf, nullptr, time);
		if (!(f & 0x10000000)) {
			kComboPart(itf, nullptr, time);
			kComboGlowPart(itf, nullptr, time);
		}
	}
	g.inP2Tick = false;
	if (g.pending[1] && !(f & 0x4000)) g.pending[1] = false;   // shown
	HideIdleShields(itf);   // player 2's
	IndivLeave(prev);
	f = (f & ~kShowFlags) | (saved & kShowFlags);
	Capture(itf, g.s2);
	Apply(itf, g.s1);
}

void Hud2BeginP1(RwCamera* hud) {
	g.nMoved2d = 0;
	g.nMovedFrames = 0;
	char* itf = g.itf;
	if (!On() || !hud || !itf || itf != (char*)GetLvlObject(2, 132, 0)) return;
	// The helmet counter (plate +0x134 drawn leftwards from its place, texts,
	// helmet model +0xA8; the x3 / x10 sign +0x13C / +0x130 beside it) to the
	// top middle: player 2's health takes the top right.
	float* plate = *(float**)(itf + 0x134);
	if (!plate) return;
	float dx = 0.5f - (plate[5] - plate[13] * 0.5f);
	float* cl = (float*)((char*)*(void**)((char*)hud + 4) + 0x50);   // right +0, up +4
	auto move2d = [&](int o, float ddx, float ddy) {
		char* e = *(char**)(itf + o);
		if (!e || g.nMoved2d >= 8) return;
		float* f = (float*)e;
		g.moved2d[g.nMoved2d++] = { e, f[5], f[6] };
		f[5] += ddx;
		f[6] += ddy;
	};
	auto move3d = [&](int o, float ddx, float ddy) {
		char* n = *(char**)(itf + o);
		float* fr = n && !IsBadReadPtr(n, 8) ? FrameOf(n) : nullptr;
		if (!fr) return;
		MoveAlong(fr, cl, -ddx, true);   // screen x is minus the camera's
		MoveAlong(fr, cl + 4, ddy, false);
	};
	// (+0x154 is the slide's helmet requirement text, under its own plate +0x138
	// at the right: it stays.)
	const int helmet2d[] = { 0x134, 0x150 };
	for (int o : helmet2d) move2d(o, dx, 0.0f);
	move3d(0xA8, dx, 0.0f);
	// The x3 / x10 sign (FUN_0051E440: +0x3A0 slides from 1.01, off the right
	// edge, to +0x31C and back) goes up off the top instead, like the helmets.
	float shown = *(float*)(itf + 0x31C), cur = *(float*)(itf + 0x3A0);
	float off = cur - shown;
	float p = 1.01f - shown > 0.01f ? off / (1.01f - shown) : 0.0f;
	if (p < 0.0f) p = 0.0f;
	if (p > 1.0f) p = 1.0f;
	move2d(0x13C, dx - off, p * 0.5f);
	move2d(0x158, dx - off, p * 0.5f);
	move3d(0x130, dx - off, p * 0.5f);
	int hp = 0, shields = 3;
	if (IndivStats(CoopP1Hero(), &hp, &shields)) ScaleHealth(itf, cl, shields, true);
}

void Hud2EndP1() {
	for (int i = 0; i < g.nScaled; i++) memcpy((char*)g.scaled[i].f + 0x50, g.scaled[i].m, sizeof(g.scaled[i].m));
	g.nScaled = 0;
	for (int i = 0; i < g.nMoved2d; i++) {
		((float*)g.moved2d[i].p)[5] = g.moved2d[i].x;
		((float*)g.moved2d[i].p)[6] = g.moved2d[i].y;
	}
	for (int i = 0; i < g.nMovedFrames; i++) {
		State::MovedFrame& m = g.movedFrames[i];
		memcpy((char*)m.f + 0x80, m.lt, sizeof(m.lt));
		memcpy((char*)m.f + 0x40, m.md, sizeof(m.md));
	}
	g.nMoved2d = g.nMovedFrames = 0;
}

bool Hud2BeginP2(RwCamera* hud, int part) {
	g.p2Pass = false;
	g.nHidden = 0;
	char* itf = g.itf;
	if (!On() || !hud || !itf || !g.s2.valid || itf != (char*)GetLvlObject(2, 132, 0)) return false;
	// The game's HUD hidden: a menu's interface in the game manager's place
	// for the current one (+0xF8: the pause menu's -- the in-game interface,
	// type +0x28 == 9, is the one the HUD's pieces run for), or its HUD hidden
	// for a pause (+0x5C bit 27, FUN_00515830 / FUN_00515A30) -- player 2's too.
	char* gm = GM();
	char* cur = gm ? *(char**)(gm + 0xF8) : nullptr;
	static char* lastCur = nullptr;
	if (cur != lastCur) {
		lastCur = cur;
		Log("hud2: the game's interface is now %p (vt %p, type %d)%s", cur, cur ? *(void**)cur : nullptr,
		    cur ? *(uint8_t*)(cur + 0x28) : -1, cur == itf ? " -- the in-game one" : " -- player 2's HUD hidden");
	}
	if (!gm || cur != itf || (*(uint32_t*)(gm + 0x5C) & 0x8000000)) return false;
	// Part 0: the health (shields, sparkles: elements 0-30); part 1: the combo
	// bar (31-). The shields and the combo bar are in the same draw list (0),
	// and each needs its own culling (below), so each is a pass of its own.
	int from = part == 0 ? 0 : 31, to = part == 0 ? 31 : kNumP2Elems;
	bool any = false;
	for (int i = from; i < to && !any; i++) any = ElemPtr(itf, i) && !(g.s2.e[i].f10 & 2);
	if (!any) return false;
	Capture(itf, g.s1);
	Apply(itf, g.s2);
	// Every other model of the interface hidden for this pass.
	for (int o = 4; o < 0x170; o += 4) {
		bool mine = false;
		for (int i = from; i < to; i++) mine |= kP2Elems[i] == o;
		char* n = *(char**)(itf + o);
		if (mine || !n || IsBadReadPtr(n, 0x14) || !IsNodeVt(*(uintptr_t*)n) || g.nHidden >= 64) continue;
		g.hidden[g.nHidden++] = { n, *(uint32_t*)(n + 0x10) };
		*(uint32_t*)(n + 0x10) |= 2;
	}
	// Mirrored -- player 1's HUD seen in a mirror, models and all: the first
	// shield at the right edge, more of them leftwards, the last (the first to
	// go) leftmost, each shield eaten from its left, the combo bar at the right
	// edge. The HUD camera (on the HUD's middle) has its right axis turned round
	// for the pass. That turns every face round too: the draw lists set the
	// culling themselves (rwRENDERSTATECULLMODE 0x14, through the engine's
	// render state function at engine +0x20), so for the pass that function is
	// one that swaps "cull back" (2) and "cull front" (3).
	float* cl = (float*)((char*)*(void**)((char*)hud + 4) + 0x50);
	float* cm = (float*)((char*)*(void**)((char*)hud + 4) + 0x10);
	if (part == 0) {   // as many shields as player 1's row fits (before the mirror; Apply puts them back)
		int hp = 0, shields = 3;
		if (IndivStats(CoopP2Hero(), &hp, &shields)) ScaleHealth(itf, cl, shields, false);
	}
	g.mirrorCam = cl;
	g.mirrorCamModelling = cm;
	for (int k = 0; k < 3; k++) { cl[k] = -cl[k]; cm[k] = -cm[k]; }
	// The combo bar's models (+0xB4..+0xCC) turned round on their own as well:
	// mirrored twice they read the right way ("Combo" on the full bar), in
	// their mirrored place.
	g.nFlipped = 0;
	g_swapCull = part == 0;   // the combo bar, mirrored twice, keeps the game's culling
	for (int i = 31; part == 1 && i < kNumP2Elems && g.nFlipped < 8; i++) {
		char* n = ElemPtr(itf, i);
		float* fr = n ? FrameOf(n) : nullptr;
		if (!fr || *(uintptr_t*)n == kVtParticles) continue;
		float* lt = (float*)((char*)fr + 0x50);
		float* md = (float*)((char*)fr + 0x10);
		for (int k = 0; k < 3; k++) { lt[k] = -lt[k]; md[k] = -md[k]; }
		g.flipped[g.nFlipped++] = fr;
	}
	char* engine = *(char**)0x66885C;
	if (engine) {
		g.renderStateSlot = (void**)(engine + 0x20);
		g_origRenderState = (RenderStateFn)*g.renderStateSlot;
		*g.renderStateSlot = (void*)MirroredRenderState;
	}
	g.p2Pass = true;
	static const bool posLog = getenv("XXL_HUDPOS") != nullptr;
	static DWORD lastPos = 0;
	if (posLog && GetTickCount() - lastPos > 1000) {
		lastPos = GetTickCount();
		Log("hud2: p2 state bits %08X (p1 %08X), health fields 230 %.3f 24C %.3f 254 %.3f 270 %u", g.s2.bits, g.s1.bits,
		    *(float*)(itf + 0x230), *(float*)(itf + 0x24C), *(float*)(itf + 0x254), *(uint32_t*)(itf + 0x270));
		for (int i = 0; i < kNumP2Elems; i += (i < 30 ? 5 : 1)) {
			char* n = ElemPtr(itf, i);
			float* fr = n ? FrameOf(n) : nullptr;
			if (!fr) continue;
			float* lt = (float*)((char*)fr + 0x80);
			float d[3] = { lt[0] - cl[12], lt[1] - cl[13], lt[2] - cl[14] };
			Log("hud2:   +%02X flags %04X cam %.3f %.3f (p1 flags %04X y %.3f)", kP2Elems[i], *(uint16_t*)(n + 0x10),
			    d[0] * cl[0] + d[1] * cl[1] + d[2] * cl[2], d[0] * cl[4] + d[1] * cl[5] + d[2] * cl[6],
			    g.s1.e[i].f10 & 0xFFFF, g.s1.e[i].frame[16 + 13] - cl[13]);
		}
	}
	if (!g.loggedP2) {
		g.loggedP2 = true;
		Log("hud2: player 2's HUD drawn (mirrored to the right edge; %d other models hidden for it)", g.nHidden);
	}
	return true;
}

void Hud2EndP2() {
	if (!g.p2Pass) return;
	for (int i = 0; i < g.nFlipped; i++) {
		float* lt = (float*)((char*)g.flipped[i] + 0x50);
		float* md = (float*)((char*)g.flipped[i] + 0x10);
		for (int k = 0; k < 3; k++) { lt[k] = -lt[k]; md[k] = -md[k]; }
	}
	g.nFlipped = 0;
	if (g.renderStateSlot) {
		*g.renderStateSlot = (void*)g_origRenderState;
		g.renderStateSlot = nullptr;
		g_origRenderState(0x14, 2);   // as FUN_00414210 leaves it
	}
	if (g.mirrorCam) {
		for (int k = 0; k < 3; k++) { g.mirrorCam[k] = -g.mirrorCam[k]; g.mirrorCamModelling[k] = -g.mirrorCamModelling[k]; }
		g.mirrorCam = g.mirrorCamModelling = nullptr;
	}
	// What the pass did to player 2's models stays theirs: a finished
	// particle burst (shield chips, combo sparkles) stops only in the walk.
	for (int i = 0; i < g.nHidden; i++) *(uint32_t*)(g.hidden[i].node + 0x10) = g.hidden[i].f10;
	g.nHidden = 0;
	Capture(g.itf, g.s2);   // after the un-hiding: the other part's models are player 2's too
	Apply(g.itf, g.s1);
	g.p2Pass = false;
}
