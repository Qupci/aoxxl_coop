#include "freedom.h"
#include "coop.h"
#include "config.h"
#include "kal.h"
#include "klusters.h"
#include "log.h"
#include "pathfind.h"
#include "trio.h"

#include <cmath>
#include <cstdlib>

// Freedom mode's Romans ([experimental] freedom_romans): while the level would
// have kept a player's hero waiting -- a scripted order freedom mode leaves
// unfollowed (CoopFreedomWaiting) -- Romans come for it now and then, so the
// free player has something to do until the other lifts the wait.
//
// Romans come from a squad's pools: vf55 (CKGrpSquadEnemy, the squad script's
// "send in at a point", thiscall(args) -- args { count, Vector3* point, pool or
// -1 }) takes them from a pool's free list and places them around the point
// (+-0.5 unit), counted alive (+0x11) -- only for an active squad (+0x152 bit 1,
// its AI running) and a pool with Romans left to send in (to defeat +0x18 < 0, or
// more than sent +0x16 + fallen +0x15 + queued +0x14). Squads (group class 4,26)
// box: max +0x11C..+0x124, min +0x128..+0x130; pools: +0x144, 0x1C each, count
// +0x14C.
namespace {

const DWORD kSpawnEveryMs = 6000;
const int   kMaxPerWait   = 6;

struct Waiting {
	bool  on = false;
	DWORD next = 0;
	int   sent = 0;
	int   noneTold = 0;
} g_wait[2];

bool PoolHasRomans(const char* rec) {
	int16_t toDefeat = *(const int16_t*)(rec + 0x18);
	if (toDefeat < 0) return true;
	int left = toDefeat - *(const uint16_t*)(rec + 0x16) - *(const uint8_t*)(rec + 0x15) - *(const uint8_t*)(rec + 0x14);
	return left > 0;
}

// The active squad nearest `at` (its pool in `pool`): one with Romans left to
// send first; else any (its pool 0 -- given one more to defeat, see SpawnFor).
char* SquadFor(const Vector3& at, int* pool) {
	char* best = nullptr;
	float bestD = 1e30f;
	for (unsigned i = 0, n = GetNumLvlClassInstances(4, 26); i < n; i++) {
		char* sq = (char*)GetLvlObject(4, 26, i);
		if (!sq || !(*(uint8_t*)(sq + 0x152) & 2) || (*(uint16_t*)(sq + 0x152) & 0x2000)) continue;
		char* recs = *(char**)(sq + 0x144);
		unsigned nr = *(unsigned*)(sq + 0x14C);
		if (!recs || !nr) continue;
		int p = -1;
		for (unsigned k = 0; k < nr && k < 8 && p < 0; k++)
			if (PoolHasRomans(recs + 0x1C * k)) p = (int)k;
		float penalty = p < 0 ? 1e6f : 0.0f;   // spent squads only when no other is active
		if (p < 0) p = 0;
		const float* f = (const float*)sq;
		Vector3 c((f[0x47] + f[0x4A]) * 0.5f, (f[0x48] + f[0x4B]) * 0.5f, (f[0x49] + f[0x4C]) * 0.5f);
		float d = (c - at).len2() + penalty;
		if (d < bestD) { bestD = d; best = sq; *pool = p; }
	}
	return best;
}

// The ground under `p`, as the game drops a squad's reinforcement point (FUN_00530060):
// FUN_004493C0 on the collision world ([[0x6621F4] + 0x50]), thiscall(params
// { radius 0.3, height 2, 0 }, result, point, 1) ret 0x10 -- result +0x14 the hit's
// kind (4 nothing), +0x18 the point on the ground.
bool GroundUnder(const Vector3& p, float* y) {
	struct { float radius, height; uint32_t flags; } params = { 0.3f, 2.0f, 0 };
	uint8_t out[0x40] = {};
	*(int*)(out + 0x14) = 4;
	float pt[3] = { p.x, p.y, p.z };
	typedef void(__fastcall* DropFn)(char* self, void* edx, void* params, void* out, float* pt, int n);
	__try {
		char* g = *(char**)0x6621F4;
		char* world = g ? *(char**)(g + 0x50) : nullptr;
		if (!world) return false;
		((DropFn)0x4493C0)(world, nullptr, &params, out, pt, 1);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
	if (*(int*)(out + 0x14) == 4) return false;
	*y = *(float*)(out + 0x1C);
	return true;
}

bool InAView(const Vector3& p) {
	float c[3] = { p.x, p.y, p.z };
	return KlusterSphereInAnyView(c, 1.5f, 40.0f);
}

// Where a Roman comes in for `h`, as squads bring theirs: a spawn marker (any
// squad's: +0x5C, 8 bytes each, +0 -> position; count +0x64) within 30 units the
// hero can walk to, at least 6 from both heroes and in no player's view (the
// co-op rule for squad markers) -- the nearest. Else a place the hero can walk
// to (the level's pathfinding graph: not behind a shut door) with ground at its
// height under it (the graph's walkable cells take in shallow water: LV2-3's
// Romans there drowned), out of view if one is.
bool SpawnPoint(Trio t, Hero h, Vector3* at, const char** how) {
	Hero ha = t.GetHero(HERO_ASTERIX), ho = t.GetHero(HERO_OBELIX);
	float bestD = 30.0f * 30.0f;
	bool have = false;
	for (unsigned i = 0, n = GetNumLvlClassInstances(4, 26); i < n; i++) {
		char* sq = (char*)GetLvlObject(4, 26, i);
		char* list = sq ? *(char**)(sq + 0x5C) : nullptr;
		unsigned nm = list ? *(unsigned*)(sq + 0x64) : 0;
		for (unsigned k = 0; k < nm && k < 64; k++) {
			const float* p = *(const float**)(list + k * 8);
			if (!p) continue;
			Vector3 pv(p[0], p[1], p[2]);
			float d = (pv - h.Pos()).len2();
			if (d >= bestD || (ha && (ha.Pos() - pv).len2() < 36.0f) || (ho && (ho.Pos() - pv).len2() < 36.0f)) continue;
			if (InAView(pv) || PfReachable(h.Pos(), pv) != PF_YES) continue;
			bestD = d;
			*at = pv;
			have = true;
		}
	}
	if (have) { *how = "a spawn marker"; return true; }
	bool found = false, hidden = false;
	for (int tries = 0; tries < 30 && !hidden; tries++) {
		Vector3 c;
		if (!PfSpotNear(h.Pos(), 3.0f, 10.0f, &c)) break;
		float gy;
		if (!GroundUnder(Vector3(c.x, h.Pos().y + 1.5f, c.z), &gy) || fabsf(gy - h.Pos().y) >= 1.0f) continue;
		c.y = gy + 0.5f;
		bool unseen = !InAView(c);
		if (!found || unseen) { *at = c; found = true; hidden = unseen; }
	}
	*how = hidden ? "walkable ground out of view" : "walkable ground (in view)";
	return found;
}

void SpawnFor(int hero, Trio t, Hero h) {
	Waiting& w = g_wait[hero];
	int pool = -1;
	char* sq = SquadFor(h.Pos(), &pool);
	if (!sq) {
		if (w.noneTold++ < 3) Log("freedom: no active squad with Romans to send for %s", HeroName(hero));
		return;
	}
	Vector3 at;
	const char* how = "";
	if (!SpawnPoint(t, h, &at, &how)) {
		if (w.noneTold++ < 3) Log("freedom: no place near %s for a Roman", HeroName(hero));
		return;
	}
	uint32_t args[3] = { 1, (uint32_t)(uintptr_t)&at, (uint32_t)pool };
	char* rec = *(char**)(sq + 0x144) + 0x1C * pool;
	// A spent pool gets one more to defeat (+0x18): a gate counting the squad
	// (count - sent + alive) stays even -- one more sent, one more to beat.
	if (!PoolHasRomans(rec)) *(int16_t*)(rec + 0x18) += 1;
	char alive0 = *(char*)(rec + 0x11);
	typedef uint32_t(__fastcall* CmdFn)(char* self, void* edx, uint32_t* args);
	((CmdFn)(*(void***)sq)[55])(sq, nullptr, args);
	char alive1 = *(char*)(rec + 0x11);
	w.sent++;
	Log("freedom: a Roman of squad %p (pool %d) sent at %s from %s (%.1f %.1f %.1f): alive %d -> %d [%d/%d]", sq, pool,
	    HeroName(hero), how, at.x, at.y, at.z, alive0, alive1, w.sent, kMaxPerWait);
}

} // namespace

void FreedomFrame() {
	if (!g_config.freedom || !g_config.freedomRomans || !CoopP2Joined() || CoopSuspended()) return;
	Trio t = GetTrio();
	if (!t) return;
	DWORD now = GetTickCount();
	for (int hero = HERO_ASTERIX; hero <= HERO_OBELIX; hero++) {
		Hero h = t.GetHero(hero);
		Waiting& w = g_wait[hero];
		bool waiting = h && CoopFreedomWaiting(hero);
		if (waiting != w.on) {
			Log("freedom: %s %s", HeroName(hero),
			    waiting ? "would be kept waiting by the level -- Romans will come for it"
			            : "is no longer kept waiting -- no more Romans for it");
			w = Waiting();
			w.on = waiting;
			w.next = now + 2000;
		}
		if (!waiting || w.sent >= kMaxPerWait || (int)(now - w.next) < 0) continue;
		w.next = now + kSpawnEveryMs;
		SpawnFor(hero, t, h);
	}
}
