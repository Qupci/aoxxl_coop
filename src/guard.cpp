#include "guard.h"
#include "coop.h"
#include "individual.h"
#include "kal.h"
#include "log.h"
#include "nolock.h"
#include "trio.h"

#include <cmath>

// Romans guard a hero who is down (individual heroes): the Romans after it as
// it fell -- members of the active squads within 10 units -- stay round it until
// it is revived or they are beaten, so the partner fights them to get to it.
// The lead goes to the standing hero and the squads run for that one: their
// slots (vf31 -> FUN_0052B670: component 0x46, +0 slot, +0x18 the step towards
// it) would take those Romans over to it. After each squad's tick their slots
// are moved round the downed hero, 3 units off, spread out (as SplitSquad's and
// no-lock mode's), and their squads are not put away meanwhile (vf35). They
// leave the downed hero alone (out of the objects' slicing: HeroOut) and go
// for the one who comes.
namespace {

const int   kMaxGuards  = 16;
const float kGuardReach = 10.0f;

// A guard, in its pool record's list (+8, chained +0x50) and not finished
// (hook +8 bit 0) while it lasts; the squad checked to be the level's still.
struct Guard { char* roman; char* sq; unsigned sqIdx; int pool; int hero; };
Guard g_guards[kMaxGuards];
int   g_n = 0;
bool  g_down[2] = {};
int   g_told = 0;

bool Valid(const Guard& g) {
	if (!KalLevelIsLoaded() || g.sqIdx >= GetNumLvlClassInstances(4, 26) || (char*)GetLvlObject(4, 26, g.sqIdx) != g.sq)
		return false;
	char* recs = *(char**)(g.sq + 0x144);
	if (!recs || g.pool >= (int)*(unsigned*)(g.sq + 0x14C)) return false;
	int guard = 0;
	for (char* m = *(char**)(recs + 0x1C * g.pool + 8); m && guard++ < 256; m = *(char**)(m + 0x50))
		if (m == g.roman) return !(*(uint8_t*)(m + 8) & 1);
	return false;
}

// Its hero still down, in co-op.
bool OnDuty(const Guard& g) {
	return CoopP2Joined() && !CoopSuspended() && IndivFallen(g.hero) && Valid(g);
}

bool RomanPos(char* m, Vector3* out) {
	char* node = *(char**)(m + 0x10);
	char* fr = node ? *(char**)(node + 4) : nullptr;
	if (!fr) return false;
	const float* p = (const float*)(fr + 0x80);
	*out = Vector3(p[0], p[1], p[2]);
	return true;
}

void Collect(int hero, const Vector3& at) {
	int n0 = g_n;
	for (unsigned i = 0, n = GetNumLvlClassInstances(4, 26); i < n && g_n < kMaxGuards; i++) {
		char* sq = (char*)GetLvlObject(4, 26, i);
		if (!sq || !(*(uint8_t*)(sq + 0x152) & 2)) continue;
		char* recs = *(char**)(sq + 0x144);
		unsigned nr = *(unsigned*)(sq + 0x14C);
		for (unsigned r = 0; recs && r < nr && r < 8; r++)
			for (char* m = *(char**)(recs + 0x1C * r + 8); m && g_n < kMaxGuards; m = *(char**)(m + 0x50)) {
				Vector3 p;
				if ((*(uint8_t*)(m + 8) & 1) || !RomanPos(m, &p) || (p - at).len2() > kGuardReach * kGuardReach) continue;
				bool have = false;
				for (int k = 0; k < g_n; k++) have |= g_guards[k].roman == m;
				if (!have) g_guards[g_n++] = { m, sq, i, (int)r, hero };
			}
	}
	if (g_told++ < 40)
		Log("guard: %s is down -- %d Roman(s) within %.0f units stay round it until it is revived", HeroName(hero), g_n - n0,
		    kGuardReach);
}

void Release(int hero) {
	int n = 0;
	for (int i = 0; i < g_n;) {
		if (g_guards[i].hero != hero) { i++; continue; }
		g_guards[i] = g_guards[--g_n];
		n++;
	}
	if (n && g_told++ < 40) Log("guard: %s is no longer down -- its %d guard(s) go back to their squads", HeroName(hero), n);
}

typedef void(__fastcall* SquadAwayFn)(char* sq, void* edx);
const uintptr_t kSquadEnemyVtbl = 0x6266B0, kSquadAway = 0x52F6D0;
SquadAwayFn g_origSquadAway = nullptr;
volatile long g_awayKept = 0;

// vf35: an active squad nobody is near is put away (vf34, its Romans back to
// the pool) once its box is out of the game camera's view -- guards and Romans
// out for a waiting hero would vanish, by the hero they are with.
void __fastcall SquadAwayHook(char* sq, void* edx) {
	if (GuardKeepsSquad(sq) || NoLockKeepsSquad(sq)) {
		if (InterlockedIncrement(&g_awayKept) % 600 == 1)
			Log("guard: squad %p not put away -- Romans of it are by a downed or waiting hero", sq);
		return;
	}
	g_origSquadAway(sq, edx);
}

} // namespace

bool GuardInstallHooks() {
	return PatchVtableSlot(kSquadEnemyVtbl, 35, kSquadAway, (void*)SquadAwayHook, (void**)&g_origSquadAway,
	                       "squads put away out of view (not with Romans by a downed or waiting hero)");
}

void GuardFrame() {
	for (int i = 0; i < g_n;) {   // beaten or put away
		if (Valid(g_guards[i])) i++;
		else g_guards[i] = g_guards[--g_n];
	}
	Trio t = GetTrio();
	bool on = t && CoopP2Joined() && !CoopSuspended() && KalLevelIsLoaded();
	for (int hero = HERO_ASTERIX; hero <= HERO_OBELIX; hero++) {
		Hero h = on ? t.GetHero(hero) : Hero();
		bool down = h && IndivFallen(hero);
		if (down && !g_down[hero]) Collect(hero, h.Pos());
		else if (!down && g_down[hero]) Release(hero);
		g_down[hero] = down;
	}
}

void GuardSteer(char* sq) {
	if (!g_n) return;
	Trio t = GetTrio();
	if (!t) return;
	typedef char*(__fastcall* CpntFn)(char* self, void* edx, int id);
	float minStep = *(float*)(sq + 0xC0);
	int n[2] = {}, k[2] = {};
	for (int i = 0; i < g_n; i++) n[g_guards[i].hero]++;   // one ring per hero, over all squads
	for (int i = 0; i < g_n; i++) {
		const Guard& g = g_guards[i];
		int place = k[g.hero]++;
		if (g.sq != sq || !OnDuty(g)) continue;
		Hero h = t.GetHero(g.hero);
		char* node = *(char**)(g.roman + 0x10);
		char* fr = node ? *(char**)(node + 4) : nullptr;
		float* slot = (float*)((CpntFn)(*(void***)g.roman)[12])(g.roman, nullptr, 0x46);
		if (!h || !fr || !slot) continue;
		const float* rp = (const float*)(fr + 0x80);
		float a = 6.2831853f * place / n[g.hero] + 0.7f;
		Vector3 hp = h.Pos();
		float sx = hp.x + 3.0f * cosf(a), sz = hp.z + 3.0f * sinf(a);
		slot[0] = sx; slot[1] = hp.y; slot[2] = sz;
		float dx = sx - rp[0], dz = sz - rp[2], len = sqrtf(dx * dx + dz * dz);
		float f = len <= 0.01f ? 0.0f : 1.0f / (len > minStep ? len : minStep);
		slot[6] = dx * f; slot[7] = 0.0f; slot[8] = dz * f;
	}
}

bool GuardKeepsSquad(char* sq) {
	for (int i = 0; i < g_n; i++)
		if (g_guards[i].sq == sq && OnDuty(g_guards[i])) return true;
	return false;
}
