// Interaction tracing: logs the events that interactive objects receive while
// player 2 is in, with the trio's leader at that moment and the address of the
// code that sent the event. Used to find out where player 2's attempt at an
// interaction (spring, swing, iron crate...) goes a different way from player
// 1's. Pure logging: every call is passed straight through.

#include "coop.h"
#include "log.h"
#include "kal.h"
#include "trio.h"

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
};
volatile long g_traceLines = 0;

int __fastcall TracedSendEvent(void* self, void* edx, int event, void* param) {
	uintptr_t vt = *(uintptr_t*)self;
	const Traced* t = nullptr;
	for (const Traced& c : g_traced) if (c.vtbl == vt) { t = &c; break; }
	if (!t) return 0;   // cannot happen: only these vtables point here
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
