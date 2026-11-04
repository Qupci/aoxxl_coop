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
#include <cstring>

// No-lock mode's Romans ([coop_difficulty.experimental] nolock_romans): while
// the level would have kept a player's hero waiting -- a scripted order no-lock
// mode leaves unfollowed (CoopNoLockWaiting): the other hero in a locked fight
// (LV3-4: Obelix shut in the library while Asterix fights in the courtyard) or
// held by a sequence -- Romans come for it in groups of 1 to 6, the next group
// 20 to 90 s (mostly about 30) after the last of a group is gone, so the free
// player has something to do until the other lifts the wait; the first 5 to 25
// s after the wait begins. They go after that hero wherever it goes and start
// no fight music. Never for the leader: the one in the locked fight (the level's
// scripts follow it). They come from a squad whose fight was won ("revenge",
// the user's choice), 20 to 90 s after the win, from anywhere in the level: its
// box is moved over the waiting hero meanwhile. Only while no fight has been won
// yet, from a squad whose box holds the hero -- never one whose fight has not
// begun (NotBegun) or is under way at a gate.
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
const DWORD kRetryMs  = 6000;   // no Roman could come: the next try

// The game's rand() is its own; this one is seeded from the clock.
unsigned Random(unsigned n) {
	static uint32_t s = GetTickCount() * 2654435761u;
	s = s * 1664525u + 1013904223u;
	return n ? (s >> 8) % n : 0;
}

// The pause before the next group: 20 + 70 u^3 s -- 20 to 90, half under 29.
DWORD PauseMs() {
	static const char* s_test = getenv("XXL_NOLOCKPAUSE");   // autotests: a fixed pause (ms)
	if (s_test) return (DWORD)atoi(s_test);
	float u = Random(10001) / 10000.0f;
	return 20000 + (DWORD)(70000.0f * u * u * u);
}

// The first group, 5 to 25 s after the wait begins (the user: not behind the
// hero the moment the heroes part).
DWORD FirstMs() {
	static const char* s_test = getenv("XXL_NOLOCKFIRST");   // autotests: a fixed delay (ms)
	return s_test ? (DWORD)atoi(s_test) : 5000 + Random(20001);
}

struct Waiting {
	bool  on = false;
	DWORD next = 0;      // the next group, once none of the last is left
	int   groups = 0;
	int   noneTold = 0;
	int   soonTold = 0;
} g_wait[2];

// A Roman sent for a hero, until beaten or put away: in its pool record's list
// and not finished (as the squad sees it). The squad is checked to be the
// level's still (a level change frees it) before anything is read. `nearest` /
// `since` / (ax, az): how near its spot it has got, since when, and where it
// was then (GivenUp); `stuck`: it stands where it is, given up on the spot at
// (gx, gz) -- the walk to it checked again at `checked` -- since `stuckAt`
// (`heldTold`: still held back a second on, logged).
struct Out {
	char* roman; char* sq; unsigned sqIdx; int pool; int hero; uint32_t boxBits; bool faced;
	float nearest; DWORD since; float ax, az; bool stuck; float gx, gz; DWORD checked;
	DWORD stuckAt; bool heldTold; bool modeTold;
};
Out g_out[2 * kMaxGroup];
int g_nOut = 0;
int g_groupLogs = 0;

bool SquadValid(char* sq, unsigned idx) {
	return KalLevelIsLoaded() && idx < GetNumLvlClassInstances(4, 26) && (char*)GetLvlObject(4, 26, idx) == sq;
}

// Knocked out (+0x24 bit 1) is gone too: a squad's script may send the same
// Roman in again at once (LV3-6's squad #3 put one beaten by Obelix back up on
// its wall marker, still taken for ours).
bool StillOut(const Out& o) {
	if (!SquadValid(o.sq, o.sqIdx)) return false;
	char* recs = *(char**)(o.sq + 0x144);
	if (!recs || o.pool >= (int)*(unsigned*)(o.sq + 0x14C)) return false;
	int guard = 0;
	for (char* m = *(char**)(recs + 0x1C * o.pool + 8); m && guard++ < 256; m = *(char**)(m + 0x50))
		if (m == o.roman) return !(*(uint8_t*)(m + 8) & 1) && !(*(uint32_t*)(m + 0x24) & 2);
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

// Diagnostics: a Roman out for a hero and its squad's box as it changes (the
// Roman's tick, Shared_Class_2_84_vf23 0x4CEE00: its place, hook +0x2C, out of
// the box -- x, y and z -- puts it back at the last one inside, +0x38, with
// +0x24 bit 14, as does a place off the enemies' pathfinding cells; out with the
// last one out too, bit 1: knocked out); bounded.
void WatchOut() {
	static int s_told = 0;
	for (int i = 0; i < g_nOut && s_told < 40; i++) {
		Out& o = g_out[i];
		uint32_t bits = *(uint32_t*)(o.roman + 0x24) & 0x4002;
		if (bits == o.boxBits) continue;
		o.boxBits = bits;
		const float* p = (const float*)(o.roman + 0x2C);
		const float* f = (const float*)o.sq;
		s_told++;
		Log("nolock: Roman %p of squad #%u at (%.1f %.1f %.1f)%s%s -- its squad's box x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f",
		    o.roman, o.sqIdx, p[0], p[1], p[2], bits & 0x4000 ? " held back (out of its squad's box or off the pathfinding)" : "",
		    bits & 2 ? " knocked out (beaten, out of its squad's box or on a deadly floor)" : bits ? "" : " free to go again", f[0x4A], f[0x47],
		    f[0x4B], f[0x48], f[0x4C], f[0x49]);
	}
	static const bool s_trace = getenv("XXL_NOLOCKTRACE") != nullptr;   // autotests: each Roman every 15 frames
	static int s_frames = 0, s_lines = 0;
	if (!s_trace || !g_nOut || ++s_frames % 15 || s_lines >= 400) return;
	Trio t = GetTrio();
	if (!t) return;
	typedef char*(__fastcall* CpntFn)(char* self, void* edx, int id);
	char line[1200];
	int len = _snprintf(line, sizeof(line), "nolock: trace (pos, hero / slot off, [reaction], bits):");
	for (int i = 0; i < g_nOut && len > 0 && len < (int)sizeof(line); i++) {
		const Out& o = g_out[i];
		Hero h = t.GetHero(o.hero);
		const float* p = (const float*)(o.roman + 0x2C);
		const float* slot = (const float*)((CpntFn)(*(void***)o.roman)[12])(o.roman, nullptr, 0x46);
		if (!h || !slot) continue;
		Vector3 hp = h.Pos();
		float hx = hp.x - p[0], hz = hp.z - p[2], sx = slot[0] - p[0], sz = slot[2] - p[2];
		char* node = *(char**)(o.roman + 0x10);
		const float* fp = node && *(char**)(node + 4) ? (const float*)(*(char**)(node + 4) + 0x80) : p;
		const float* in = (const float*)(o.roman + 0x38);
		const float* fw = fp != p ? fp - 4 : p;   // (frame +0x70: forward)
		len += _snprintf(line + len, sizeof(line) - len, " (%.2f %.1f %.2f) %.1f/%.1f [%02X] %X%s fr %.2f %.2f in %.2f %.2f slot %.2f %.2f node %d fw %.2f %.2f",
		                 p[0], p[1], p[2], sqrtf(hx * hx + hz * hz), sqrtf(sx * sx + sz * sz), *(uint8_t*)(o.roman + 0x1A4),
		                 *(uint32_t*)(o.roman + 0x24) & 0x4002, o.stuck ? " stuck" : "", fp[0], fp[2], in[0], in[2], slot[0], slot[2],
		                 (int)*(int16_t*)(o.roman + 0xA8), fw[0], fw[2]);
	}
	line[sizeof(line) - 1] = 0;
	Log("%s", line);
	s_lines++;
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

// A squad whose fight was won, brought over to a waiting hero out of its box
// (the user's idea: revenge from any won fight): its box (+0x11C.., max then
// min -- its Romans out of it are sent off) round what the hero can walk to
// within 60 units (PfReachBox; LV3-4's library: the library) -- one holding the
// hero keeps its own and has that added (MoveBox `keep`) -- grown as the hero
// goes (every 1 s); its own box back once none of its Romans are out for the
// hero (the next group may come from another squad) or the wait is over (any
// still out are sent off then: bonus Romans). One per waiting hero.
const float kZoneReach = 60.0f;
struct Moved { char* sq; unsigned idx; float box[6]; DWORD grown; };
Moved g_moved[2];

// Max then min as the squad has them, a little more than the cells (Romans
// stand on the nodes' tops, jump, are knocked flying): false with no graph.
bool ReachBox(const Vector3& at, float box[6]) {
	Vector3 mn, mx;
	if (!PfReachBox(at, kZoneReach, &mn, &mx)) return false;
	box[0] = mx.x + 2.0f; box[1] = mx.y + 6.0f; box[2] = mx.z + 2.0f;
	box[3] = mn.x - 2.0f; box[4] = mn.y - 3.0f; box[5] = mn.z - 2.0f;
	return true;
}

// `keep`: a squad whose box holds the hero keeps it, grown over what the hero can
// walk to (the user: no-lock Romans go after the waiting hero anywhere; LV3-6's
// squad #4, its box's east edge x -132.0 at the pier: the Roman going for Obelix
// past it -- the squad's attacker walks straight at the hero, not to its slot --
// slid along the pier's crates onto the edge, held back there, running on the
// spot, in three of the user's runs and nolock36y; given up it stood, then went
// for him again; LV2-1's squad #29's two by its west edge).
bool MoveBox(int hero, char* sq, unsigned idx, const Vector3& at, bool keep) {
	float box[6];
	if (!ReachBox(at, box)) return false;
	Moved& m = g_moved[hero];
	float* f = (float*)sq + 0x47;
	m.sq = sq;
	m.idx = idx;
	m.grown = GetTickCount();
	memcpy(m.box, f, sizeof(m.box));
	for (int k = 0; k < 3 && keep; k++) {
		if (f[k] > box[k]) box[k] = f[k];
		if (f[k + 3] < box[k + 3]) box[k + 3] = f[k + 3];
	}
	memcpy(f, box, sizeof(box));
	if (g_groupLogs++ < 200)
		Log("nolock: squad #%u %p %s %s -- its box x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f -> x %.1f..%.1f y "
		    "%.1f..%.1f z %.1f..%.1f (what %s can walk to within %.0f units)", idx, sq, keep ? "grown round" : "brought over to",
		    HeroName(hero), m.box[3], m.box[0], m.box[4], m.box[1], m.box[5], m.box[2], f[3], f[0], f[4], f[1], f[5], f[2],
		    HeroName(hero), kZoneReach);
	return true;
}

void GrowBox(int hero, const Vector3& at, DWORD now) {
	Moved& m = g_moved[hero];
	if (!m.sq || (int)(now - m.grown) < 1000 || !SquadValid(m.sq, m.idx)) return;
	m.grown = now;
	float box[6];
	if (!ReachBox(at, box)) return;
	float* f = (float*)m.sq + 0x47;
	bool grew = false;
	for (int k = 0; k < 3; k++) {
		if (box[k] > f[k]) { f[k] = box[k]; grew = true; }
		if (box[k + 3] < f[k + 3]) { f[k + 3] = box[k + 3]; grew = true; }
	}
	static int s_told = 0;
	if (grew && s_told++ < 20)
		Log("nolock: squad #%u's box grown as %s goes: x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f", m.idx, HeroName(hero), f[3],
		    f[0], f[4], f[1], f[5], f[2]);
}

void RestoreBox(int hero, const char* why) {
	Moved& m = g_moved[hero];
	if (!m.sq) return;
	if (SquadValid(m.sq, m.idx)) {
		memcpy((float*)m.sq + 0x47, m.box, sizeof(m.box));
		if (g_groupLogs++ < 200) Log("nolock: squad #%u %p's own box back (%s)", m.idx, m.sq, why);
	}
	m = Moved();
}

// Romans gone: a hero waiting with none of its group left gets the next group
// after a pause, a squad brought over to it its own box back. Ended squads
// forgotten 2 s after their last Roman out.
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
	for (int hero = HERO_ASTERIX; hero <= HERO_OBELIX; hero++)
		if (g_moved[hero].sq && !OutFor(hero)) RestoreBox(hero, "none of its Romans left for the hero");
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
// has them (turtles, the mobile tower, jetpack Romans) nor jumping Romans or
// archers (the user: such kinds make no sense coming for a hero).
const uintptr_t kJumpingRomanCpnt = 0x627CC0, kRomanArcherCpnt = 0x627CF0;

int FreeRomans(const char* rec) {
	char* pool = *(char* const*)rec;
	char* cpnt = *(char* const*)(rec + 4);
	uintptr_t kind = cpnt ? *(uintptr_t*)cpnt : 0;
	if (!pool || EnemyKindKeptAsIs(cpnt) || kind == kJumpingRomanCpnt || kind == kRomanArcherCpnt) return 0;
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
// a hero in its box usually has the game wake it first. Its Romans out of its
// box are sent off at once: one far off is brought over (MoveBox).
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

// Squads whose fight is won (Completed, looked at every 0.5 s), by squad index:
// their Romans come back for a waiting hero only after a pause as between groups
// (20 to 90 s) -- the user's LV3-4: the courtyard's Romans at the library the
// moment its fight was won felt off.
char* g_wonSq[kMaxSquads];
bool  g_won[kMaxSquads];
DWORD g_wonAt[kMaxSquads], g_wonPause[kMaxSquads], g_wonLooked = 0;

void NoteWonSquads(DWORD now) {
	if ((int)(now - g_wonLooked) < 500) return;
	g_wonLooked = now;
	static int s_told = 0;
	for (unsigned i = 0, n = GetNumLvlClassInstances(4, 26); i < n && i < kMaxSquads; i++) {
		char* sq = (char*)GetLvlObject(4, 26, i);
		if (g_wonSq[i] != sq) { g_wonSq[i] = sq; g_won[i] = false; }   // another level's
		bool won = sq && Completed(sq);
		if (won == g_won[i]) continue;
		g_won[i] = won;
		if (!won) continue;
		g_wonAt[i] = now;
		g_wonPause[i] = PauseMs();
		if (s_told++ < 40)
			Log("nolock: squad #%u %p's fight is won -- its Romans may come back for a waiting hero in %.0f s", i, sq,
			    g_wonPause[i] / 1000.0f);
	}
}

bool CooledDown(unsigned i, DWORD now) { return i < kMaxSquads && g_won[i] && now - g_wonAt[i] >= g_wonPause[i]; }

// A squad whose fight is yet to begin: none of its Romans sent in or placed
// (every pool's sent +0x16 and alive +0x11 0), no gate on it (+0x154 -2). Its
// script sends them in as the fight begins -- LV3-6's squad #3 once the keg
// blows (at its markers up on the walls; awake before that, Obelix in its box):
// Romans of it for a waiting hero came early (the user saw them before the
// keg), and its script put the beaten ones back up on its markers.
bool NotBegun(const char* sq) {
	if (*(const int8_t*)(sq + 0x154) >= -1) return false;
	const char* recs = *(char* const*)(sq + 0x144);
	unsigned nr = *(const unsigned*)(sq + 0x14C);
	for (unsigned r = 0; recs && r < nr && r < 16; r++)
		if (*(const uint16_t*)(recs + r * 0x1C + 0x16) || recs[r * 0x1C + 0x11]) return false;
	return true;
}

// A squad that can send a Roman: its AI running (+0x152 bit 1, not 0x2000) or
// its fight over (revenge: woken for them, SendGroup), its fight begun, a pool
// to send it from; `left`: Romans left to send in.
bool SquadCanSend(char* sq, bool* left) {
	if (!sq || (*(uint16_t*)(sq + 0x152) & 0x2000)) return false;
	if (!(*(uint8_t*)(sq + 0x152) & 2) && !Completed(sq)) return false;
	if (NotBegun(sq)) {
		static const char* s_told[8];
		static int s_nTold = 0;
		bool told = false;
		for (int i = 0; i < 8; i++) told |= s_told[i] == sq;
		if (!told && s_nTold < 20) {
			s_told[s_nTold++ % 8] = sq;
			Log("nolock: squad %p's fight has not begun (nothing sent in or placed) -- no Romans of it for a waiting hero "
			    "till then", sq);
		}
		return false;
	}
	int w[8];
	if (!PoolWeights(sq, w)) return false;
	char* recs = *(char**)(sq + 0x144);
	unsigned nr = *(unsigned*)(sq + 0x14C);
	*left = false;
	for (unsigned k = 0; k < nr && k < 8; k++)
		if (PoolHasRomans(recs + 0x1C * k)) *left = true;
	return true;
}

// Is `p` in squad `sq`'s box (max +0x11C.., min +0x128..; x, y and z, as the
// Roman's tick has it: Shared_Class_2_84_vf23 0x4CEE00)? A squad's Romans out of
// its box are sent off as they come in (state 1, knocked out at once: a revenge
// squad far off had them vanish at their marker); one stepping out is put back
// at its last place inside -- they chase a hero within it.
bool InBox(const char* sq, const Vector3& p) {
	const float* f = (const float*)sq;
	return p.x >= f[0x4A] && p.x <= f[0x47] && p.y >= f[0x4B] && p.y <= f[0x48] && p.z >= f[0x4C] && p.z <= f[0x49];
}

// The squad nearest `at` that can send Romans, with `at` in its box: with
// Romans left to send in (`left`); else a spent one whose fight is not won (a
// spent pool is given one more to defeat, see SendGroup) -- a won one: Revenge.
char* SquadFor(const Vector3& at, unsigned* idx, bool left) {
	char* best = nullptr;
	float bestD = 1e30f;
	for (unsigned i = 0, n = GetNumLvlClassInstances(4, 26); i < n; i++) {
		char* sq = (char*)GetLvlObject(4, 26, i);
		bool l;
		if (!sq || !InBox(sq, at) || GatedFight(sq, i) || !SquadCanSend(sq, &l) || l != left || (!left && Completed(sq)))
			continue;
		const float* f = (const float*)sq;
		Vector3 c((f[0x47] + f[0x4A]) * 0.5f, (f[0x48] + f[0x4B]) * 0.5f, (f[0x49] + f[0x4C]) * 0.5f);
		float d = (c - at).len2();
		if (d < bestD) { bestD = d; best = sq; *idx = i; }
	}
	return best;
}

// Would waking `sq` start its fight over? vf33 (0x52EF70) on a squad that comes
// back (+0x13C, the level's, 0 or more) asleep that long (+0x138, its time since)
// sets every pool back (sent 0, alive its starting count) and places them.
bool WakeStartsOver(const char* sq) {
	const float* f = (const float*)sq;
	return !(*(const uint8_t*)(sq + 0x152) & 2) && f[0x4F] >= 0.0f && f[0x4E] >= f[0x4F];
}

// A squad whose fight was won a while ago (CooledDown) that can send Romans, any
// in the level ("revenge": one whose box does not hold the hero is brought over,
// MoveBox), at random -- another than the hero's last one when there is another.
// Not one that waking would start over.
char* Revenge(int hero, DWORD now, unsigned* idx) {
	static unsigned s_last[2] = { ~0u, ~0u };
	unsigned cand[64];
	int nc = 0;
	for (unsigned i = 0, n = GetNumLvlClassInstances(4, 26); i < n && nc < 64; i++) {
		char* sq = (char*)GetLvlObject(4, 26, i);
		bool left;
		if (!sq || !CooledDown(i, now) || !Completed(sq) || g_moved[1 - hero].sq == sq || WakeStartsOver(sq) ||
		    !SquadCanSend(sq, &left))
			continue;
		cand[nc++] = i;
	}
	if (!nc) return nullptr;
	int k = (int)Random((unsigned)nc);
	if (nc > 1 && cand[k] == s_last[hero]) k = (k + 1 + (int)Random((unsigned)nc - 1)) % nc;
	s_last[hero] = *idx = cand[k];
	return (char*)GetLvlObject(4, 26, cand[k]);
}

// A squad whose fight was won, still in its pause after the win, that could
// send Romans then (as Revenge takes them): the time till the first such pause
// is over (`ms`, its squad `idx`).
bool RevengeSoon(int hero, DWORD now, DWORD* ms, unsigned* idx) {
	bool any = false;
	for (unsigned i = 0, n = GetNumLvlClassInstances(4, 26); i < n && i < kMaxSquads; i++) {
		char* sq = (char*)GetLvlObject(4, 26, i);
		bool left;
		if (!sq || g_wonSq[i] != sq || !g_won[i] || CooledDown(i, now) || !Completed(sq) || g_moved[1 - hero].sq == sq ||
		    WakeStartsOver(sq) || !SquadCanSend(sq, &left))
			continue;
		DWORD rest = g_wonPause[i] - (now - g_wonAt[i]);
		if (any && rest >= *ms) continue;
		*ms = rest;
		*idx = i;
		any = true;
	}
	return any;
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
// user saw Romans stuck in the air: a marker placed for other Romans) or for a
// squad brought over (`anyMarkers`; its own are far off) -- within
// 40 units, at least 6 from both heroes, in no player's view (the co-op rule for
// squad markers) and one the hero can walk to (the level's pathfinding graph:
// not behind a shut door, on the other hero's side) -- the nearest; else the
// hero's walk-to marker (below). None, no Roman: places picked off the graph's
// cells were unreliable (its walkable cells take in shallow water -- LV2-3's
// Romans there drowned).
bool SpawnPoint(Trio t, Hero h, char* sender, bool anyMarkers, Vector3* at, const char** how) {
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
			if (sq != sender && !anyMarkers) { others++; continue; }
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
// picked at random (PickPool) -- fewer if the squad runs out of free Romans. The
// squad: one whose fight was won a while ago (Revenge, brought over if need be:
// the user's choice -- LV3-6's ledge squad, not fought yet, sent its spear
// Romans while the fight below was in its pause); one still in its pause after
// the win is waited for. With no won fight's Romans to come: one whose box holds
// the hero with Romans left (at its own marker, else SpawnPoint's); else a spent
// one whose box holds the hero.
void SendGroup(int hero, Trio t, Hero h, DWORD now) {
	Waiting& w = g_wait[hero];
	unsigned idx = 0;
	Vector3 at;
	const char* how = "its squad's spawn marker";
	bool placed = false, revenge = false, brought = false;
	char* sq = Revenge(hero, now, &idx);
	if (sq) {
		revenge = true;
		if (!InBox(sq, h.Pos()) && !(brought = MoveBox(hero, sq, idx, h.Pos(), false))) sq = nullptr;   // (no graph here)
	}
	DWORD soon;
	if (!sq && RevengeSoon(hero, now, &soon, &idx)) {
		w.next = now + soon;
		if (w.soonTold++ < 3)
			Log("nolock: Romans for %s from squad #%u's won fight in %.0f s (its pause after the win)", HeroName(hero), idx,
			    soon / 1000.0f);
		return;
	}
	if (!sq && !g_config.noLockRomansAnyMarkers) placed = (sq = SquadAtOwnMarker(t, h, &idx, &at)) != nullptr;
	if (!sq) { sq = SquadFor(h.Pos(), &idx, true); revenge = false; }
	if (!sq) sq = SquadFor(h.Pos(), &idx, false);
	if (!sq) {
		if (w.noneTold++ < 3) Log("nolock: no squad with Romans to send for %s (awake, or its fight won a while ago)", HeroName(hero));
		return;
	}
	if (!placed && !SpawnPoint(t, h, sq, g_config.noLockRomansAnyMarkers || brought, &at, &how)) {
		if (brought) RestoreBox(hero, "no spawn marker for its Romans");
		if (w.noneTold++ < 3) Log("nolock: no spawn marker near %s (unseen, walkable to) for a Roman", HeroName(hero));
		return;
	}
	bool grown = revenge && !brought && MoveBox(hero, sq, idx, h.Pos(), true);
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
	static const char* s_group = getenv("XXL_NOLOCKGROUP");   // autotests: a fixed group size
	int want = s_group ? atoi(s_group) : 1 + (int)Random(kMaxGroup), sent = 0;
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
			static int s_told = 0;
			if (s_told++ < 10)
				Log("nolock: squad #%u %p pool %d placed no Roman: squad flags %04X phase %d, pool %d to defeat, sent %u, "
				    "fallen %u, queued %u -> %u, alive %u, %d free", idx, sq, pool, *(uint16_t*)(sq + 0x152),
				    *(int8_t*)(sq + 0x154), *(int16_t*)(rec + 0x18), *(uint16_t*)(rec + 0x16), (uint8_t)rec[0x15], queued0,
				    (uint8_t)rec[0x14], (uint8_t)rec[0x11], FreeRomans(rec));
			if ((uint8_t)rec[0x14] > queued0) rec[0x14] = queued0;
			if (spent) *(int16_t*)(rec + 0x18) -= 1;
			break;
		}
		char* last = *(char**)(rec + 8);
		while (last && *(char**)(last + 0x50)) last = *(char**)(last + 0x50);
		if (last) g_out[g_nOut++] = { last, sq, idx, pool, hero, 0, false, 1e9f, now };
		sent++;
		pk += sprintf(pools + pk, " %d", pool);
	}
	if (!sent) {
		if (brought || grown) RestoreBox(hero, "no Roman of it free");
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
		    idx, sq, pools, brought ? " -- its fight was won: revenge, brought over --" : revenge ? " -- its fight was won: revenge --" : "",
		    HeroName(hero), how, at.x, at.y, at.z, w.groups);
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
		int k = sprintf(buf, "nolock:   squad #%u %p box centre (%.1f %.1f %.1f) size %.0fx%.0f, y %.1f..%.1f, active %d gate %d%s markers %u pools:",
		                i, sq, c.x, c.y, c.z, f[0x47] - f[0x4A], f[0x49] - f[0x4C], f[0x4B], f[0x48], (*(uint8_t*)(sq + 0x152) >> 1) & 1,
		                *(int8_t*)(sq + 0x154), NotBegun(sq) ? " (not begun)" : "", *(char**)(sq + 0x5C) ? *(unsigned*)(sq + 0x64) : 0);
		char* recs = *(char**)(sq + 0x144);
		for (unsigned p = 0, np = *(unsigned*)(sq + 0x14C); recs && p < np && p < 4; p++) {
			const char* r = recs + 0x1C * p;
			k += sprintf(buf + k, " [%u: alive %d sent %d of %d]", p, *(uint8_t*)(r + 0x11), *(uint16_t*)(r + 0x16), *(int16_t*)(r + 0x18));
		}
		Log("%s", buf);
		char* list = *(char**)(sq + 0x5C);   // its spawn markers within 40 units: in view, walkable to
		for (unsigned m = 0, nm = list ? *(unsigned*)(sq + 0x64) : 0; m < nm && m < 16; m++) {
			const float* p = *(const float**)(list + m * 8);
			if (!p) continue;
			Vector3 pv(p[0], p[1], p[2]);
			if ((pv - h.Pos()).len2() >= 40.0f * 40.0f) continue;
			Log("nolock:     marker (%.1f %.1f %.1f) %.1f units off%s, walkable to %d", pv.x, pv.y, pv.z,
			    sqrtf((pv - h.Pos()).len2()), InAView(pv) ? ", in view" : "", PfReachable(h.Pos(), pv));
		}
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
		for (Moved& m : g_moved) m = Moved();
		return;
	}
	WatchOut();   // (before Prune drops one knocked out)
	Prune(now);
	NoteGatedSquads();
	NoteWonSquads(now);
	if (!g_config.noLock || !g_config.noLockRomans || !CoopP2Joined() || CoopSuspended()) {
		for (int hero = HERO_ASTERIX; hero <= HERO_OBELIX; hero++) RestoreBox(hero, "no-lock Romans off");
		return;
	}
	Trio t = GetTrio();
	if (!t) return;
	for (int hero = HERO_ASTERIX; hero <= HERO_OBELIX; hero++) {
		Hero h = t.GetHero(hero);
		Waiting& w = g_wait[hero];
		bool waiting = h && CoopNoLockWaiting(hero);
		if (waiting == w.on) continue;
		w = Waiting();
		w.on = waiting;
		DWORD first = FirstMs();
		w.next = now + first;
		if (waiting) Log("nolock: %s would be kept waiting by the level -- Romans will come for it (the first in %.0f s)",
		                 HeroName(hero), first / 1000.0f);
		else Log("nolock: %s is no longer kept waiting -- no more Romans for it", HeroName(hero));
		if (waiting) DumpAround(hero, h);
		else RestoreBox(hero, "the wait is over");
	}
	EndFightsLeftToOurs();
	for (int hero = HERO_ASTERIX; hero <= HERO_OBELIX; hero++) {
		Waiting& w = g_wait[hero];
		if (g_moved[hero].sq && t.GetHero(hero)) GrowBox(hero, t.GetHero(hero).Pos(), now);
		if (!w.on || IndivFallen(hero) || OutFor(hero) || (int)(now - w.next) < 0) continue;   // (none for a hero down)
		w.next = now + kRetryMs;
		SendGroup(hero, t, t.GetHero(hero), now);
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

// A Roman out for a hero held back for 1 s on end (+0x24 bit 14: its every step
// out of its squad's box or off the enemies' cells put back -- it runs on the
// spot), getting neither 0.2 units nearer its spot nor 1 unit from where it was,
// gives up on it (the user: Romans that kept running for a place they could not
// get to -- LV3-6, one held at squad #4's box edge for 9 s, Obelix on the pier
// past it; single player has it too) and stands where it is, facing as it does
// (its slot its place, frame +0x80, its facing the frame's forward, +0x70, no
// step -- each frame): neither a step nor a turn to make, however small -- one
// held back finishes neither (each frame's move put back, or a cell its tick
// flags; the user's LV3-6 Roman, given up at x -132.0 with a stand spot 0.5 in,
// then one facing the hero, ran on the spot to the level's end three times;
// nolock36s + XXL_NOLOCKNOCLAMP: one given up 0.01 in from the edge, its slot
// where it stood and facing as it did then, turned 0.04 rad / stepped 0.05 out
// onto a flagged cell, 0x27 / 0x25 and held for good; LV2-1's two held where they
// came in), until the spot it gave up on has moved 1.5 units and
// the level's pathfinding has it walkable to from where the Roman stands, the
// straight line there within the graph's nodes as the Roman walks it (checked
// every 0.5 s at most -- LV2-1's walk-to marker: walkable to, yet held again 1 s
// on each time), or a hero comes within 4 units. Not one with a hero within 4 units (it goes
// for that hero on its own) or reacting to a hit (+0x1A4 below 0x22), as for
// guards (guard.cpp Unstick). Only held back: a Roman a wall stops stands on its
// own (nolock36s / nolock36u: 4.8 to 26 units off its spot, 0x22), and one that
// was by a hero walking off keeps still for 1-2.5 s (0x29 / 0x2A) before it
// follows -- "walking, not nearer" (the first tries) had those stop, and one
// left behind until the hero's spot moved again.
const float kStuckGain = 0.2f, kStuckOff = 0.5f, kStuckMove = 1.0f, kBusy = 4.0f, kRetryMove = 1.5f;
const DWORD kStuckMs = 1000, kReachCheckMs = 500;

void Unstuck(Out& o, float off, const float* rp, DWORD now) {
	o.nearest = off;
	o.since = now;
	o.ax = rp[0];
	o.az = rp[2];
}

bool GivenUp(Out& o, const float* rp, const Vector3& hp, const Vector3* other, float sx, float sz, DWORD now) {
	static int s_told = 0, s_heldTold = 0;
	float ox = sx - rp[0], oz = sz - rp[2], off = sqrtf(ox * ox + oz * oz);
	float hx = hp.x - rp[0], hz = hp.z - rp[2];
	float px = other ? other->x - rp[0] : kBusy, pz = other ? other->z - rp[2] : kBusy;
	bool byHero = hx * hx + hz * hz < kBusy * kBusy || px * px + pz * pz < kBusy * kBusy;
	if (o.stuck) {
		// Diagnostics, once a give-up: held back still a second after it.
		if (!o.heldTold && now - o.stuckAt >= kStuckMs && (*(uint32_t*)(o.roman + 0x24) & 0x4000)) {
			o.heldTold = true;
			if (s_heldTold++ < 20)
				Log("nolock: Roman %p of squad #%u given up, held back still at (%.2f %.2f %.2f), reaction %02X", o.roman,
				    o.sqIdx, rp[0], rp[1], rp[2], *(uint8_t*)(o.roman + 0x1A4));
		}
		float mx = sx - o.gx, mz = sz - o.gz;
		if (!byHero) {
			if (mx * mx + mz * mz < kRetryMove * kRetryMove || now - o.checked < kReachCheckMs) return true;
			o.checked = now;
			Vector3 from(rp[0], rp[1], rp[2]), to(sx, hp.y, sz);
			if (PfReachable(from, to) == PF_NO || PfLineInNodes(from, to) == PF_NO) return true;
		}
		o.stuck = false;
		Unstuck(o, off, rp, now);
		if (s_told++ < 40)
			Log("nolock: Roman %p of squad #%u goes for %s again (%s)", o.roman, o.sqIdx, HeroName(o.hero),
			    byHero ? "a hero by it" : "its spot moved, walkable to");
		return false;
	}
	uint8_t react = *(uint8_t*)(o.roman + 0x1A4);
	bool held = (*(uint32_t*)(o.roman + 0x24) & 0x4000) != 0;
	float wx = rp[0] - o.ax, wz = rp[2] - o.az;
	if (!held || react < 0x22 || off < kStuckOff || off < o.nearest - kStuckGain ||
	    wx * wx + wz * wz >= kStuckMove * kStuckMove || byHero) {
		Unstuck(o, off, rp, now);
		return false;
	}
	if (now - o.since < kStuckMs) return false;
	if (s_told++ < 40)
		Log("nolock: Roman %p of squad #%u at (%.2f %.2f %.2f) held back for %.1f s, %.1f units off its spot (%.1f from %s, "
		    "reaction %02X) -- it stands where it is", o.roman, o.sqIdx, rp[0], rp[1], rp[2], (now - o.since) / 1000.0f, off,
		    sqrtf(hx * hx + hz * hz), HeroName(o.hero), react);
	o.stuck = true;
	o.gx = sx;
	o.gz = sz;
	o.checked = o.stuckAt = now;
	o.heldTold = false;
	return true;
}

// After the squad's tick (vf31 gives each Roman a slot round its formation
// centre: component 0x46, +0 slot, +0x18 the step towards it -- proximity.cpp
// SplitSquad) the slots of its Romans out for a hero are moved round that hero,
// 2.5 units off, spread out: they go after it out of their squad's fight zone
// too, and stay on its side -- within their squad's box (x / z, 0.5 units in):
// the hero out of it, they wait at its edge (the Roman's tick holds one stepping
// out at its last place inside every frame: it walked on the spot there) -- a
// won fight's squad has its box grown over the hero's reach (MoveBox); one
// that can't get to its spot stands where it is, facing as it does (GivenUp).
// Each other one faces the hero (slot
// +0xC, the formation's facing: a squad run for the other hero faces that one --
// LV3-6's ledge squad, run for Asterix up there, had its spear Romans walk after
// Obelix backwards).
void KeepInBox(const float* b, float& x, float& z) {
	x = x < b[0x4A] + 0.5f ? b[0x4A] + 0.5f : x > b[0x47] - 0.5f ? b[0x47] - 0.5f : x;
	z = z < b[0x4C] + 0.5f ? b[0x4C] + 0.5f : z > b[0x49] - 0.5f ? b[0x49] - 0.5f : z;
}

// The squad's tick also copies its choreography key's flags into each slot
// (FUN_0052B670: slot +0x24 bits 0-2 = the key's "enemy attack", bits 3-4 its
// "enemy movement"). A squad's key can be a patrol's -- LV3-6's ledge squad #29
// walks (movement 0) and only attacks in position (attack 0, never goes for the
// hero) -- and its Romans out for a hero came on slowly and never hit it. Ours
// approach and attack (3) and run (1); "attack when in range" (5) and sprint stay.
void NoLockAttackMode(Out& o, float* slot) {
	uint32_t& m = *(uint32_t*)(slot + 9);
	uint32_t was = m, atk = m & 7, mv = (m >> 3) & 3;
	if (atk != 3 && atk != 5) atk = 3;
	if (mv == 0 || mv == 3) mv = 1;
	m = (m & ~0x1Fu) | atk | (mv << 3);
	static int s_told = 0;
	if (m != was && !o.modeTold && s_told < 20) {
		o.modeTold = true;
		s_told++;
		Log("nolock: Roman %p of squad #%u: its squad's key has attack %u movement %u -- made attack %u movement %u",
		    o.roman, o.sqIdx, was & 7, (was >> 3) & 3, atk, mv);
	}
}

void NoLockSteer(char* sq) {
	if (!g_nOut) return;
	Trio t = GetTrio();
	if (!t) return;
	DWORD now = GetTickCount();
	typedef char*(__fastcall* CpntFn)(char* self, void* edx, int id);
	float minStep = *(float*)(sq + 0xC0);
	int n[2] = {}, k[2] = {};
	for (int i = 0; i < g_nOut; i++) if (g_out[i].sq == sq) n[g_out[i].hero]++;
	for (int i = 0; i < g_nOut; i++) {
		Out& o = g_out[i];
		if (o.sq != sq) continue;
		int place = k[o.hero]++;
		Hero h = t.GetHero(o.hero);
		if (!h || !StillOut(o)) continue;   // (a hero down too: they stay on its side for the one who comes to revive it)
		char* node = *(char**)(o.roman + 0x10);
		char* fr = node ? *(char**)(node + 4) : nullptr;
		float* slot = (float*)((CpntFn)(*(void***)o.roman)[12])(o.roman, nullptr, 0x46);
		if (!fr || !slot) continue;
		NoLockAttackMode(o, slot);
		const float* rp = (const float*)(fr + 0x80);
		static const char* s_angle = getenv("XXL_NOLOCKANGLE");   // autotests: one spot for all (crowding)
		float a = s_angle ? (float)atof(s_angle) : 6.2831853f * place / n[o.hero] + 1.3f * o.hero;
		Vector3 hp = h.Pos();
		float sx = hp.x + 2.5f * cosf(a), sz = hp.z + 2.5f * sinf(a);
		const float* b = (const float*)sq;
		static const bool s_noClamp = getenv("XXL_NOLOCKNOCLAMP") != nullptr;   // autotests: Romans held back at the box's edge
		if (!s_noClamp) KeepInBox(b, sx, sz);
		float sy = hp.y;
		Hero oh = t.GetHero(1 - o.hero);
		Vector3 op = oh ? oh.Pos() : Vector3();
		if (GivenUp(o, rp, hp, oh ? &op : nullptr, sx, sz, now)) {
			const float* fw = (const float*)(fr + 0x70);
			float fl = sqrtf(fw[0] * fw[0] + fw[2] * fw[2]);
			slot[0] = rp[0]; slot[1] = rp[1]; slot[2] = rp[2];
			slot[6] = slot[7] = slot[8] = 0.0f;
			if (fl > 0.01f) { slot[3] = fw[0] / fl; slot[4] = 0.0f; slot[5] = fw[2] / fl; }
			continue;
		}
		slot[0] = sx; slot[1] = sy; slot[2] = sz;
		float dx = sx - rp[0], dz = sz - rp[2], len = sqrtf(dx * dx + dz * dz);
		float f = len <= 0.01f ? 0.0f : 1.0f / (len > minStep ? len : minStep);
		slot[6] = dx * f; slot[7] = 0.0f; slot[8] = dz * f;
		float hx = hp.x - rp[0], hz = hp.z - rp[2], hl = sqrtf(hx * hx + hz * hz);
		if (hl < 0.01f) continue;
		hx /= hl; hz /= hl;
		// Diagnostics, once a Roman: its squad's facing more than 90 degrees off the
		// hero (it would walk to it backwards); how it faces the hero (frame +0x70,
		// forward) then.
		static int s_told = 0;
		float sl = sqrtf(slot[3] * slot[3] + slot[5] * slot[5]);
		float sq0 = sl > 0.01f ? (slot[3] * hx + slot[5] * hz) / sl : 1.0f;
		if (!o.faced && sq0 < 0.0f && s_told < 20) {
			o.faced = true;
			s_told++;
			const float* fw = (const float*)(fr + 0x70);
			float fl = sqrtf(fw[0] * fw[0] + fw[2] * fw[2]);
			Log("nolock: Roman %p of squad #%u, %.1f units from %s: its squad's facing %.0f deg off the hero -- turned to it "
			    "(facing %.0f deg off it)", o.roman, o.sqIdx, hl, HeroName(o.hero), acosf(fmaxf(-1.0f, sq0)) * 57.3f,
			    fl > 0.01f ? acosf(fmaxf(-1.0f, fminf(1.0f, (fw[0] * hx + fw[2] * hz) / fl))) * 57.3f : -1.0f);
		}
		slot[3] = hx; slot[4] = 0.0f; slot[5] = hz;
	}
}

bool NoLockEndedFight(char* sq) { return FindEnded(sq) != nullptr; }

bool NoLockKeepsSquad(char* sq) {
	for (int i = 0; i < g_nOut; i++)
		if (g_out[i].sq == sq && StillOut(g_out[i])) return true;
	return false;
}
