#include "nolock.h"
#include "coop.h"
#include "config.h"
#include "kal.h"
#include "klusters.h"
#include "log.h"
#include "pathfind.h"
#include "proximity.h"
#include "trio.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

// No-lock mode's Romans ([coop_difficulty.experimental] nolock_romans): while
// the level would have kept a player's hero waiting -- a scripted order no-lock
// mode leaves unfollowed (CoopNoLockWaiting) -- Romans come for it now and
// then, so the free player has something to do until the other lifts the wait.
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

// Seen by a player as the squads' markers are (proximity.cpp): a wall between
// counts as out of view.
bool InAView(const Vector3& p) {
	float c[3] = { p.x, p.y, p.z };
	return MarkerSeen(c);
}

// Where a Roman of squad `sender` comes in for `h`, as squads bring theirs: a
// spawn marker the level placed (+0x5C, 8 bytes each, +0 -> position; count
// +0x64) -- the sender's own; any squad's with nolock_romans_any_markers (the
// user saw Romans stuck in the air: a marker placed for other Romans) -- within
// 40 units, at least 6 from both heroes, in no player's view (the co-op rule for
// squad markers) and one the hero can walk to (the level's pathfinding graph:
// not behind a shut door) -- the nearest; else the hero's walk-to marker
// (below). None, no Roman: places picked off the graph's cells were unreliable
// (its walkable cells take in shallow water -- LV2-3's Romans there drowned).
bool SpawnPoint(Trio t, Hero h, char* sender, Vector3* at, const char** how) {
	Hero ha = t.GetHero(HERO_ASTERIX), ho = t.GetHero(HERO_OBELIX);
	const float kReach = 40.0f;
	float bestD = kReach * kReach;
	bool have = false;
	int total = 0, others = 0, nearHero = 0, seen = 0, unreachable = 0;
	for (unsigned i = 0, n = GetNumLvlClassInstances(4, 26); i < n; i++) {
		char* sq = (char*)GetLvlObject(4, 26, i);
		char* list = sq ? *(char**)(sq + 0x5C) : nullptr;
		unsigned nm = list ? *(unsigned*)(sq + 0x64) : 0;
		for (unsigned k = 0; k < nm && k < 64; k++) {
			const float* p = *(const float**)(list + k * 8);
			if (!p) continue;
			Vector3 pv(p[0], p[1], p[2]);
			float d = (pv - h.Pos()).len2();
			if (d >= kReach * kReach) continue;
			total++;
			if (sq != sender && !g_config.noLockRomansAnyMarkers) { others++; continue; }
			if ((ha && (ha.Pos() - pv).len2() < 36.0f) || (ho && (ho.Pos() - pv).len2() < 36.0f)) { nearHero++; continue; }
			if (InAView(pv)) { seen++; continue; }
			if (PfReachable(h.Pos(), pv) != PF_YES) { unreachable++; continue; }
			if (d >= bestD) continue;
			bestD = d;
			*at = pv;
			*how = sq == sender ? "its squad's spawn marker" : "another squad's spawn marker";
			have = true;
		}
	}
	if (have) return true;
	// Else the marker the level would have walked the hero to (its AI target,
	// +0x568 -- LV2-3's hearth: no squad marker within 40 units, Obelix's marker by
	// the doors): placed by the level and on the hero's side by design. Only out of
	// view (else the next try).
	const Vector3& tg = h.AiTargetPos();
	float dt = (tg - h.Pos()).len2();
	bool byHero = (ha && (ha.Pos() - tg).len2() < 36.0f) || (ho && (ho.Pos() - tg).len2() < 36.0f);
	if (dt < kReach * kReach && !byHero && !InAView(tg)) {
		*at = tg;
		*how = "its walk-to marker";
		return true;
	}
	static int s_told = 0;
	if (s_told++ < 5)
		Log("nolock: spawn markers within %.0f units of %s: %d -- %d other squads', %d by a hero, %d in view, %d not walkable "
		    "to; its walk-to marker %.0f units off%s%s", kReach,
		    HeroName(t.GetHero(HERO_ASTERIX).raw() == h.raw() ? HERO_ASTERIX : HERO_OBELIX), total, others, nearHero, seen,
		    unreachable, sqrtf(dt), byHero ? ", by a hero" : "", InAView(tg) ? ", in view" : "");
	return false;
}

// Without nolock_romans_any_markers: a Roman of an active squad with Romans
// left to send at one of that squad's own spawn markers (as SpawnPoint takes
// them) -- the nearest to `h` of all squads'. LV3-6: the nearest squad's own
// markers are up on the walls (not walkable to), the fight's squad's three on
// the ground.
char* SquadAtOwnMarker(Trio t, Hero h, int* pool, Vector3* at) {
	Hero ha = t.GetHero(HERO_ASTERIX), ho = t.GetHero(HERO_OBELIX);
	float bestD = 40.0f * 40.0f;
	char* best = nullptr;
	for (unsigned i = 0, n = GetNumLvlClassInstances(4, 26); i < n; i++) {
		char* sq = (char*)GetLvlObject(4, 26, i);
		if (!sq || !(*(uint8_t*)(sq + 0x152) & 2) || (*(uint16_t*)(sq + 0x152) & 0x2000)) continue;
		char* recs = *(char**)(sq + 0x144);
		unsigned nr = *(unsigned*)(sq + 0x14C);
		char* list = *(char**)(sq + 0x5C);
		unsigned nm = list ? *(unsigned*)(sq + 0x64) : 0;
		if (!recs || !nr || !nm) continue;
		int p = -1;
		for (unsigned k = 0; k < nr && k < 8 && p < 0; k++)
			if (PoolHasRomans(recs + 0x1C * k)) p = (int)k;
		if (p < 0) continue;
		for (unsigned k = 0; k < nm && k < 64; k++) {
			const float* m = *(const float**)(list + k * 8);
			if (!m) continue;
			Vector3 pv(m[0], m[1], m[2]);
			float d = (pv - h.Pos()).len2();
			if (d >= bestD || (ha && (ha.Pos() - pv).len2() < 36.0f) || (ho && (ho.Pos() - pv).len2() < 36.0f)) continue;
			if (InAView(pv) || PfReachable(h.Pos(), pv) != PF_YES) continue;
			bestD = d;
			best = sq;
			*pool = p;
			*at = pv;
		}
	}
	return best;
}

void SpawnFor(int hero, Trio t, Hero h) {
	Waiting& w = g_wait[hero];
	int pool = -1;
	Vector3 at;
	const char* how = "its squad's spawn marker";
	char* sq = g_config.noLockRomansAnyMarkers ? nullptr : SquadAtOwnMarker(t, h, &pool, &at);
	if (!sq) {
		sq = SquadFor(h.Pos(), &pool);
		if (!sq) {
			if (w.noneTold++ < 3) Log("nolock: no active squad with Romans to send for %s", HeroName(hero));
			return;
		}
		if (!SpawnPoint(t, h, sq, &at, &how)) {
			if (w.noneTold++ < 3) Log("nolock: no spawn marker near %s (unseen, walkable to) for a Roman", HeroName(hero));
			return;
		}
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
	Log("nolock: a Roman of squad %p (pool %d) sent at %s from %s (%.1f %.1f %.1f): alive %d -> %d [%d/%d]", sq, pool,
	    HeroName(hero), how, at.x, at.y, at.z, alive0, alive1, w.sent, kMaxPerWait);
}

// Diagnostics, once a wait: where the level would have sent the hero (its AI
// target, hero +0x568) and the squads within 60 units (box centre, active,
// pools: alive / sent / to defeat, spawn markers).
void DumpAround(int hero, Hero h) {
	const Vector3& tg = h.AiTargetPos();
	Log("nolock: %s at (%.1f %.1f %.1f), order %d to (%.1f %.1f %.1f)", HeroName(hero), h.Pos().x, h.Pos().y, h.Pos().z,
	    (int)h.Order(), tg.x, tg.y, tg.z);
	for (unsigned i = 0, n = GetNumLvlClassInstances(4, 26); i < n; i++) {
		char* sq = (char*)GetLvlObject(4, 26, i);
		if (!sq) continue;
		const float* f = (const float*)sq;
		Vector3 c((f[0x47] + f[0x4A]) * 0.5f, (f[0x48] + f[0x4B]) * 0.5f, (f[0x49] + f[0x4C]) * 0.5f);
		if ((c - h.Pos()).len2() > 60.0f * 60.0f) continue;
		char buf[400];
		int k = sprintf(buf, "nolock:   squad #%u %p box centre (%.1f %.1f %.1f) size %.0fx%.0f active %d markers %u pools:", i, sq,
		                c.x, c.y, c.z, f[0x47] - f[0x4A], f[0x49] - f[0x4C], (*(uint8_t*)(sq + 0x152) >> 1) & 1,
		                *(char**)(sq + 0x5C) ? *(unsigned*)(sq + 0x64) : 0);
		char* recs = *(char**)(sq + 0x144);
		for (unsigned p = 0, np = *(unsigned*)(sq + 0x14C); recs && p < np && p < 4; p++) {
			const char* r = recs + 0x1C * p;
			k += sprintf(buf + k, " [%u: alive %d sent %d of %d]", p, *(uint8_t*)(r + 0x11), *(uint16_t*)(r + 0x16), *(int16_t*)(r + 0x18));
		}
		Log("%s", buf);
	}
}

} // namespace

void NoLockRomansFrame() {
	if (!g_config.noLock || !g_config.noLockRomans || !CoopP2Joined() || CoopSuspended()) return;
	Trio t = GetTrio();
	if (!t) return;
	DWORD now = GetTickCount();
	for (int hero = HERO_ASTERIX; hero <= HERO_OBELIX; hero++) {
		Hero h = t.GetHero(hero);
		Waiting& w = g_wait[hero];
		bool waiting = h && CoopNoLockWaiting(hero);
		if (waiting != w.on) {
			Log("nolock: %s %s", HeroName(hero),
			    waiting ? "would be kept waiting by the level -- Romans will come for it"
			            : "is no longer kept waiting -- no more Romans for it");
			w = Waiting();
			w.on = waiting;
			w.next = now + 2000;
			if (waiting) DumpAround(hero, h);
		}
		if (!waiting || w.sent >= kMaxPerWait || (int)(now - w.next) < 0) continue;
		w.next = now + kSpawnEveryMs;
		SpawnFor(hero, t, h);
	}
}
