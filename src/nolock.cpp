#include "nolock.h"
#include "coop.h"
#include "config.h"
#include "experimental.h"
#include "individual.h"
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
// mode leaves unfollowed (CoopNoLockWaiting): the other hero in a locked fight
// (LV3-4: Obelix shut in the library while Asterix fights in the courtyard) or
// held by a sequence -- Romans come for it in groups of 1 to 6, the next group
// 20 to 90 s (mostly about 30) after the last of a group is gone, so the free
// player has something to do until the other lifts the wait. They go after
// that hero wherever it goes and start no fight music. Never for the leader:
// the one in the locked fight (the level's scripts follow it).
//
// Romans come from a squad's pools: vf55 (CKGrpSquadEnemy, the squad script's
// "send in at a point", thiscall(args) -- args { count, Vector3* point, pool or
// -1 }) takes them from a pool's free list and places them around the point
// (+-0.5 unit), counted alive (+0x11) -- only for an active squad (+0x152 bit 1,
// its AI running) and a pool with Romans left to send in (to defeat +0x18 < 0, or
// more than sent +0x16 + fallen +0x15 + queued +0x14); with the free list empty
// it queues them (+0x14). Squads (group class 4,26) box: max +0x11C..+0x124, min
// +0x128..+0x130; pools: +0x144, 0x1C each, count +0x14C. A pool's free Romans
// are its CKGrpPoolSquad's members (vf12 -> head, hooks chained +4). A Roman sent
// in joins its pool record's list (+8, chained +0x50; FUN_00530380) and the
// squad's members (vf12, chained +4), and leaves both once beaten (hook +8 bit 0:
// FUN_00530570 takes it off) or put away (vf34).
namespace {

const int   kMaxGroup = 6;
const DWORD kFirstMs  = 2000;   // the first group, after the wait begins
const DWORD kRetryMs  = 6000;   // no Roman could come: the next try

// The game's rand() is its own; this one is seeded from the clock.
unsigned Random(unsigned n) {
	static uint32_t s = GetTickCount() * 2654435761u;
	s = s * 1664525u + 1013904223u;
	return n ? (s >> 8) % n : 0;
}

// The pause before the next group: 20 + 70 u^3 s -- 20 to 90, half under 29.
DWORD PauseMs() {
	float u = Random(10001) / 10000.0f;
	return 20000 + (DWORD)(70000.0f * u * u * u);
}

struct Waiting {
	bool  on = false;
	DWORD next = 0;      // the next group, once none of the last is left
	int   groups = 0;
	int   noneTold = 0;
} g_wait[2];

// A Roman sent for a hero, until beaten or put away: in its pool record's list
// and not finished (as the squad sees it). The squad is checked to be the
// level's still (a level change frees it) before anything is read.
struct Out { char* roman; char* sq; unsigned sqIdx; int pool; int hero; };
Out g_out[2 * kMaxGroup];
int g_nOut = 0;
int g_groupLogs = 0;

bool SquadValid(char* sq, unsigned idx) {
	return KalLevelIsLoaded() && idx < GetNumLvlClassInstances(4, 26) && (char*)GetLvlObject(4, 26, idx) == sq;
}

bool StillOut(const Out& o) {
	if (!SquadValid(o.sq, o.sqIdx)) return false;
	char* recs = *(char**)(o.sq + 0x144);
	if (!recs || o.pool >= (int)*(unsigned*)(o.sq + 0x14C)) return false;
	int guard = 0;
	for (char* m = *(char**)(recs + 0x1C * o.pool + 8); m && guard++ < 256; m = *(char**)(m + 0x50))
		if (m == o.roman) return !(*(uint8_t*)(m + 8) & 1);
	return false;
}

bool IsOurs(const char* m) {
	for (int i = 0; i < g_nOut; i++) if (g_out[i].roman == m) return true;
	return false;
}

int OutFor(int hero) {
	int n = 0;
	for (int i = 0; i < g_nOut; i++) if (g_out[i].hero == hero) n++;
	return n;
}

// Squads whose end (event 0D16) has come with Romans of theirs out for a hero:
// it would come again as the last of those goes (FUN_00530570: every member
// beaten, nothing left to send) -- dropped (SquadEventHook) until 2 s after.
// `woke`: asleep, woken for them (a revenge squad, SendGroup): put away
// then (vf34: flag off, members back to their pools; no events).
struct Ended { char* sq; unsigned idx; DWORD clearAt; bool woke; };
Ended g_ended[4];
int g_endedTold = 0;

Ended* FindEnded(const char* sq) {
	for (Ended& e : g_ended) if (e.sq == sq) return &e;
	return nullptr;
}

void MarkEnded(char* sq, unsigned idx, bool woke) {
	if (Ended* e = FindEnded(sq)) { e->woke |= woke; return; }
	Ended* slot = &g_ended[0];
	for (Ended& e : g_ended) if (!e.sq) { slot = &e; break; }
	*slot = { sq, idx, 0, woke };
}

typedef uint32_t(__fastcall* SquadEventFn)(char* sq, void* edx, unsigned ev, void* arg);
const uintptr_t kSquadEnemyVtbl = 0x6266B0, kSquadEnemyEvent = 0x4C7E70;
SquadEventFn g_origSquadEvent = nullptr;

uint32_t __fastcall SquadEventHook(char* sq, void* edx, unsigned ev, void* arg) {
	if ((ev & 0xFFFF) == 0xD16 && FindEnded(sq)) {
		if (g_endedTold++ < 20) Log("nolock: squad %p's end (0D16) again as its Romans out for a hero go -- dropped", sq);
		return 0;
	}
	return g_origSquadEvent(sq, edx, ev, arg);
}

// Nothing left to send in or to replace (FUN_00530570's test).
bool SquadSpent(const char* sq) {
	const char* recs = *(char* const*)(sq + 0x144);
	unsigned nr = *(const unsigned*)(sq + 0x14C);
	for (unsigned r = 0; recs && r < nr && r < 16; r++) {
		const char* rec = recs + r * 0x1C;
		int16_t toDefeat = *(const int16_t*)(rec + 0x18);
		if (toDefeat < 0 ? rec[0x14] || rec[0x15] : *(const uint16_t*)(rec + 0x16) < (uint16_t)toDefeat) return false;
	}
	return true;
}

// Members of the squad not beaten, the Romans out for a hero not counted.
int OthersAlive(char* sq) {
	typedef char** (__fastcall* ListFn)(char* self, void* edx);
	char** head = ((ListFn)(*(void***)sq)[12])(sq, nullptr);
	int n = 0, guard = 0;
	for (char* m = head ? *head : nullptr; m && guard++ < 256; m = *(char**)(m + 4))
		if (!(*(uint8_t*)(m + 8) & 1) && !IsOurs(m)) n++;
	return n;
}

// A squad a fight's gate counts (+0x154 >= -1) with nothing left but Romans out
// for the waiting hero would keep the gate shut until that hero beats them. The
// fight is the other hero's: ended as the game ends it (0D16, +0x154 = -2); the
// waiting hero's Romans stay with it.
void EndFightsLeftToOurs() {
	for (int i = 0; i < g_nOut; i++) {
		char* sq = g_out[i].sq;
		if (*(int8_t*)(sq + 0x154) < -1 || FindEnded(sq) || !SquadSpent(sq) || OthersAlive(sq)) continue;
		int n = 0;
		for (int j = 0; j < g_nOut; j++) if (g_out[j].sq == sq) n++;
		Log("nolock: squad %p's fight is won but for %d Roman(s) out for %s -- the fight ends (0D16), they stay", sq, n,
		    HeroName(g_out[i].hero));
		typedef uint32_t(__fastcall* SendFn)(char* self, void* edx, int ev, int arg);
		((SendFn)(*(void***)sq)[5])(sq, nullptr, 0xD16, 0);
		*(uint8_t*)(sq + 0x154) = 0xFE;
		MarkEnded(sq, g_out[i].sqIdx, false);
	}
}

// Romans gone: a hero waiting with none of its group left gets the next group
// after a pause. Ended squads forgotten 2 s after their last Roman out.
void Prune(DWORD now) {
	for (int i = 0; i < g_nOut;) {
		if (StillOut(g_out[i])) { i++; continue; }
		int hero = g_out[i].hero;
		g_out[i] = g_out[--g_nOut];
		Waiting& w = g_wait[hero];
		if (!OutFor(hero) && w.on) {
			DWORD pause = PauseMs();
			w.next = now + pause;
			if (g_groupLogs++ < 200)
				Log("nolock: none left of the Romans sent at %s -- the next ones in %.0f s", HeroName(hero), pause / 1000.0f);
		}
	}
	for (Ended& e : g_ended) {
		if (!e.sq) continue;
		bool out = false;
		for (int i = 0; i < g_nOut; i++) if (g_out[i].sq == e.sq) out = true;
		if (out && SquadValid(e.sq, e.idx)) e.clearAt = 0;
		else if (!SquadValid(e.sq, e.idx)) e = Ended();
		else if (!e.clearAt) e.clearAt = now + 2000;
		else if ((int)(now - e.clearAt) >= 0) {
			if (e.woke && (*(uint8_t*)(e.sq + 0x152) & 2)) {   // a revenge squad: asleep again
				typedef void(__fastcall* AwayFn)(char* self, void* edx);
				((AwayFn)(*(void***)e.sq)[34])(e.sq, nullptr);
				if (g_groupLogs++ < 200) Log("nolock: squad %p's revenge is over -- it is put away again", e.sq);
			}
			e = Ended();
		}
	}
}

bool PoolHasRomans(const char* rec) {
	int16_t toDefeat = *(const int16_t*)(rec + 0x18);
	if (toDefeat < 0) return true;
	int left = toDefeat - *(const uint16_t*)(rec + 0x16) - *(const uint8_t*)(rec + 0x15) - *(const uint8_t*)(rec + 0x14);
	return left > 0;
}

// Free Romans of a pool record's kind; none of the enemies left as the level
// has them (turtles, the mobile tower, jetpack Romans).
int FreeRomans(const char* rec) {
	char* pool = *(char* const*)rec;
	if (!pool || EnemyKindKeptAsIs(*(char* const*)(rec + 4))) return 0;
	typedef char** (__fastcall* ListFn)(char* self, void* edx);
	char** head = ((ListFn)(*(void***)pool)[12])(pool, nullptr);
	int n = 0;
	for (char* m = head ? *head : nullptr; m && n < 64; m = *(char**)(m + 4)) n++;
	return n;
}

// The pools of `sq` a Roman may come from, weighted for a random pick by their
// Romans left to send in (an endless pool as 8; a spent one as 1, given one
// more to defeat -- not in a squad a gate counts: the fight's number would go
// up): the ones with a free Roman. The squad's own mix: LV3-6's fight sends a
// big Roman for every 3 basic ones. The total.
int PoolWeights(char* sq, int w[8]) {
	char* recs = *(char**)(sq + 0x144);
	unsigned nr = *(unsigned*)(sq + 0x14C);
	bool gated = *(int8_t*)(sq + 0x154) >= -1;
	int total = 0;
	for (unsigned k = 0; k < 8; k++) {
		w[k] = 0;
		if (!recs || k >= nr) continue;
		const char* rec = recs + 0x1C * k;
		if (!FreeRomans(rec)) continue;
		int16_t toDefeat = *(const int16_t*)(rec + 0x18);
		int left = toDefeat - *(const uint16_t*)(rec + 0x16) - *(const uint8_t*)(rec + 0x15) - *(const uint8_t*)(rec + 0x14);
		w[k] = toDefeat < 0 ? 8 : left > 0 ? left : gated ? 0 : 1;
		total += w[k];
	}
	return total;
}

int PickPool(char* sq) {
	int w[8];
	int total = PoolWeights(sq, w);
	if (!total) return -1;
	int r = (int)Random((unsigned)total);
	for (int k = 0; k < 8; k++) {
		if (r < w[k]) return k;
		r -= w[k];
	}
	return -1;
}

// A squad whose fight is over: nothing left to send (SquadSpent), none of its
// Romans alive but those out for a hero, and a pool beaten to its count (it was
// fought, not empty). Its Romans may come back for a waiting hero -- "revenge",
// the user's idea: asleep (+0x152 bit 1 clear) it is woken for them (SendGroup);
// a hero in its box usually has the game wake it first. Only for a hero in its
// box (SquadFor): its Romans out of it are sent off at once.
bool Completed(char* sq) {
	if (*(int8_t*)(sq + 0x154) >= -1 || !SquadSpent(sq) || OthersAlive(sq)) return false;
	char* recs = *(char**)(sq + 0x144);
	unsigned nr = *(unsigned*)(sq + 0x14C);
	for (unsigned r = 0; recs && r < nr && r < 16; r++) {
		int16_t count = *(int16_t*)(recs + 0x1C * r + 0x18);
		if (count > 0 && *(uint16_t*)(recs + 0x1C * r + 0x16) >= (uint16_t)count) return true;
	}
	return false;
}

// Squads a fight's gate has taken this level (+0x154 >= -1 at some time; -2
// again once its fight is over, as for squads never gated), by squad index. A
// gated fight is the other hero's (the waiting one is never in it): none of its
// Romans come for the waiting hero while it lasts -- the user's LV3-4, Obelix
// got one of the courtyard fight's Romans at the library's marker; once it is
// over, its Romans may come back (Completed).
const unsigned kMaxSquads = 256;
char* g_gated[kMaxSquads];
int   g_gatedLevel = -1;

void NoteGatedSquads() {
	CKYellowPages* yp = YellowPages();
	int level = yp && yp->gameManager ? *(int*)((char*)yp->gameManager + ax::kGM_currentLevel) : -1;
	if (level != g_gatedLevel) {   // a level loaded (again): its squads anew
		for (char*& g : g_gated) g = nullptr;
		g_gatedLevel = level;
	}
	for (unsigned i = 0, n = GetNumLvlClassInstances(4, 26); i < n && i < kMaxSquads; i++) {
		char* sq = (char*)GetLvlObject(4, 26, i);
		if (g_gated[i] && g_gated[i] != sq) g_gated[i] = nullptr;   // another level's
		if (!sq || g_gated[i] || *(int8_t*)(sq + 0x154) < -1) continue;
		g_gated[i] = sq;
		static int s_told = 0;
		if (s_told++ < 40) Log("nolock: squad #%u %p is a gated fight's -- no Romans of it for a waiting hero this level", i, sq);
	}
}

bool GatedFight(char* sq, unsigned i) { return i < kMaxSquads && g_gated[i] == sq && !Completed(sq); }

// A squad that can send a Roman: its AI running (+0x152 bit 1, not 0x2000) or
// its fight over (revenge: woken for them, SendGroup), a pool to send it from;
// `left`: Romans left to send in.
bool SquadCanSend(char* sq, bool* left) {
	if (!sq || (*(uint16_t*)(sq + 0x152) & 0x2000)) return false;
	if (!(*(uint8_t*)(sq + 0x152) & 2) && !Completed(sq)) return false;
	int w[8];
	if (!PoolWeights(sq, w)) return false;
	char* recs = *(char**)(sq + 0x144);
	unsigned nr = *(unsigned*)(sq + 0x14C);
	*left = false;
	for (unsigned k = 0; k < nr && k < 8; k++)
		if (PoolHasRomans(recs + 0x1C * k)) *left = true;
	return true;
}

// Is `p` in squad `sq`'s box (x and z: max +0x11C.., min +0x128..)? A squad's
// Romans out of its box are sent off as they come in (state 1, knocked out at
// once: a revenge squad far off had them vanish at their marker) -- they chase a
// hero within it.
bool InBox(const char* sq, const Vector3& p) {
	const float* f = (const float*)sq;
	return p.x >= f[0x4A] && p.x <= f[0x47] && p.z >= f[0x4C] && p.z <= f[0x49];
}

// The squad nearest `at` that can send Romans, with `at` in its box: one with
// Romans left first; else any (a spent pool is given one more to defeat, see
// SendGroup; a squad whose fight is over: revenge).
char* SquadFor(const Vector3& at, unsigned* idx) {
	char* best = nullptr;
	float bestD = 1e30f;
	for (unsigned i = 0, n = GetNumLvlClassInstances(4, 26); i < n; i++) {
		char* sq = (char*)GetLvlObject(4, 26, i);
		bool left;
		if (!sq || !InBox(sq, at) || GatedFight(sq, i) || !SquadCanSend(sq, &left)) continue;
		float penalty = left ? 0.0f : 1e6f;   // spent squads only when no other can send
		const float* f = (const float*)sq;
		Vector3 c((f[0x47] + f[0x4A]) * 0.5f, (f[0x48] + f[0x4B]) * 0.5f, (f[0x49] + f[0x4C]) * 0.5f);
		float d = (c - at).len2() + penalty;
		if (d < bestD) { bestD = d; best = sq; *idx = i; }
	}
	return best;
}

// Seen by a player as the squads' markers are (proximity.cpp): a wall between
// counts as out of view.
bool InAView(const Vector3& p) {
	float c[3] = { p.x, p.y, p.z };
	return MarkerSeen(c);
}

// Where Romans of squad `sender` come in for `h`, as squads bring theirs: a
// spawn marker the level placed (+0x5C, 8 bytes each, +0 -> position; count
// +0x64) -- the sender's own; any squad's with nolock_romans_any_markers (the
// user saw Romans stuck in the air: a marker placed for other Romans) -- within
// 40 units, at least 6 from both heroes, in no player's view (the co-op rule for
// squad markers) and one the hero can walk to (the level's pathfinding graph:
// not behind a shut door, on the other hero's side) -- the nearest; else the
// hero's walk-to marker (below). None, no Roman: places picked off the graph's
// cells were unreliable (its walkable cells take in shallow water -- LV2-3's
// Romans there drowned).
bool SpawnPoint(Trio t, Hero h, char* sender, Vector3* at, const char** how) {
	Hero ha = t.GetHero(HERO_ASTERIX), ho = t.GetHero(HERO_OBELIX);
	const float kReach = 40.0f;
	float bestD = kReach * kReach;
	bool have = false;
	int total = 0, others = 0, outside = 0, nearHero = 0, seen = 0, unreachable = 0;
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
			if (!InBox(sender, pv)) { outside++; continue; }
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
	// the doors; LV3-4: Obelix's in the library): placed by the level and on the
	// hero's side by design -- walkable to, checked all the same. Only out of
	// view (else the next try).
	const Vector3& tg = h.AiTargetPos();
	float dt = (tg - h.Pos()).len2();
	bool byHero = (ha && (ha.Pos() - tg).len2() < 36.0f) || (ho && (ho.Pos() - tg).len2() < 36.0f);
	bool walk = dt < kReach * kReach && !byHero && InBox(sender, tg) && !InAView(tg) && PfReachable(h.Pos(), tg) == PF_YES;
	if (walk) {
		*at = tg;
		*how = "its walk-to marker";
		return true;
	}
	static int s_told = 0;
	if (s_told++ < 5)
		Log("nolock: spawn markers within %.0f units of %s: %d -- %d other squads', %d out of the squad's box, %d by a hero, "
		    "%d in view, %d not walkable to; its walk-to marker %.0f units off%s%s%s", kReach,
		    HeroName(t.GetHero(HERO_ASTERIX).raw() == h.raw() ? HERO_ASTERIX : HERO_OBELIX), total, others, outside, nearHero,
		    seen, unreachable, sqrtf(dt), byHero ? ", by a hero" : "", InAView(tg) ? ", in view" : "",
		    InBox(sender, tg) ? "" : ", out of the squad's box");
	return false;
}

// Without nolock_romans_any_markers: a squad that can send Romans, with Romans
// left to send in, at one of its own spawn markers (as SpawnPoint takes them)
// -- the nearest to `h` of all squads'. LV3-6: the nearest squad's own markers
// are up on the walls (not walkable to), the fight's squad's three on the
// ground.
char* SquadAtOwnMarker(Trio t, Hero h, unsigned* idx, Vector3* at) {
	Hero ha = t.GetHero(HERO_ASTERIX), ho = t.GetHero(HERO_OBELIX);
	float bestD = 40.0f * 40.0f;
	char* best = nullptr;
	for (unsigned i = 0, n = GetNumLvlClassInstances(4, 26); i < n; i++) {
		char* sq = (char*)GetLvlObject(4, 26, i);
		bool left;
		if (!sq || !InBox(sq, h.Pos()) || GatedFight(sq, i) || !SquadCanSend(sq, &left) || !left) continue;
		char* list = *(char**)(sq + 0x5C);
		unsigned nm = list ? *(unsigned*)(sq + 0x64) : 0;
		for (unsigned k = 0; k < nm && k < 64; k++) {
			const float* m = *(const float**)(list + k * 8);
			if (!m) continue;
			Vector3 pv(m[0], m[1], m[2]);
			float d = (pv - h.Pos()).len2();
			if (d >= bestD || (ha && (ha.Pos() - pv).len2() < 36.0f) || (ho && (ho.Pos() - pv).len2() < 36.0f)) continue;
			if (InAView(pv) || PfReachable(h.Pos(), pv) != PF_YES) continue;
			bestD = d;
			best = sq;
			*idx = i;
			*at = pv;
		}
	}
	return best;
}

// A group of 1 to 6 Romans, all from one squad at one point, each from a pool
// picked at random (PickPool) -- fewer if the squad runs out of free Romans.
void SendGroup(int hero, Trio t, Hero h) {
	Waiting& w = g_wait[hero];
	unsigned idx = 0;
	Vector3 at;
	const char* how = "its squad's spawn marker";
	char* sq = g_config.noLockRomansAnyMarkers ? nullptr : SquadAtOwnMarker(t, h, &idx, &at);
	if (!sq) {
		sq = SquadFor(h.Pos(), &idx);
		if (!sq) {
			if (w.noneTold++ < 3) Log("nolock: no squad with Romans to send for %s (awake, or its fight over)", HeroName(hero));
			return;
		}
		if (!SpawnPoint(t, h, sq, &at, &how)) {
			if (w.noneTold++ < 3) Log("nolock: no spawn marker near %s (unseen, walkable to) for a Roman", HeroName(hero));
			return;
		}
	}
	// A squad whose end has come already (spent, none of its Romans left) would
	// send it again as these go.
	bool ended = SquadSpent(sq) && !OthersAlive(sq);
	// Asleep (its fight over: revenge): woken the game's way, as a hero coming back
	// into its box wakes it (vf33: no Romans of its own left to place there; the
	// flag alone, without its set-up, had these taken off at its next tick).
	bool asleep = !(*(uint8_t*)(sq + 0x152) & 2);
	if (asleep) {
		typedef void(__fastcall* WakeFn)(char* self, void* edx);
		((WakeFn)(*(void***)sq)[33])(sq, nullptr);
	}
	int want = 1 + (int)Random(kMaxGroup), sent = 0;
	char pools[64];
	int pk = 0;
	pools[0] = 0;
	typedef uint32_t(__fastcall* CmdFn)(char* self, void* edx, uint32_t* args);
	for (int k = 0; k < want && g_nOut < 2 * kMaxGroup; k++) {
		int pool = PickPool(sq);
		if (pool < 0) break;
		char* rec = *(char**)(sq + 0x144) + 0x1C * pool;
		// A spent pool gets one more to defeat (+0x18): a gate counting the squad
		// (count - sent + alive) stays even -- one more sent, one more to beat.
		bool spent = !PoolHasRomans(rec);
		if (spent) *(int16_t*)(rec + 0x18) += 1;
		uint8_t alive0 = rec[0x11], queued0 = rec[0x14];
		uint32_t args[3] = { 1, (uint32_t)(uintptr_t)&at, (uint32_t)pool };
		((CmdFn)(*(void***)sq)[55])(sq, nullptr, args);
		if ((uint8_t)rec[0x11] == alive0) {   // not placed: what it queued taken back
			if ((uint8_t)rec[0x14] > queued0) rec[0x14] = queued0;
			if (spent) *(int16_t*)(rec + 0x18) -= 1;
			break;
		}
		char* last = *(char**)(rec + 8);
		while (last && *(char**)(last + 0x50)) last = *(char**)(last + 0x50);
		if (last) g_out[g_nOut++] = { last, sq, idx, pool, hero };
		sent++;
		pk += sprintf(pools + pk, " %d", pool);
	}
	if (!sent) {
		if (asleep) {   // asleep again
			typedef void(__fastcall* AwayFn)(char* self, void* edx);
			((AwayFn)(*(void***)sq)[34])(sq, nullptr);
		}
		if (w.noneTold++ < 3) Log("nolock: squad %p has no Roman free for %s", sq, HeroName(hero));
		return;
	}
	if (ended) MarkEnded(sq, idx, asleep);
	w.groups++;
	if (g_groupLogs++ < 200)
		Log("nolock: %d of %d Romans of squad #%u %p (pools%s)%s sent at %s from %s (%.1f %.1f %.1f) [group %d]", sent, want,
		    idx, sq, pools, asleep ? " -- its fight was over: revenge --" : "", HeroName(hero), how, at.x, at.y, at.z, w.groups);
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
		int k = sprintf(buf, "nolock:   squad #%u %p box centre (%.1f %.1f %.1f) size %.0fx%.0f active %d gate %d markers %u pools:",
		                i, sq, c.x, c.y, c.z, f[0x47] - f[0x4A], f[0x49] - f[0x4C], (*(uint8_t*)(sq + 0x152) >> 1) & 1,
		                *(int8_t*)(sq + 0x154), *(char**)(sq + 0x5C) ? *(unsigned*)(sq + 0x64) : 0);
		char* recs = *(char**)(sq + 0x144);
		for (unsigned p = 0, np = *(unsigned*)(sq + 0x14C); recs && p < np && p < 4; p++) {
			const char* r = recs + 0x1C * p;
			k += sprintf(buf + k, " [%u: alive %d sent %d of %d]", p, *(uint8_t*)(r + 0x11), *(uint16_t*)(r + 0x16), *(int16_t*)(r + 0x18));
		}
		Log("%s", buf);
	}
}

} // namespace

bool NoLockInstallHooks() {
	return PatchVtableSlot(kSquadEnemyVtbl, 5, kSquadEnemyEvent, (void*)SquadEventHook, (void**)&g_origSquadEvent,
	                       "squad events (a squad's end again, no-lock Romans)");
}

void NoLockRomansFrame() {
	DWORD now = GetTickCount();
	if (!KalLevelIsLoaded()) {
		g_nOut = 0;
		for (Ended& e : g_ended) e = Ended();
		for (char*& g : g_gated) g = nullptr;
		return;
	}
	Prune(now);
	NoteGatedSquads();
	if (!g_config.noLock || !g_config.noLockRomans || !CoopP2Joined() || CoopSuspended()) return;
	Trio t = GetTrio();
	if (!t) return;
	for (int hero = HERO_ASTERIX; hero <= HERO_OBELIX; hero++) {
		Hero h = t.GetHero(hero);
		Waiting& w = g_wait[hero];
		bool waiting = h && CoopNoLockWaiting(hero);
		if (waiting == w.on) continue;
		Log("nolock: %s %s", HeroName(hero),
		    waiting ? "would be kept waiting by the level -- Romans will come for it"
		            : "is no longer kept waiting -- no more Romans for it");
		w = Waiting();
		w.on = waiting;
		w.next = now + kFirstMs;
		if (waiting) DumpAround(hero, h);
	}
	EndFightsLeftToOurs();
	for (int hero = HERO_ASTERIX; hero <= HERO_OBELIX; hero++) {
		Waiting& w = g_wait[hero];
		if (!w.on || IndivFallen(hero) || OutFor(hero) || (int)(now - w.next) < 0) continue;   // (none for a hero down)
		w.next = now + kRetryMs;
		SendGroup(hero, t, t.GetHero(hero));
	}
}

// The trio's fight mode (FUN_00481950) counts an enemy near the hero only if
// it was updated this frame (its node's +0x14 = the frame count 0x6464C4): the
// Romans out for a hero are given another count for the pass.
void NoLockHideFromFight(bool hide) {
	static char* s_nodes[2 * kMaxGroup];
	static uint16_t s_stamps[2 * kMaxGroup];
	static int s_n = 0;
	if (!hide) {
		for (int i = 0; i < s_n; i++) *(uint16_t*)(s_nodes[i] + 0x14) = s_stamps[i];
		s_n = 0;
		return;
	}
	s_n = 0;
	uint16_t frame = *(const uint16_t*)0x6464C4;
	for (int i = 0; i < g_nOut; i++) {
		if (!StillOut(g_out[i])) continue;
		char* node = *(char**)(g_out[i].roman + 0x10);
		if (!node) continue;
		s_nodes[s_n] = node;
		s_stamps[s_n] = *(uint16_t*)(node + 0x14);
		*(uint16_t*)(node + 0x14) = (uint16_t)(frame + 0x8000);
		s_n++;
	}
}

// After the squad's tick (vf31 gives each Roman a slot round its formation
// centre: component 0x46, +0 slot, +0x18 the step towards it -- proximity.cpp
// SplitSquad) the slots of its Romans out for a hero are moved round that hero,
// 2.5 units off, spread out: they go after it out of their squad's box and
// fight zone too, and stay on its side.
void NoLockSteer(char* sq) {
	if (!g_nOut) return;
	Trio t = GetTrio();
	if (!t) return;
	typedef char*(__fastcall* CpntFn)(char* self, void* edx, int id);
	float minStep = *(float*)(sq + 0xC0);
	int n[2] = {}, k[2] = {};
	for (int i = 0; i < g_nOut; i++) if (g_out[i].sq == sq) n[g_out[i].hero]++;
	for (int i = 0; i < g_nOut; i++) {
		const Out& o = g_out[i];
		if (o.sq != sq) continue;
		int place = k[o.hero]++;
		Hero h = t.GetHero(o.hero);
		if (!h || !StillOut(o)) continue;   // (a hero down too: they stay on its side for the one who comes to revive it)
		char* node = *(char**)(o.roman + 0x10);
		char* fr = node ? *(char**)(node + 4) : nullptr;
		float* slot = (float*)((CpntFn)(*(void***)o.roman)[12])(o.roman, nullptr, 0x46);
		if (!fr || !slot) continue;
		const float* rp = (const float*)(fr + 0x80);
		float a = 6.2831853f * place / n[o.hero] + 1.3f * o.hero;
		Vector3 hp = h.Pos();
		float sx = hp.x + 2.5f * cosf(a), sz = hp.z + 2.5f * sinf(a);
		slot[0] = sx; slot[1] = hp.y; slot[2] = sz;
		float dx = sx - rp[0], dz = sz - rp[2], len = sqrtf(dx * dx + dz * dz);
		float f = len <= 0.01f ? 0.0f : 1.0f / (len > minStep ? len : minStep);
		slot[6] = dx * f; slot[7] = 0.0f; slot[8] = dz * f;
	}
}

bool NoLockEndedFight(char* sq) { return FindEnded(sq) != nullptr; }

bool NoLockKeepsSquad(char* sq) {
	for (int i = 0; i < g_nOut; i++)
		if (g_out[i].sq == sq && StillOut(g_out[i])) return true;
	return false;
}
