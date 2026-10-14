#include "hud2.h"
#include "coop.h"
#include "config.h"
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
// Their state in the interface: +0x24 bits, and three ranges.
const uint32_t kStateBits = 0x30 | 0x7000 | 0x1800000;
struct Range { int from, to; };
const Range kRanges[] = { { 0x1E4, 0x278 }, { 0x2A0, 0x310 }, { 0x494, 0x4A0 } };

const uintptr_t kVtNode = 0x6231D0, kVtParticles = 0x622FCC, kVtAnimNode = 0x623128;

bool IsNodeVt(uintptr_t vt) { return vt == kVtNode || vt == kVtParticles || vt == kVtAnimNode || vt == 0x62317C || vt == 0x6230D4; }

struct Elem {
	uint32_t f10 = 0, f20 = 0, atom4 = 0;
	uint8_t  anim[0x10] = {};
	float    frame[32] = {};   // frame +0x10..+0x8F: modelling and LTM matrices
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
	// Render: what was moved or hidden for a pass.
	struct Moved { char* p; float v; } moved2d[8];
	int      nMoved2d = 0;
	struct MovedFrame { float* f; float lt[3], md[3]; } movedFrames[4];
	int      nMovedFrames = 0;
	struct Hidden { char* node; uint32_t f10; } hidden[64];
	int      nHidden = 0;
	bool     p2Pass = false;
	bool     loggedP2 = false;
} g;

char* GM() {
	CKYellowPages* yp = YellowPages();
	return yp ? (char*)yp->gameManager : nullptr;
}

bool On() {
	return IndivActive() && g_config.splitScreen && CoopP2Joined() && !CoopSuspended() && !CoopOneScreen() &&
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

} // namespace

void Hud2BeforeInterfaceTick(char* itf) {
	(void)itf;
	if (!On()) return;
	// Player 1's HUD shows for player 1's own health changes (the game's flag
	// goes up for either hero's).
	char* gm = GM();
	if (!gm) return;
	bool mine = StatsChanged(0, CoopP1Hero());
	*(uint32_t*)(gm + 0x5C) = (*(uint32_t*)(gm + 0x5C) & ~0x4000u) | (mine ? 0x4000u : 0u);
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
	Capture(itf, g.s1);
	if (!g.s2.valid) {
		g.s2 = g.s1;
		Log("hud2: player 2's HUD (health, combo bar) set up from the interface %p", itf);
	}
	Apply(itf, g.s2);
	uint32_t& f = *(uint32_t*)(gm + 0x5C);
	uint32_t saved = f;
	f &= ~kShowFlags;
	if (StatsChanged(1, CoopP2Hero())) f |= 0x4000;
	if (CoopP2ShowsHud()) f |= 0x1000000;
	int prev = IndivEnter(CoopP2Hero());
	if (!(f & 0x100000)) {
		kHealthPart(itf, nullptr, time);
		if (!(f & 0x10000000)) {
			kComboPart(itf, nullptr, time);
			kComboGlowPart(itf, nullptr, time);
		}
	}
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
	const int o2d[] = { 0x134, 0x13C, 0x150, 0x154, 0x158 };
	for (int o : o2d) {
		char* e = *(char**)(itf + o);
		if (!e || g.nMoved2d >= 8) continue;
		g.moved2d[g.nMoved2d++] = { e, ((float*)e)[5] };
		((float*)e)[5] += dx;
	}
	const float* right = (const float*)((char*)*(void**)((char*)hud + 4) + 0x50);
	const int o3d[] = { 0xA8, 0x130 };
	for (int o : o3d) {
		char* n = *(char**)(itf + o);
		if (!n || IsBadReadPtr(n, 8)) continue;
		if (float* fr = FrameOf(n)) MoveAlong(fr, right, -dx, true);   // screen x is minus the camera's
	}
}

void Hud2EndP1() {
	for (int i = 0; i < g.nMoved2d; i++) ((float*)g.moved2d[i].p)[5] = g.moved2d[i].v;
	for (int i = 0; i < g.nMovedFrames; i++) {
		State::MovedFrame& m = g.movedFrames[i];
		memcpy((char*)m.f + 0x80, m.lt, sizeof(m.lt));
		memcpy((char*)m.f + 0x40, m.md, sizeof(m.md));
	}
	g.nMoved2d = g.nMovedFrames = 0;
}

bool Hud2BeginP2(RwCamera* hud) {
	g.p2Pass = false;
	g.nHidden = 0;
	char* itf = g.itf;
	if (!On() || !hud || !itf || !g.s2.valid || itf != (char*)GetLvlObject(2, 132, 0)) return false;
	// Anything of player 2's to show?
	bool any = false;
	for (int i = 0; i < kNumP2Elems && !any; i++) any = ElemPtr(itf, i) && !(g.s2.e[i].f10 & 2);
	if (!any) return false;
	Capture(itf, g.s1);
	Apply(itf, g.s2);
	// Every other model of the interface hidden for this pass.
	for (int o = 4; o < 0x170; o += 4) {
		bool mine = false;
		for (int i = 0; i < kNumP2Elems; i++) mine |= kP2Elems[i] == o;
		char* n = *(char**)(itf + o);
		if (mine || !n || IsBadReadPtr(n, 0x14) || !IsNodeVt(*(uintptr_t*)n) || g.nHidden >= 64) continue;
		g.hidden[g.nHidden++] = { n, *(uint32_t*)(n + 0x10) };
		*(uint32_t*)(n + 0x10) |= 2;
	}
	// Mirrored to the right edge: each model's screen x (minus its x along
	// the HUD camera's right axis) negated.
	float* cl = (float*)((char*)*(void**)((char*)hud + 4) + 0x50);
	for (int i = 0; i < kNumP2Elems; i++) {
		char* n = ElemPtr(itf, i);
		float* fr = n ? FrameOf(n) : nullptr;
		if (!fr) continue;
		float* lt = (float*)((char*)fr + 0x80);
		float x = (lt[0] - cl[12]) * cl[0] + (lt[1] - cl[13]) * cl[1] + (lt[2] - cl[14]) * cl[2];
		MoveAlong(fr, cl, -2.0f * x, false);
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
	for (int i = 0; i < g.nHidden; i++) *(uint32_t*)(g.hidden[i].node + 0x10) = g.hidden[i].f10;
	g.nHidden = 0;
	Apply(g.itf, g.s1);
	g.p2Pass = false;
}
