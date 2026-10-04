#include "proximity.h"
#include "coop.h"
#include "log.h"
#include "trio.h"

#include <cmath>

namespace {

const uintptr_t kBundleVtbl     = 0x6240C4;   // CKBundle (12,3)
const size_t    kBundleTickSlot = 17;
const uintptr_t kBundleTick     = 0x43DFE0;

// Lives that must keep the real arrangement: the heroes' own (their ticks do
// their own slicing in coop.cpp) and the in-game HUD.
const uintptr_t kAsterixLifeVtbl = 0x625C90;
const uintptr_t kObelixLifeVtbl  = 0x625D18;
const uintptr_t kIdefixLifeVtbl  = 0x625DA0;
const uintptr_t kHudHookVtbl     = 0x6285F0;   // CKHkInterfaceInGame (2,132)
const uintptr_t kBonusGroupLife  = 0x6296FC;   // CKGrpBonusLife: handled per bonus below

// Bonus pools (helmets and the like). CKGrpBonusPool::vf27 updates every active
// bonus, then attracts those within the pool's radius of the leader's position
// to the leader (bonus->vf48(hero, 0)); a bonus then flies to the hero it was
// given and is collected by it. After each pool update the same attraction pass
// is run for player 2's hero, with the pool's own "may this hero collect"
// check (vf39). CKGrpAsterixBonusPool::vf27 calls the base function directly,
// so both vtables are hooked.
const uintptr_t kBonusPoolVtbl        = 0x629658;   // CKGrpBonusPool (4,61)
const uintptr_t kAsterixBonusPoolVtbl = 0x62808C;   // CKGrpAsterixBonusPool (4,63)
const size_t    kPoolUpdateSlot       = 27;
const uintptr_t kBonusPoolUpdate        = 0x54C380;
const uintptr_t kAsterixBonusPoolUpdate = 0x5072C0;

// Objects further than this from both players keep the real leader.
const float kMaxRange2 = 90.0f * 90.0f;

typedef void(__fastcall* TickFn)(void* self, void* edx, void* dt);
TickFn g_origBundleTick = nullptr;
// __thiscall with one stack argument (dt): CKGrpBonusLife::vf17 passes it and
// the function ends in `ret 4` (the decompiler shows it as taking none).
typedef void(__fastcall* PoolFn)(void* self, void* edx, void* dt);
PoolFn g_origPoolUpdate = nullptr;
PoolFn g_origAsterixPoolUpdate = nullptr;
volatile long g_slices = 0;
volatile long g_p2Bonuses = 0;
bool g_loggedOverride = false;

bool NodePos(void* node, Vector3* out) {
	if (!node) return false;
	char* frame = *(char**)((char*)node + 4);
	if (!frame) return false;
	const float* p = (const float*)(frame + 0x80);   // LTM position
	if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
	if (p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f) return false;
	*out = Vector3(p[0], p[1], p[2]);
	return true;
}

// Where in the world is this Life's object? Hook lives point at their hook
// (life+0x10); group lives at their group (life+0x0C, as CKGrpTrioLife and
// CKGrpBonusLife read it), located by its first child hook with a node.
bool LifePos(int* life, bool isGroup, Vector3* out) {
	char* owner = *(char**)((char*)life + (isGroup ? 0x0C : 0x10));
	if (!owner) return false;
	if (!isGroup) {
		if (*(uintptr_t*)owner == kHudHookVtbl) return false;
		return NodePos(*(void**)(owner + 0x10), out);
	}
	for (char* hook = *(char**)(owner + 0x20); hook; hook = *(char**)(hook + 4))
		if (NodePos(*(void**)(hook + 0x10), out)) return true;
	return false;
}

void TickLife(int* life, void* dt, bool isGroup, Trio t, int p1, int p2, const Vector3& pos1,
              const Vector3& pos2) {
	TickFn tick = (TickFn)(*(void***)life)[17];
	uintptr_t vt = *(uintptr_t*)life;
	Vector3 at;
	if (vt == kAsterixLifeVtbl || vt == kObelixLifeVtbl || vt == kIdefixLifeVtbl ||
	    vt == kBonusGroupLife || !LifePos(life, isGroup, &at)) {
		tick(life, nullptr, dt);
		return;
	}
	float d1 = (at - pos1).len2(), d2 = (at - pos2).len2();
	int nearest = d2 < d1 ? p2 : p1;
	uint8_t* tr = (uint8_t*)t.raw();
	if ((d1 > kMaxRange2 && d2 > kMaxRange2) || tr[0x2C] == nearest) {
		tick(life, nullptr, dt);
		return;
	}
	uint8_t leader = tr[0x2C], companion = tr[0x2D];
	int other = nearest == p1 ? p2 : p1;
	tr[0x2C] = (uint8_t)nearest;
	tr[0x2D] = (uint8_t)other;
	InterlockedIncrement(&g_slices);
	tick(life, nullptr, dt);
	if (tr[0x2C] == (uint8_t)nearest && tr[0x2D] == (uint8_t)other) {
		tr[0x2C] = leader;
		tr[0x2D] = companion;
	} else if (!g_loggedOverride) {
		g_loggedOverride = true;   // e.g. a blue circle switched leaders: keep it
		Log("proximity: an object changed the trio's leader during its tick (now %s); kept",
			HeroName(tr[0x2C]));
	}
}

// Mirrors CKBundle::vf17 exactly, with the per-object leader around each tick.
void __fastcall BundleTickHook(char* bundle, void* edx, void* dt) {
	Trio t = GetTrio();
	int p1 = CoopP1Hero(), p2 = CoopP2Hero();
	Hero h1 = t ? t.GetHero(p1) : Hero(), h2 = t ? t.GetHero(p2) : Hero();
	if (!CoopP2Joined() || CoopSuspended() || !h1 || !h2) {
		g_origBundleTick(bundle, edx, dt);
		return;
	}
	Vector3 pos1 = h1.Pos(), pos2 = h2.Pos();

	int* groupLife = *(int**)(bundle + 0x0C);
	if (groupLife && (groupLife[1] & 3) == 0)
		TickLife(groupLife, dt, true, t, p1, p2, pos1, pos2);
	for (int* life = *(int**)(bundle + 0x10); life; life = (int*)life[2]) {
		if ((*(uint8_t*)(life + 1) & 2) == 0)
			TickLife(life, dt, false, t, p1, p2, pos1, pos2);
	}
}

void AttractToPlayer2(char* pool) {
	Trio t = GetTrio();
	Hero h2 = t ? t.GetHero(CoopP2Hero()) : Hero();
	if (!h2 || !CoopP2Joined() || CoopSuspended()) return;
	typedef char(__fastcall* CanFn)(void* pool, void* edx, void* hero);
	typedef char(__fastcall* BoolFn)(void* self, void* edx);
	typedef void(__fastcall* AttractFn)(void* self, void* edx, void* hero, int zero);
	if (!((CanFn)(*(void***)pool)[39])(pool, nullptr, h2.hook)) return;
	char* radiusObj = *(char**)(pool + 0x3C);
	if (!radiusObj) return;
	float r2 = *(float*)(radiusObj + 4);
	Vector3 p2 = h2.Pos();
	for (char* b = *(char**)(pool + 0x30); b; b = *(char**)(b + 0x14)) {
		if (((BoolFn)(*(void***)b)[19])(b, nullptr)) continue;   // already following someone
		Vector3 at;
		if (!NodePos(*(void**)(b + 0x10), &at)) continue;
		if ((at - p2).len2() < r2) {
			((AttractFn)(*(void***)b)[18])(b, nullptr, h2.hook, 0);
			InterlockedIncrement(&g_p2Bonuses);
		}
	}
}

void __fastcall PoolUpdateHook(void* self, void* edx, void* dt) {
	g_origPoolUpdate(self, edx, dt);
	AttractToPlayer2((char*)self);
}

void __fastcall AsterixPoolUpdateHook(void* self, void* edx, void* dt) {
	g_origAsterixPoolUpdate(self, edx, dt);
	AttractToPlayer2((char*)self);
}

} // namespace

long ProximitySlices() { return g_slices; }
long ProximityP2Bonuses() { return g_p2Bonuses; }

bool ProximityInstallHooks() {
	bool ok = PatchVtableSlot(kBundleVtbl, kBundleTickSlot, kBundleTick,
	                          (void*)BundleTickHook, (void**)&g_origBundleTick, "CKBundle::tick");
	ok &= PatchVtableSlot(kBonusPoolVtbl, kPoolUpdateSlot, kBonusPoolUpdate,
	                      (void*)PoolUpdateHook, (void**)&g_origPoolUpdate, "CKGrpBonusPool::update");
	ok &= PatchVtableSlot(kAsterixBonusPoolVtbl, kPoolUpdateSlot, kAsterixBonusPoolUpdate,
	                      (void*)AsterixPoolUpdateHook, (void**)&g_origAsterixPoolUpdate,
	                      "CKGrpAsterixBonusPool::update");
	return ok;
}
