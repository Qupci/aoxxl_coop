#include "life.h"
#include "autotest.h"
#include "coop.h"
#include "gamecam.h"
#include "log.h"

namespace {

typedef int(__fastcall* LifeTickFn)(void* self, void* edx, int arg);

LifeTickFn g_origAsterixLifeTick = nullptr;
LifeTickFn g_origObelixLifeTick  = nullptr;

volatile uint8_t g_lastState[HERO_COUNT] = { 0xFF, 0xFF, 0xFF };

// Which trio slot does this Life's hero occupy? The Life class says which hero
// it was built for, but asking the trio is what the rest of the mod keys on.
int HeroIndexOfLife(void* life) {
	KHook* hook = *(KHook**)((char*)life + axl::kLife_hero);
	Trio t = GetTrio();
	if (!t || !hook) return -1;
	for (int i = 0; i < HERO_COUNT; i++)
		if (t.GetHero(i).hook == hook) return i;
	return -1;
}

int LifeTickCommon(LifeTickFn orig, void* self, void* edx, int arg) {
	int idx = HeroIndexOfLife(self);
	if (idx < 0) return orig(self, edx, arg);

	Hero hero;
	hero.hook = *(KHook**)((char*)self + axl::kLife_hero);

	if (idx == HERO_ASTERIX) InterlockedIncrement(&g_hookCounters.asterixTicks);
	else if (idx == HERO_OBELIX) InterlockedIncrement(&g_hookCounters.obelixTicks);

	AutotestBeforeLifeTick(idx, hero);
	CoopBeforeLifeTick(idx, hero);
	GameCamBeginHeroTick(idx);
	int r = orig(self, edx, arg);
	GameCamEndHeroTick();
	CoopAfterLifeTick(idx, hero);
	AutotestAfterLifeTick(idx, hero);

	g_lastState[idx] = *(uint8_t*)((char*)self + axl::kLife_state);
	return r;
}

int __fastcall AsterixLifeTickHook(void* self, void* edx, int arg) {
	return LifeTickCommon(g_origAsterixLifeTick, self, edx, arg);
}

// Idefix (CKHkIdefixLife, vtable 0x625DA0, tick 0x4AC560) answers the *leader's*
// RB (hero +0x3EC bit 0x100, set for one frame by the hero's input processor on
// the ground): his AI chooser (0x49EE80) sends him to bite an enemy near the
// leader (AI order 6), his sound handler (0x4AA080) plays the "no" sound when
// there is none. Player 2's RB, while not leading, went nowhere. When the
// non-leading player's hero has RB this frame, Idefix's tick runs with that hero
// as the trio's leader.
const uintptr_t kIdefixLifeVtbl = 0x625DA0;
const uintptr_t kIdefixLifeTick = 0x4AC560;
LifeTickFn g_origIdefixLifeTick = nullptr;

int __fastcall IdefixLifeTickHook(void* self, void* edx, int arg) {
	int nl = CoopNonLeaderPlayer();
	Trio t = GetTrio();
	Hero h = (t && nl >= 0 && !CoopSuspended()) ? t.GetHero(nl) : Hero();
	if (!h || !(*(uint32_t*)(h.raw() + 0x3EC) & 0x100) || CoopHeroHeld(nl))
		return g_origIdefixLifeTick(self, edx, arg);
	uint8_t* tr = (uint8_t*)t.raw();
	uint8_t lead = tr[0x2C], comp = tr[0x2D];
	tr[0x2C] = (uint8_t)nl;
	tr[0x2D] = lead;
	// Idefix's range gate (+0xBDC bit 1: the hero beyond +0xB80 -> no bite) is
	// for the leader he follows; the other player calls him from anywhere.
	Hero idefix = t.GetHero(HERO_IDEFIX);
	uint32_t* gate = idefix ? (uint32_t*)(idefix.raw() + 0xBDC) : nullptr;
	uint32_t gateBit = gate ? (*gate & 2) : 0;
	if (gate) *gate &= ~2u;
	*(uint32_t*)(tr + 0x3F4) &= ~1u;   // the trio's enemy list (vf62..) is built around the leader
	Log("life: %s pressed RB (not leading): Idefix's tick with %s as the leader", HeroName(nl), HeroName(nl));
	int r = g_origIdefixLifeTick(self, edx, arg);
	if (gate) *gate |= gateBit;
	if (tr[0x2C] == (uint8_t)nl) { tr[0x2C] = lead; tr[0x2D] = comp; }
	*(uint32_t*)(tr + 0x3F4) &= ~1u;
	return r;
}

int __fastcall ObelixLifeTickHook(void* self, void* edx, int arg) {
	return LifeTickCommon(g_origObelixLifeTick, self, edx, arg);
}

} // namespace

uint8_t LifeLastState(int heroIndex) {
	if (heroIndex < 0 || heroIndex >= HERO_COUNT) return 0xFF;
	return g_lastState[heroIndex];
}

bool LifeInstallHooks() {
	bool ok = true;
	ok &= PatchVtableSlot(axl::kAsterixLifeVtbl, axl::kLifeTickSlot, axl::kAsterixLifeTick,
	                      (void*)AsterixLifeTickHook, (void**)&g_origAsterixLifeTick,
	                      "CKHkAsterixLife::tick");
	ok &= PatchVtableSlot(axl::kObelixLifeVtbl, axl::kLifeTickSlot, axl::kObelixLifeTick,
	                      (void*)ObelixLifeTickHook, (void**)&g_origObelixLifeTick,
	                      "CKHkObelixLife::tick");
	ok &= PatchVtableSlot(kIdefixLifeVtbl, axl::kLifeTickSlot, kIdefixLifeTick, (void*)IdefixLifeTickHook,
	                      (void**)&g_origIdefixLifeTick, "CKHkIdefixLife::tick");
	return ok;
}
