#include "life.h"
#include "autotest.h"
#include "coop.h"
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
	int r = orig(self, edx, arg);
	CoopAfterLifeTick(idx, hero);
	AutotestAfterLifeTick(idx, hero);

	g_lastState[idx] = *(uint8_t*)((char*)self + axl::kLife_state);
	return r;
}

int __fastcall AsterixLifeTickHook(void* self, void* edx, int arg) {
	return LifeTickCommon(g_origAsterixLifeTick, self, edx, arg);
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
	return ok;
}
