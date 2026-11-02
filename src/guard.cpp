#include "guard.h"
#include "config.h"
#include "coop.h"
#include "individual.h"
#include "kal.h"
#include "log.h"
#include "nolock.h"
#include "pathfind.h"
#include "trio.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Romans guard a hero who is down (individual heroes, [coop] romans_guard_downed):
// the basic Romans after it as it fell -- members of the active squads within 10
// units, the nearest 5 -- stay close round it until it is revived, so the partner
// fights them to get to it. The lead goes to the standing hero and the squads run
// for that one: their slots (vf31 -> FUN_0052B670: component 0x46, +0 slot, +0x18
// the step towards it) would take those Romans over to it. After each squad's
// tick their slots are moved to spots round the downed hero, 1.8 units off (0.8
// had them pressing into it, walking all the time), 5 spread out as it falls --
// a spot the level's pathfinding graph can't reach from the hero (a wall, a drop)
// is left out, one guard fewer -- and their squads are not put away meanwhile
// (vf35). They leave the downed hero alone (out of the objects' slicing: HeroOut).
// The standing hero within 5 units of the downed one: the guards go for it (as a
// Roman 2.8 off a guard's spot would) -- their slots 1.5 units off it, on each
// one's side, kept within those 5 units of the downed hero (the squads' own slots
// keep their distance from the hero they run for: they backed off as it came,
// away from the downed hero); 6.5 off, back to their spots. A guard walking that
// has not got 0.2 units nearer its spot for 1 s (a wall, another guard, the
// downed hero's body, a fight lock's dome: a Roman walks on the spot even for a
// small step, or slides back and forth along what it presses on; once one did
// for good) takes the place it stands on as its spot: it stops -- not one with
// the standing hero within 4 units or more than 6.5 units from the downed hero
// (after the standing hero on its own).
// A guard blocks hits with a shield: its hook's component (+0x54, the pool
// record's +4, shared by the pool's Romans) swapped for a copy of its own with
// the shield flag (+4 bit 0x10) and shield points (+0x110) -- the hit code
// (Class_2_84, 0x4CFA30) blocks while the hook's hits in a row (+0x148) are at
// most the points; the next one lands. A guard a hit lands on is "mad": it breaks
// off and fights as usual (its own component back). The last guard left goes for
// the standing hero. Never guards: anything but CKBasicEnemyCpnt (leaders,
// archers, jumping and jetpack Romans, turtles, towers) and models with lion,
// pirate or Norman textures.
namespace {

const int    kPerHero      = 5;
const int    kMaxGuards    = 2 * kPerHero;
const float  kGuardReach   = 10.0f;
const float  kRing         = 1.8f;
const float  kEngage       = 5.0f;    // the standing hero this near the downed one: the guards fight it
const float  kDisengage    = 6.5f;    // ... this far again: back to their spots
const float  kClose        = 1.5f;    // the guards going for the standing hero: their slots this far off it
const float  kStuckGain    = 0.2f;    // a guard walking, not this much nearer its spot in kStuckMs,
const DWORD  kStuckMs      = 1000;
const float  kStuckOff     = 0.1f;    // ... more than this off it: its spot is where it stands
const float  kBusy         = 4.0f;    // ... not with the standing hero this near it (a Roman's range 2.8 and some)
const int    kShieldPoints = 2;       // a guard's shield, for a Roman without one: a 3-hit combo gets through
const size_t kCpntSize     = 0x180;   // CKBasicEnemyCpnt (deserialized up to +0x174)
const uintptr_t kBasicEnemyCpnt = 0x627BD0;

// A guard's component with a shield, its hook's +0x54 while it guards.
struct Shield { alignas(16) char data[kCpntSize]; bool used; };
Shield g_shields[kMaxGuards];

// A guard, in its pool record's list (+8, chained +0x50) and not finished
// (hook +8 bit 0) while it lasts; the squad checked to be the level's still.
// Its spot: (dx, dz) from its hero; the nearest it has been to it (`nearest`)
// since `since`.
struct Guard {
	char* roman; char* sq; unsigned sqIdx; int pool; int hero; char* cpnt; int shield; float dx, dz;
	float nearest; DWORD since;
};
Guard g_guards[kMaxGuards];
int   g_n = 0;
bool  g_down[2] = {};
bool  g_engaged[2] = {};   // the standing hero near: the hero's guards fight it
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

// Its hero still down, in co-op or single player's second chance.
bool OnDuty(const Guard& g) {
	return g_config.romansGuardDowned && (CoopP2Joined() || IndivActive()) && !CoopSuspended() && IndivFallen(g.hero) && Valid(g);
}

bool RomanPos(char* m, Vector3* out) {
	char* node = *(char**)(m + 0x10);
	char* fr = node ? *(char**)(node + 4) : nullptr;
	if (!fr) return false;
	const float* p = (const float*)(fr + 0x80);
	*out = Vector3(p[0], p[1], p[2]);
	return true;
}

template <class T> T* Ptr(const char* p, size_t size) {
	return (uintptr_t)p >= 0x10000 && !IsBadReadPtr(p, size) ? (T*)p : nullptr;
}

// The textures of the Roman's model, each name to `f` until it returns true:
// hook +0x10 its node (CAnimatedClone), node +0x54 its team player; the
// player's +0xD0 the team dictionary (+0xC slots, +0x50 -> +4 its elements, 0x24
// each, +0 an atomic), +0xD4 an element index per slot (-1 none); an atomic's
// +0x18 its geometry: +0x20 its materials, +0x24 their count; a material's +0
// its texture, the name at +0x10 (the level files' names: lion01, pirate01,
// villageois_normand01, romnl01, ...).
template <class F> bool ForEachTexture(char* roman, F f) {
	char* node = *(char**)(roman + 0x10);
	char* pl = Ptr<char>(node, 0x58) ? *(char**)(node + 0x54) : nullptr;
	if (!Ptr<char>(pl, 0xF8)) return false;
	char* dict = *(char**)(pl + 0xD0);
	int* idx = *(int**)(pl + 0xD4);
	if (!Ptr<char>(dict, 0x58)) return false;
	unsigned slots = *(unsigned*)(dict + 0xC);
	char* head = Ptr<char>(*(char**)(dict + 0x50), 8);
	char* elems = head ? *(char**)(head + 4) : nullptr;
	for (unsigned k = 0; k < slots && k < 16 && elems && Ptr<int>((char*)idx, (k + 1) * 4); k++) {
		if (idx[k] < 0 || idx[k] > 4096 || !Ptr<char>(elems + idx[k] * 0x24, 4)) continue;
		char* atomic = Ptr<char>(*(char**)(elems + idx[k] * 0x24), 0x1C);
		char* geo = atomic ? Ptr<char>(*(char**)(atomic + 0x18), 0x28) : nullptr;
		if (!geo) continue;
		char** mats = *(char***)(geo + 0x20);
		int nm = *(int*)(geo + 0x24);
		for (int i = 0; i < nm && i < 16 && Ptr<char*>((char*)mats, (i + 1) * 4); i++) {
			char* mat = Ptr<char>(mats[i], 4);
			char* tex = mat ? Ptr<char>(*(char**)mat, 0x30) : nullptr;
			if (!tex) continue;
			char name[32];
			memcpy(name, tex + 0x10, 31);
			name[31] = 0;
			if (f(name)) return true;
		}
	}
	return false;
}

// A texture of the Roman's model with one of `names` in its name (the first in `found`).
bool ModelHas(char* roman, const char* const* names, int nNames, char found[32]) {
	return ForEachTexture(roman, [&](const char* name) {
		for (int n = 0; n < nNames; n++)
			if (strstr(name, names[n])) { strcpy(found, name); return true; }
		return false;
	});
}

// May this Roman guard: a basic Roman (CKBasicEnemyCpnt) that serves the empire?
bool GuardKind(const char* rec, char* roman) {
	char* cpnt = *(char* const*)(rec + 4);
	static const char* const kNot[] = { "lion", "pirate", "normand" };
	char tex[32] = {};
	bool basic = cpnt && *(uintptr_t*)cpnt == kBasicEnemyCpnt;
	bool ok = basic && !ModelHas(roman, kNot, 3, tex);
	static const char* s_told[16];
	static int s_nTold = 0;
	if (!ok && s_nTold < 16) {
		bool told = false;
		for (int i = 0; i < s_nTold; i++) told |= s_told[i] == rec;
		if (!told) {
			s_told[s_nTold++] = rec;
			Log("guard: Romans of pool record %p never guard (%s%s)", rec, basic ? "model texture " : "component vtable ",
			    basic ? tex : "not CKBasicEnemyCpnt");
		}
	}
	return ok;
}

// The guard's own component with a shield in its hook.
void GiveShield(Guard& g) {
	g.shield = -1;
	char* cur = *(char**)(g.roman + 0x54);
	if (!cur || *(uintptr_t*)cur != kBasicEnemyCpnt) return;
	for (int i = 0; i < kMaxGuards; i++) {
		if (g_shields[i].used) continue;
		char* c = g_shields[i].data;
		memcpy(c, cur, kCpntSize);
		*(char**)(c + 0x60) = c + 0x10;   // its move parameters (the deserialize's own pointer)
		*(uint32_t*)(c + 4) |= 0x10;
		if (!*(uint8_t*)(c + 0x110)) *(uint8_t*)(c + 0x110) = kShieldPoints;
		g_shields[i].used = true;
		g.cpnt = cur;
		g.shield = i;
		*(uint8_t*)(g.roman + 0x148) = 0;   // its hits in a row
		*(char**)(g.roman + 0x54) = c;
		return;
	}
}

// Its own component back (a hook back in its pool has +0x54 0 -- left so).
void TakeShield(Guard& g) {
	if (g.shield < 0) return;
	if (*(char**)(g.roman + 0x54) == g_shields[g.shield].data) *(char**)(g.roman + 0x54) = g.cpnt;
	g_shields[g.shield].used = false;
	g.shield = -1;
}

void Drop(int i) {
	TakeShield(g_guards[i]);
	g_guards[i] = g_guards[--g_n];
}

// Diagnostics: a guard's hit state as it changes (hook +8 bits 1-4 the hit
// pending, +0x21 its state, +0x148 its hits in a row, +0x1A4 / +0x1A5 its
// reaction now / next, +0x24 flags); bounded.
struct Seen { char* roman; uint32_t key; };
Seen g_seen[kMaxGuards];
int g_watchLogs = 0;

void WatchHits() {
	static int s_frames = 0, s_rings = 0;
	static const bool s_trace = getenv("XXL_GUARDTRACE") != nullptr;   // autotests: the line every 15 frames
	Trio t = GetTrio();
	if (g_n && t && ++s_frames % (s_trace ? 15 : 120) == 0 && s_rings < (s_trace ? 400 : 40)) {   // how far the guards are from their hero / spot, [reaction]
		char line[500];
		int len = _snprintf(line, sizeof(line), "guard: distances to the downed hero / spot:");
		for (int i = 0; i < g_n && len > 0 && len < (int)sizeof(line); i++) {
			const Guard& g = g_guards[i];
			Hero h = t.GetHero(g.hero), o = t.GetHero(1 - g.hero);
			Vector3 p;
			if (!h || !RomanPos(g.roman, &p)) continue;
			Vector3 hp = h.Pos();
			float ox = hp.x + g.dx - p.x, oz = hp.z + g.dz - p.z;
			uint8_t react = *(uint8_t*)(g.roman + 0x1A4);
			if (g_engaged[g.hero] && o)
				len += _snprintf(line + len, sizeof(line) - len, " %s %.1f [%02X] (fighting, %.1f from %s)", HeroName(g.hero),
				                 sqrtf((p - hp).len2()), react, sqrtf((p - o.Pos()).len2()), HeroName(1 - g.hero));
			else
				len += _snprintf(line + len, sizeof(line) - len, " %s %.1f/%.1f [%02X]", HeroName(g.hero), sqrtf((p - hp).len2()),
				                 sqrtf(ox * ox + oz * oz), react);
		}
		line[sizeof(line) - 1] = 0;
		Log("%s", line);
		s_rings++;
	}
	for (int i = 0; i < g_n && g_watchLogs < 60; i++) {
		char* r = g_guards[i].roman;
		uint8_t react = *(uint8_t*)(r + 0x1A4) < 0x20 ? *(uint8_t*)(r + 0x1A4) : 0xFF;   // 0x22.. its walk and turns
		uint32_t key = ((*(uint32_t*)(r + 8) >> 1) & 0xF) | *(uint8_t*)(r + 0x21) << 4 | *(uint8_t*)(r + 0x148) << 12 |
		               (uint32_t)react << 20;
		uint32_t f24 = *(uint32_t*)(r + 0x24);
		Seen& s = g_seen[i];
		if (s.roman == r && s.key == key) continue;
		s.roman = r;
		s.key = key;
		g_watchLogs++;
		Log("guard: %p hit %u state %02X hits %u reaction %02X next %02X flags %08X", r, (*(uint32_t*)(r + 8) >> 1) & 0xF,
		    *(uint8_t*)(r + 0x21), *(uint8_t*)(r + 0x148), *(uint8_t*)(r + 0x1A4), *(uint8_t*)(r + 0x1A5), f24);
	}
}

// A hit landed on the guard: in the hit state (+0x21: 2, or 1 stunned, seized,
// knocked flying) with a hurt reaction (+0x1A4 below 0x1B; 0x1B..0x20 are its
// shield's block and cover, 0x22.. its walk and turns). The shield takes hits
// from the front only (the hit kind, hook +8 bits 1-4, 0): one from behind lands.
bool HitLanded(const Guard& g) {
	uint8_t st = *(uint8_t*)(g.roman + 0x21), re = *(uint8_t*)(g.roman + 0x1A4);
	return (st == 1 || st == 2) && re < 0x1B;
}

// The guards' spots round a hero down at `at` ((dx, dz) each): 5 on a ring, those
// the level's pathfinding graph reaches from it (no graph here: all). Their count.
int Spots(const Vector3& at, float dx[kPerHero], float dz[kPerHero], bool* graph) {
	int ns = 0;
	static const float s_ring = getenv("XXL_GUARDRING") ? (float)atof(getenv("XXL_GUARDRING")) : kRing;   // autotests: round 170's 0.8
	*graph = PfReachable(at, at) != PF_UNKNOWN;
	for (int s = 0; s < kPerHero; s++) {
		float a = 6.2831853f * s / kPerHero + 0.7f;
		Vector3 p(at.x + s_ring * cosf(a), at.y, at.z + s_ring * sinf(a));
		if (*graph && PfReachable(at, p) != PF_YES) continue;
		dx[ns] = p.x - at.x;
		dz[ns++] = p.z - at.z;
	}
	return ns;
}

void Collect(int hero, const Vector3& at) {
	struct Cand { char* m; char* sq; unsigned i; int r; float d; };
	Cand c[32];
	int nc = 0;
	for (unsigned i = 0, n = GetNumLvlClassInstances(4, 26); i < n; i++) {
		char* sq = (char*)GetLvlObject(4, 26, i);
		if (!sq || !(*(uint8_t*)(sq + 0x152) & 2)) continue;
		char* recs = *(char**)(sq + 0x144);
		unsigned nr = *(unsigned*)(sq + 0x14C);
		for (unsigned r = 0; recs && r < nr && r < 8; r++)
			for (char* m = *(char**)(recs + 0x1C * r + 8); m && nc < 32; m = *(char**)(m + 0x50)) {
				Vector3 p;
				if ((*(uint8_t*)(m + 8) & 1) || !RomanPos(m, &p)) continue;
				float d = (p - at).len2();
				if (d > kGuardReach * kGuardReach) continue;
				bool have = false;
				for (int k = 0; k < g_n; k++) have |= g_guards[k].roman == m;
				if (have || !GuardKind(recs + 0x1C * r, m)) continue;
				c[nc++] = { m, sq, i, (int)r, d };
			}
	}
	for (int a = 1; a < nc; a++)   // nearest first
		for (int b = a; b > 0 && c[b].d < c[b - 1].d; b--) { Cand t = c[b]; c[b] = c[b - 1]; c[b - 1] = t; }
	float sx[kPerHero], sz[kPerHero];
	bool open[kPerHero], graph;
	int ns = Spots(at, sx, sz, &graph);
	for (int s = 0; s < ns; s++) open[s] = true;
	static const int s_max = getenv("XXL_GUARDMAX") ? atoi(getenv("XXL_GUARDMAX")) : kPerHero;   // autotests: fewer
	int take = nc < s_max ? nc : s_max;
	if (take > ns) take = ns;
	if (take < 2) take = 0;   // a lone guard goes for the standing hero (the last guard's rule)
	for (int k = 0; k < take && g_n < kMaxGuards; k++) {
		Vector3 p;   // the free spot nearest it
		RomanPos(c[k].m, &p);
		int best = -1;
		float bd = 1e30f;
		for (int s = 0; s < ns; s++) {
			float ex = at.x + sx[s] - p.x, ez = at.z + sz[s] - p.z;
			if (open[s] && ex * ex + ez * ez < bd) { bd = ex * ex + ez * ez; best = s; }
		}
		open[best] = false;
		Guard& g = g_guards[g_n++];
		g = { c[k].m, c[k].sq, c[k].i, c[k].r, hero, nullptr, -1, sx[best], sz[best], 1e30f, GetTickCount() };
		GiveShield(g);
	}
	g_engaged[hero] = false;
	if (g_told++ < 40)
		Log("guard: %s is down -- %d basic Roman(s) within %.0f units, %d of %d spots round it reachable%s: %d stay there "
		    "with a shield until it is revived", HeroName(hero), nc, kGuardReach, ns, kPerHero, graph ? "" : " (no graph)", take);
}

// The standing hero near the downed one: its guards fight it; away again: back
// to their spots.
void Engage(Trio t, int hero) {
	Hero h = t.GetHero(hero), o = t.GetHero(1 - hero);
	bool any = false;
	for (int i = 0; i < g_n; i++) any |= g_guards[i].hero == hero;
	if (!h || !o || !any) { g_engaged[hero] = false; return; }
	float d = sqrtf((o.Pos() - h.Pos()).len2());
	bool on = g_engaged[hero] ? d < kDisengage : d < kEngage;
	if (on == g_engaged[hero]) return;
	g_engaged[hero] = on;
	static int s_told = 0;
	if (s_told++ < 60)
		Log("guard: %s %.1f units from downed %s -- its guards %s", HeroName(1 - hero), d, HeroName(hero),
		    on ? "fight it" : "go back to their spots");
}

// A guard at its spot (0.1 units off), getting nearer it, going for the standing
// hero or reacting to a hit (+0x1A4 below 0x22: hurt, blocking, covering) is
// where it should be; one walking (0x24 on) that has not got 0.2 units nearer
// in 1 s takes the place it stands on as its spot. Standing a moment between
// does not count: the user's LV4-1 guard pressed on downed Asterix's body walked
// back and forth for good (round 173's rule took one 0.4 off or less as at its
// spot). Not one with the standing hero within 4 units or more than 6.5 units
// from the downed hero: it is after the standing hero (a Roman goes for a hero
// near it on its own; walking on the spot facing it) -- one had its spot moved
// after it, 8 units off; guard41a's stopped 4.3 off, the standing hero 7.5 off.
void Unstick(Trio t, DWORD now) {
	static int s_told = 0;
	for (int i = 0; i < g_n; i++) {
		Guard& g = g_guards[i];
		Hero h = t.GetHero(g.hero), o = t.GetHero(1 - g.hero);
		Vector3 p;
		if (!h || !RomanPos(g.roman, &p)) continue;
		Vector3 hp = h.Pos();
		float ox = hp.x + g.dx - p.x, oz = hp.z + g.dz - p.z, off = sqrtf(ox * ox + oz * oz);
		uint8_t react = *(uint8_t*)(g.roman + 0x1A4);
		if (g_engaged[g.hero] || react < 0x22 || off < kStuckOff || off < g.nearest - kStuckGain ||
		    (p - hp).len2() > kDisengage * kDisengage || (o && (p - o.Pos()).len2() < kBusy * kBusy)) {
			g.nearest = off;
			g.since = now;
			continue;
		}
		if (react < 0x24 || now - g.since < kStuckMs) continue;
		if (s_told++ < 30)
			Log("guard: %p walking, not nearer its spot for %.0f s, %.1f units off it (%.1f from %s, reaction %02X) -- its spot "
			    "is where it stands", g.roman, (now - g.since) / 1000.0f, off, sqrtf((p - hp).len2()), HeroName(g.hero), react);
		g.dx = p.x - hp.x; g.dz = p.z - hp.z;
		g.nearest = 0.0f;
		g.since = now;
	}
}

void Release(int hero) {
	int n = 0;
	for (int i = 0; i < g_n;) {
		if (g_guards[i].hero != hero) { i++; continue; }
		Drop(i);
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
		else Drop(i);
	}
	WatchHits();
	for (int i = 0; i < g_n;) {   // a hit landed: mad, it fights as usual
		if (!HitLanded(g_guards[i])) { i++; continue; }
		if (g_told++ < 40)
			Log("guard: a hit landed on %p (state %02X, reaction %02X, hits in a row %u) -- it breaks off from %s and fights",
			    g_guards[i].roman, *(uint8_t*)(g_guards[i].roman + 0x21), *(uint8_t*)(g_guards[i].roman + 0x1A4),
			    *(uint8_t*)(g_guards[i].roman + 0x148), HeroName(g_guards[i].hero));
		Drop(i);
	}
	Trio t = GetTrio();
	bool on = t && g_config.romansGuardDowned && (CoopP2Joined() || IndivActive()) && !CoopSuspended() && KalLevelIsLoaded();
	for (int hero = HERO_ASTERIX; hero <= HERO_OBELIX; hero++) {
		Hero h = on ? t.GetHero(hero) : Hero();
		bool down = h && IndivFallen(hero);
		if (down && !g_down[hero]) Collect(hero, h.Pos());
		else if (!down && g_down[hero]) Release(hero);
		g_down[hero] = down;
		int left = 0, last = -1;
		for (int i = 0; i < g_n; i++)
			if (g_guards[i].hero == hero) { left++; last = i; }
		if (left == 1) {   // the last guard goes for the standing hero
			if (g_told++ < 40)
				Log("guard: %p is the last guard of %s -- it goes for %s", g_guards[last].roman, HeroName(hero),
				    HeroName(1 - hero));
			Drop(last);
		}
		if (on) Engage(t, hero);
	}
	if (on) Unstick(t, GetTickCount());
}

void GuardSteer(char* sq) {
	if (!g_n) return;
	Trio t = GetTrio();
	if (!t) return;
	typedef char*(__fastcall* CpntFn)(char* self, void* edx, int id);
	float minStep = *(float*)(sq + 0xC0);
	for (int i = 0; i < g_n; i++) {
		const Guard& g = g_guards[i];
		if (g.sq != sq || !OnDuty(g)) continue;
		Hero h = t.GetHero(g.hero), o = t.GetHero(1 - g.hero);
		char* node = *(char**)(g.roman + 0x10);
		char* fr = node ? *(char**)(node + 4) : nullptr;
		float* slot = (float*)((CpntFn)(*(void***)g.roman)[12])(g.roman, nullptr, 0x46);
		if (!h || !fr || !slot) continue;
		const float* rp = (const float*)(fr + 0x80);
		Vector3 hp = h.Pos();
		float sx = hp.x + g.dx, sy = hp.y, sz = hp.z + g.dz;
		if (g_engaged[g.hero] && o) {   // by the standing hero on its side, within kEngage of the downed one
			Vector3 op = o.Pos();
			float ax = rp[0] - op.x, az = rp[2] - op.z, al = sqrtf(ax * ax + az * az);
			if (al < 0.01f) { ax = g.dx; az = g.dz; al = sqrtf(ax * ax + az * az); }
			sx = op.x + kClose * ax / al; sy = op.y; sz = op.z + kClose * az / al;
			float cx = sx - hp.x, cz = sz - hp.z, cl = sqrtf(cx * cx + cz * cz);
			if (cl > kEngage) { sx = hp.x + cx * kEngage / cl; sz = hp.z + cz * kEngage / cl; }
		}
		slot[0] = sx; slot[1] = sy; slot[2] = sz;
		float dx = sx - rp[0], dz = sz - rp[2], len = sqrtf(dx * dx + dz * dz);
		float f = len <= 0.01f ? 0.0f : 1.0f / (len > minStep ? len : minStep);
		slot[6] = dx * f; slot[7] = 0.0f; slot[8] = dz * f;
	}
}

bool GuardEligible(const char* rec, char* roman) { return GuardKind(rec, roman); }

int GuardSpots(const Vector3& at, char* out, int room) {
	float dx[kPerHero], dz[kPerHero];
	bool graph;
	int ns = Spots(at, dx, dz, &graph);
	int len = _snprintf(out, room, "%d of %d reachable%s:", ns, kPerHero, graph ? "" : " (no graph)");
	for (int s = 0; s < ns && len > 0 && len < room; s++)
		len += _snprintf(out + len, room - len, " (%.1f %.1f)", at.x + dx[s], at.z + dz[s]);
	out[room - 1] = 0;
	return ns;
}

void GuardModelTextures(char* roman, char* out, int room) {
	int len = 0;
	out[0] = 0;
	ForEachTexture(roman, [&](const char* name) {
		int n = _snprintf(out + len, room - len, " %s", name);
		if (n < 0 || len + n >= room) return true;
		len += n;
		return false;
	});
	out[room - 1] = 0;
}

bool GuardKeepsSquad(char* sq) {
	for (int i = 0; i < g_n; i++)
		if (g_guards[i].sq == sq && OnDuty(g_guards[i])) return true;
	return false;
}
