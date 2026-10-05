// Interaction tracing: logs the events that interactive objects receive while
// player 2 is in, with the trio's leader at that moment and the address of the
// code that sent the event. Used to find out where player 2's attempt at an
// interaction (spring, swing, iron crate...) goes a different way from player
// 1's. Pure logging: every call is passed straight through.

#include "coop.h"
#include "log.h"
#include "kal.h"
#include "trio.h"
#include "inputmgr.h"

#include <cstdio>
#include <cstring>

#include <intrin.h>

namespace {

typedef int(__fastcall* SendEventFn)(void* self, void* edx, int event, void* param);

struct Traced {
	uintptr_t   vtbl;
	uintptr_t   original;   // expected slot-5 value, then the saved original
	const char* name;
};
Traced g_traced[] = {
	{ 0x626264, 0x4BB4C0, "Bumper" },
	{ 0x626110, 0x4B4630, "PushPull" },
	{ 0x62928C, 0x544060, "HkCrate" },
	{ 0x6291E0, 0x5424D0, "CrateCpnt" },
	{ 0x627738, 0x004E3F00, "Roman" },
	{ 0x624058, 0x0043C400, "SoundManager" },   // only its music messages (0x02xx) are logged
};
volatile long g_traceLines = 0;

int __fastcall TracedSendEvent(void* self, void* edx, int event, void* param) {
	uintptr_t vt = *(uintptr_t*)self;
	const Traced* t = nullptr;
	for (const Traced& c : g_traced) if (c.vtbl == vt) { t = &c; break; }
	if (!t) return 0;   // cannot happen: only these vtables point here
	// Music: the sound manager's 0x02xx messages (play track ...), always logged,
	// with both heroes' positions -- what starts it, and for which hero.
	if (t->vtbl == 0x624058) {
		if ((event >> 8) == 0x02) {
			Trio tr = GetTrio();
			Hero a = tr ? tr.GetHero(HERO_ASTERIX) : Hero(), o = tr ? tr.GetHero(HERO_OBELIX) : Hero();
			Log("trace: music message %04X from %p; leader %s; Asterix (%.1f %.1f %.1f) Obelix (%.1f %.1f %.1f)", event,
			    _ReturnAddress(), tr ? HeroName(tr.Leader()) : "?", a ? a.Pos().x : 0, a ? a.Pos().y : 0,
			    a ? a.Pos().z : 0, o ? o.Pos().x : 0, o ? o.Pos().y : 0, o ? o.Pos().z : 0);
		}
		return ((SendEventFn)t->original)(self, edx, event, param);
	}
	// 0x906 is the crates' per-frame "where are you" query: noise.
	if (CoopP2Joined() && event != 0x906 && event != 0x4C01 && InterlockedIncrement(&g_traceLines) < 4000) {
		Trio tr = GetTrio();
		Log("trace: %s %p event %04X param %p leader=%s from %p", t->name, self, event, param,
			tr ? HeroName(tr.Leader()) : "?", _ReturnAddress());
	}
	return ((SendEventFn)t->original)(self, edx, event, param);
}

// CKSrvCollision (yellow pages +0x50) keeps the level's collision tests: an
// array of 0x1C-byte records at +0x1C8 (count u16 at +0x1DC) pairing two
// bounding shapes, each with an index into the receiver table at +0x1CC (12-byte
// entries, receiver hook at +4; count u16 at +0x1DE). Tests that hit send event
// 0x300 to their receivers.
struct CollTest {
	char*    shape1;
	char*    shape2;
	uint16_t life1, life2;
	CollTest *nextHit, *nextMiss, *parent;
	uint16_t flags;
	uint8_t  contact, priority;
};
static_assert(sizeof(CollTest) == 0x1C, "collision test record");

const char* ShapeOwner(char* shape, Trio t) {
	static const struct { int hero; int off; const char* name; } kHeroShapes[] = {
		{ HERO_ASTERIX, 0x228, "Asterix+228" }, { HERO_ASTERIX, 0xC18, "Asterix+C18" },
		{ HERO_ASTERIX, 0xC1C, "Asterix+C1C" }, { HERO_OBELIX, 0x228, "Obelix+228" },
		{ HERO_OBELIX, 0xB84, "Obelix+B84" },   { HERO_OBELIX, 0xB88, "Obelix+B88" },
	};
	for (const auto& k : kHeroShapes) {
		Hero h = t ? t.GetHero(k.hero) : Hero();
		if (h && *(char**)((char*)h.hook + k.off) == shape) return k.name;
	}
	return "";
}

} // namespace

void TraceDumpCollisionTests(void* receiver) {
	CKYellowPages* yp = YellowPages();
	char* srv = yp ? *(char**)((char*)yp + 0x50) : nullptr;
	if (!srv) return;
	CollTest* tests = *(CollTest**)(srv + 0x1C8);
	char* recv = *(char**)(srv + 0x1CC);
	unsigned nTests = *(uint16_t*)(srv + 0x1DC), nRecv = *(uint16_t*)(srv + 0x1DE);
	Trio t = GetTrio();
	Log("trace: collision service %p: %u tests, %u receivers, flags %08X; receiver %p (vt %p)",
		srv, nTests, nRecv, *(uint32_t*)(srv + 0xC), receiver, receiver ? *(void**)receiver : nullptr);
	for (int hero = HERO_ASTERIX; hero <= HERO_OBELIX; hero++) {
		Hero h = t ? t.GetHero(hero) : Hero();
		if (!h) continue;
		int offs[3] = { 0x228, hero == HERO_ASTERIX ? 0xC18 : 0xB84, hero == HERO_ASTERIX ? 0xC1C : 0xB88 };
		for (int off : offs) {
			char* sh = *(char**)((char*)h.hook + off);
			unsigned uses = 0;
			for (unsigned i = 0; i < nTests; i++)
				if (tests[i].shape1 == sh || tests[i].shape2 == sh) uses++;
			Log("trace:   %s+%X shape %p vt %p flags %08X, in %u tests", HeroName(hero), off, sh,
				sh ? *(void**)sh : nullptr, sh ? *(uint32_t*)(sh + 0x18) : 0, uses);
		}
	}
	auto recvAt = [&](uint16_t idx) -> char* {
		return idx < nRecv ? *(char**)(recv + idx * 12 + 4) : nullptr;
	};
	for (unsigned i = 0; i < nTests && receiver; i++) {
		CollTest& c = tests[i];
		char *r1 = recvAt(c.life1), *r2 = recvAt(c.life2);
		if (r1 != receiver && r2 != receiver) continue;
		Log("trace:   test %u: flags %04X prio %u parent %d | %p vt %p f %08X [%s] o %p r %p vt %p | "
			"%p vt %p f %08X [%s] o %p r %p vt %p",
			i, c.flags, c.priority, c.parent ? (int)(c.parent - tests) : -1,
			c.shape1, c.shape1 ? *(void**)c.shape1 : nullptr, c.shape1 ? *(uint32_t*)(c.shape1 + 0x18) : 0,
			ShapeOwner(c.shape1, t), c.shape1 ? *(void**)(c.shape1 + 0x20) : nullptr, r1,
			r1 ? *(void**)r1 : nullptr,
			c.shape2, c.shape2 ? *(void**)c.shape2 : nullptr, c.shape2 ? *(uint32_t*)(c.shape2 + 0x18) : 0,
			ShapeOwner(c.shape2, t), c.shape2 ? *(void**)(c.shape2 + 0x20) : nullptr, r2,
			r2 ? *(void**)r2 : nullptr);
	}
}

bool TraceInstallHooks() {
	bool ok = true;
	for (Traced& c : g_traced) {
		void* orig = nullptr;
		if (PatchVtableSlot(c.vtbl, 5, c.original, (void*)TracedSendEvent, &orig, c.name))
			c.original = (uintptr_t)orig;
		else
			ok = false;
	}
	return ok;
}

// Input bindings: for each mapped action, which device byte feeds it in every
// preset (binding +0x0C -> source; source +0x04.. = one raw-byte pointer per
// preset, +0x14 = the preset in use). Devices: CKInput +0x04 (count +0x10,
// 0xCC each), +0x08 (count +0x11, 0x68 each), +0x0C (count +0x12, 0x3FC each).
void TraceDumpBindings() {
	InputManager im = GetInputManager();
	if (!im) return;
	char* in = im.raw();
	struct Dev { const char* name; char* base; int count, stride; } devs[3] = {
		{ "A", *(char**)(in + 0x04), *(uint8_t*)(in + 0x10), 0xCC },
		{ "B", *(char**)(in + 0x08), *(uint8_t*)(in + 0x11), 0x68 },
		{ "C", *(char**)(in + 0x0C), *(uint8_t*)(in + 0x12), 0x3FC },
	};
	for (auto& d : devs) Log("bind: device %s at %p x%d", d.name, d.base, d.count);
	auto where = [&](char* p, char* out) {
		if (!p) { strcpy(out, "-"); return; }
		for (auto& d : devs)
			if (d.base && p >= d.base && p < d.base + d.count * d.stride) {
				int off = (int)(p - d.base);
				sprintf(out, "%s%d+%03X", d.name, off / d.stride, off % d.stride);
				return;
			}
		sprintf(out, "%p", p);
	};
	int n = im.NumActions();
	for (int i = 0; i < n; i++) {
		char* b = im.Binding(i);
		char* src = *(char**)(b + 0xC);
		char w[5][24];
		for (int k = 0; k < 5; k++) where(src ? *(char**)(src + 4 + 4 * k) : nullptr, w[k]);
		Log("bind: %2d active=%d cont=%d src=%p presets %s %s %s %s cur %s mapper=%p",
		    i, b[4], b[0x10], src, w[0], w[1], w[2], w[3], w[4], *(void**)(b + 8));
	}
}

// Magic potion probe. The potion is one global gauge, [game manager (yellow
// pages +0x8C) +0x40] +0x9C (float, ~300 when full; +0x52 a byte the trio's
// potion code reads), counting down; the trio starts its potion effects when the
// gauge goes above 0 (FUN_0047DE80, trio +0x5AC = running) and ends them at 0
// (FUN_0047FA90). Which of those effects reach which hero is not known yet: from
// the moment the gauge fills, the heroes' and the trio's memory is compared with
// that moment at a few checkpoints and the changed dwords are logged.
namespace {
const int kProbeHeroBytes = 0x1000, kProbeTrioBytes = 0x400;
const DWORD kProbeAt[] = { 1500, 5000, 15000, 30000, 60000 };
struct PotionProbe {
	bool    on = false;
	DWORD   start = 0;
	int     next = 0;
	uint8_t hero[2][kProbeHeroBytes];
	uint8_t trio[kProbeTrioBytes];
	char*   heroPtr[2] = {};
	char*   trioPtr = nullptr;
} g_potion;

void ProbeDiff(const char* what, const uint8_t* before, const char* now, int bytes, int& budget) {
	for (int off = 0; off < bytes && budget > 0; off += 4) {
		uint32_t a = *(const uint32_t*)(before + off), b = *(const uint32_t*)(now + off);
		if (a == b) continue;
		float fa, fb;
		memcpy(&fa, &a, 4); memcpy(&fb, &b, 4);
		Log("potion:   %s+%03X %08X -> %08X (%g -> %g)", what, off, a, b, fa, fb);
		budget--;
	}
}
} // namespace

float PotionGauge() {
	CKYellowPages* yp = YellowPages();
	char* gm = yp ? *(char**)((char*)yp + 0x8C) : nullptr;
	char* prof = gm ? *(char**)(gm + 0x40) : nullptr;
	return prof ? *(float*)(prof + 0x9C) : 0.0f;
}

void SetPotionGauge(float v) {
	CKYellowPages* yp = YellowPages();
	char* gm = yp ? *(char**)((char*)yp + 0x8C) : nullptr;
	char* prof = gm ? *(char**)(gm + 0x40) : nullptr;
	if (prof) *(float*)(prof + 0x9C) = v;
}

void TracePotionOffered() {
	Trio t = GetTrio();
	if (!t || g_potion.on) return;
	for (int i = 0; i < 2; i++) {
		Hero h = t.GetHero(i);
		g_potion.heroPtr[i] = h ? h.raw() : nullptr;
		if (h && !IsBadReadPtr(h.raw(), kProbeHeroBytes)) memcpy(g_potion.hero[i], h.raw(), kProbeHeroBytes);
		else g_potion.heroPtr[i] = nullptr;
	}
	g_potion.trioPtr = t.raw();
	memcpy(g_potion.trio, t.raw(), kProbeTrioBytes);
	g_potion.on = true;
	g_potion.start = GetTickCount();
	g_potion.next = 0;
	Log("potion: gauge filled (%.0f), leader %s; probing the heroes for a minute", PotionGauge(),
	    HeroName(t.Leader()));
}

void TracePotionFrame() {
	static float last = 0.0f;
	float now = PotionGauge();
	if (now > last + 1.0f && GetTrio()) {
		Log("potion: gauge %.1f -> %.1f", last, now);
		g_potion.on = false;
		TracePotionOffered();
	}
	if (last > 0.0f && now <= 0.0f) Log("potion: gauge empty");
	last = now;
	if (!g_potion.on) return;
	DWORD el = GetTickCount() - g_potion.start;
	if (el < kProbeAt[g_potion.next]) return;
	Log("potion: +%.1f s since the offer", el / 1000.0f);
	int budget = 120;
	static const char* names[2] = { "Asterix", "Obelix" };
	for (int i = 0; i < 2; i++)
		if (g_potion.heroPtr[i]) ProbeDiff(names[i], g_potion.hero[i], g_potion.heroPtr[i], kProbeHeroBytes, budget);
	ProbeDiff("trio", g_potion.trio, g_potion.trioPtr, kProbeTrioBytes, budget);
	if (++g_potion.next >= (int)(sizeof(kProbeAt) / sizeof(kProbeAt[0]))) g_potion.on = false;
}
