#include "individual.h"
#include "proximity.h"
#include "coop.h"
#include "config.h"
#include "detour.h"
#include "hints.h"
#include "kal.h"
#include "life.h"
#include "log.h"
#include "pathfind.h"
#include "trio.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// One hero's share of what the game keeps once.
struct Stats {
	uint8_t  hp = 15, shields = 3;     // game manager +0x40 -> +0x50, +0x51
	float    gauge = 0, decay = 0;     // game manager +0xA4, +0xA8
	uint32_t hits = 0, chain = 0, c4 = 0;   // +0xB0, +0xB4, +0xC4
	uint32_t level = 0;                // +0xCC bits 9-10
	bool     full = false;             // +0x40 -> +0x10 bit 0x4000
	// +0x160 bits 2 and 4: a hit filled the full gauge, a combo spent from it --
	// the combo bar's "full" state (FUN_00518190 state 4) takes them. Kept with
	// one hero, the other's bar took them and the hero's own never saw them.
	uint8_t  barFlags = 0;
};
const uint8_t kBarFlags = 0x14;

struct SafeSpot { Vector3 pos; DWORD at = 0; };
const int kSafeSpots = 8;

// (The grab's hold time: [coop] revive_hold_seconds, under catapult fire
// revive_hold_catapult_window -- HoldSeconds.)
const float kReviveRange = 3.0f;      // horizontally
const float kReviveHeight = 2.5f;
// A hero's walk, units a second (user: 115.4 units in 17 s; an autotest's
// Obelix up a slight slope: ~6.1), for the revive time limit's "auto": the walk
// to the fallen hero over the level's pathfinding, then revive_time_limit_grace.
const float kWalkSpeed = 115.36f / 17.0f;

// The way a hero walked (a spot every 3 units on solid ground, 64 of them), with
// the sector loaded then: where the hero cut off from its fallen leader may be
// brought to (CatchUpSpot).
struct PathSpot { Vector3 pos; int sector = -1; };
const int kPathSpots = 64;
// A revive time run out: the scope IndivEnter opens for the hero still
// standing (its own health in the game's place for its own business only).
const int kSpareScope = 2;

struct State {
	bool     active = false;
	bool     solo = false;             // single player's second chance ([single_player] sp_second_chance)
	int      level = -2;
	Stats    st[2];
	int      ctx = -1;                 // whose stats are in the game's place (-1: the game's own)
	int      depth = 0;                // IndivEnter scopes open (a hero's tick, event, pickup...)
	bool     fallen[2] = {};
	bool     drain[2] = {};            // its combo bar draining to empty (DrainCombo)
	float    revive[2] = {};           // seconds the other player has held the grab button by it
	bool     gameOver = false;
	SafeSpot safe[2][kSafeSpots];
	int      safeNext[2] = {};
	// A longer trail of safe spots, one every 2 units walked (the put-back from
	// what needs both heroes: far enough not to be walked back onto at once).
	Vector3  trail[2][16];
	int      trailN[2] = {}, trailNext[2] = {};
	PathSpot path[2][kPathSpots];
	int      pathN[2] = {}, pathNext[2] = {};
	// Where the hero was lately, whatever it stood on (every 0.1 s, 2 s): the
	// last resort of a put-back.
	Vector3  recent[2][20];
	int      recentN[2] = {}, recentNext[2] = {};
	DWORD    recentAt = 0;
	Vector3  frameStart[2];   // each hero's place when the frame began
	DWORD    noSafeUntil[2] = {};
	DWORD    placedAt[2] = {};      // last put somewhere by this file
	DWORD    onLauncherAt[2] = {};  // last seen where a launcher offers itself
	DWORD    blockedAt[2] = {};     // last put back from something that needs both heroes
	Vector3  blockedTo[2];          // ... to there   // just put back: no new safe spot (it would be the lifted place)
	int      lastSector = -1;
	int      lastLead = -1;            // the trio's leader as the frame began
	bool     fellLeading[2] = {};      // it was the leader when it fell
	bool     levelLead[2] = {};        // down, the level gave the other hero the lead: none back after the revive
	int      leadBack = -1;            // revived, it was leading: the lead goes back to it
	// The hero brought to a fallen leader (BringToFallenLeader): where from, where to.
	bool     brought[2] = {};
	bool     broughtStays[2] = {};     // ... past a lifter shut while the other was down, or having cut itself off: it stays
	bool     hadWay[2] = {};           // down: the other hero could walk to it at some time since the fall
	Vector3  broughtFrom[2], broughtTo[2];
	int      sendBack = -1;            // revived its leader: that hero goes back where it came from
	bool     offGround[2] = {};        // down where the graph has walls only (WayToFallen)
	int      parkRevived = -1;         // single player: revived -- the companion's kept wait back once it leads
	DWORD    parkRevivedAt = 0;
	DWORD    fellAt[2] = {};
	DWORD    reachAt[2] = {};          // last asked whether the other hero can walk to it
	int      cutOff[2] = {};           // ... and was told no that many times in a row
	int      reachTold[2] = { -2, -2 };
	// The revive time limit ([coop] revive_time_limit): seconds left, counted
	// while the other hero is known to have a way to it.
	bool     limitSet[2] = {};
	float    limitLeft[2] = {};
	DWORD    limitHintAt = 0;
	// It ran out: the hero still standing (its own health kept for its own
	// business through the game over -- not a second death), the game's place
	// while it is in.
	int      spared = -1;
	bool     sparing = false;
	uint8_t  sparedPlace = 0;
	DWORD    lastFrame = 0;
	DWORD    hintAt = 0;
	int      p2Hero = -1;              // player 2's hero while individual heroes are on
	// Player 2 left: their hero's stats, given back if they come in again in the
	// same level (leaving and joining again is no free heal).
	bool     kept = false;
	int      keptHero = -1, keptLevel = -2;
	Stats    keptStats;
	bool     keptFallen = false;
} g;

char* GM() {
	CKYellowPages* yp = YellowPages();
	return yp ? (char*)yp->gameManager : nullptr;
}
char* CB() {
	char* gm = GM();
	return gm ? *(char**)(gm + 0x40) : nullptr;
}

bool Read(Stats& s) {
	char* gm = GM();
	char* cb = CB();
	if (!gm || !cb) return false;
	s.hp = *(uint8_t*)(cb + 0x50);
	s.shields = *(uint8_t*)(cb + 0x51);
	s.gauge = *(float*)(gm + 0xA4);
	s.decay = *(float*)(gm + 0xA8);
	s.hits = *(uint32_t*)(gm + 0xB0);
	s.chain = *(uint32_t*)(gm + 0xB4);
	s.c4 = *(uint32_t*)(gm + 0xC4);
	s.level = *(uint32_t*)(gm + 0xCC) & 0x600;
	s.full = (*(uint32_t*)(cb + 0x10) & 0x4000) != 0;
	s.barFlags = *(uint8_t*)(gm + 0x160) & kBarFlags;
	return true;
}

void Write(const Stats& s) {
	char* gm = GM();
	char* cb = CB();
	if (!gm || !cb) return;
	*(uint8_t*)(cb + 0x50) = s.hp;
	*(uint8_t*)(cb + 0x51) = s.shields;
	*(float*)(gm + 0xA4) = s.gauge;
	*(float*)(gm + 0xA8) = s.decay;
	*(uint32_t*)(gm + 0xB0) = s.hits;
	*(uint32_t*)(gm + 0xB4) = s.chain;
	*(uint32_t*)(gm + 0xC4) = s.c4;
	*(uint32_t*)(gm + 0xCC) = (*(uint32_t*)(gm + 0xCC) & ~0x600u) | s.level;
	*(uint32_t*)(cb + 0x10) = (*(uint32_t*)(cb + 0x10) & ~0x4000u) | (s.full ? 0x4000u : 0u);
	*(uint8_t*)(gm + 0x160) = (*(uint8_t*)(gm + 0x160) & ~kBarFlags) | s.barFlags;
}

void Use(int idx) {
	if (idx < 0 || idx > 1 || idx == g.ctx) return;
	if (g.ctx >= 0) Read(g.st[g.ctx]);
	Write(g.st[idx]);
	g.ctx = idx;
}

// The stats of `idx` as they are now (in the game's place or in the copy).
Stats& Current(int idx) {
	if (idx == g.ctx) Read(g.st[idx]);
	return g.st[idx];
}
void Commit(int idx) {
	if (idx == g.ctx) Write(g.st[idx]);
}

int Other(int idx) { return idx == HERO_ASTERIX ? HERO_OBELIX : HERO_ASTERIX; }
int PlayerOf(int hero) { return !CoopP2Joined() || hero == CoopP1Hero() ? 0 : 1; }

// A put-back is a jump, not a walk: the node's world frame (+0x80, what the
// detection service tests) and the detection service's last position move
// along, so it crosses no zone (as coop.cpp's SetHeroPosition).
void PlaceHero(Hero h, const Vector3& p) {
	h.Pos() = p;
	if (h.hook->node && h.hook->node->matrix) {
		float* m = (float*)((char*)h.hook->node->matrix + 0x40);
		m[0] = p.x; m[1] = p.y; m[2] = p.z;
		m = (float*)((char*)h.hook->node->matrix + 0x80);
		m[0] = p.x; m[1] = p.y; m[2] = p.z;
	}
	if (h.hook->node) ProximityForgetMove(h.hook->node, &p.x);
}

// Out of the death (situation 0x0E), which the situation chooser never leaves
// by itself (only a respawn did): the state's exit, then "no situation", from
// which the chooser starts afresh on the hero's next tick (FUN_0049A460).
void Raise(Hero h) {
	uint8_t* sit = (uint8_t*)(h.raw() + 0x3E9);
	if (*sit != 0x0E) return;
	typedef char(__fastcall* SitFn)(void* self, void* edx, int mode, int other);
	void* owner = *(void**)(h.raw() + 0x3E0);
	SitFn* table = *(SitFn**)(h.raw() + 0x3E4);
	if (owner && table) table[0x0E](owner, nullptr, 2, 0xFF);
	*sit = 0xFF;
}

// A death pit: the hero touches ground whose flags (+0x2A) have bit 2.
bool OnDeathGround(Hero h) {
	char* ground = *(char**)(h.raw() + 0x280);
	return ground && !IsBadReadPtr(ground, 0x2C) && (*(uint8_t*)(ground + 0x2A) & 4);
}

bool OnPartnerGround(Hero h, const char** what);
bool SafeSpotAway(int idx, const Vector3& from, Vector3* out);

// The level's sector loaded now (level +0x24), -1 none.
int LoadedSector() {
	CKYellowPages* yp = YellowPages();
	char* lvl = yp ? *(char**)((char*)yp + 0x90) : nullptr;
	return lvl ? *(int*)(lvl + 0x24) : -1;
}

// Where a hero stood last on solid ground: a static ground (CGround, not a
// lifter's CDynamicGround), not a death pit, on its feet (+0x384 bits 0-2).
void NoteSafeSpot(int idx, Hero h, DWORD now) {
	if ((int)(g.noSafeUntil[idx] - now) > 0 || now - g.onLauncherAt[idx] < 500) return;
	if (now - g.safe[idx][(g.safeNext[idx] + kSafeSpots - 1) % kSafeSpots].at < 250) return;
	char* ground = *(char**)(h.raw() + 0x280);
	if (!ground || IsBadReadPtr(ground, 0x2C) || *(uintptr_t*)ground != 0x624258) return;
	if ((*(uint8_t*)(ground + 0x2A) & 4) || (*(uint8_t*)(h.raw() + 0x384) & 7) != 0) return;
	// in no situation but walking (0), or the AI companion's none (0xFF: one that
	// only ever followed had no spot since the level's start -- lv2-6_tp_back.log)
	uint8_t sit = *(uint8_t*)(h.raw() + 0x3E9);
	if (sit != 0 && !(sit == 0xFF && (*(uint8_t*)(h.raw() + 0x18) & 3) == 3)) return;
	if (*(uint8_t*)(ground + 0x2A) & 8) return;   // slide ground (the belly slide)
	const char* partner = nullptr;
	if (OnPartnerGround(h, &partner)) return;   // a blue circle's plate: not a place to be put back on
	SafeSpot& s = g.safe[idx][g.safeNext[idx]];
	s.pos = h.Pos();
	s.at = now;
	g.safeNext[idx] = (g.safeNext[idx] + 1) % kSafeSpots;
	int last = (g.trailNext[idx] + 15) % 16;
	if (g.trailN[idx] == 0 || (g.trail[idx][last] - s.pos).len2() >= 2.0f * 2.0f) {
		g.trail[idx][g.trailNext[idx]] = s.pos;
		g.trailNext[idx] = (g.trailNext[idx] + 1) % 16;
		if (g.trailN[idx] < 16) g.trailN[idx]++;
	}
	last = (g.pathNext[idx] + kPathSpots - 1) % kPathSpots;
	if (g.pathN[idx] == 0 || (g.path[idx][last].pos - s.pos).len2() >= 3.0f * 3.0f) {
		g.path[idx][g.pathNext[idx]].pos = s.pos;
		g.path[idx][g.pathNext[idx]].sector = LoadedSector();
		g.pathNext[idx] = (g.pathNext[idx] + 1) % kPathSpots;
		if (g.pathN[idx] < kPathSpots) g.pathN[idx]++;
	}
}

// Put back from something that needs both heroes: the newest safe spot at
// least 4 units from where the hero stands (the last one can be right at the
// thing's edge, walked back onto at once), else the farthest one known.
// Spots further than this are not "the last solid ground" any more (left from
// an earlier part of the level): not gone back to.
const float kSpotReach = 40.0f;

bool SafeSpotAway(int idx, const Vector3& from, Vector3* out) {
	// The trail first: the newest of its spots 6 units or more away.
	for (int k = 1; k <= g.trailN[idx]; k++) {
		const Vector3& p = g.trail[idx][(g.trailNext[idx] + 16 - k) % 16];
		float d = (p - from).len2();
		if (d >= 6.0f * 6.0f && d <= kSpotReach * kSpotReach) { *out = p; return true; }
	}
	const SafeSpot* best = nullptr;
	const SafeSpot* farthest = nullptr;
	float farD = -1.0f;
	for (int i = 0; i < kSafeSpots; i++) {
		const SafeSpot& s = g.safe[idx][i];
		if (!s.at) continue;
		float d = (s.pos - from).len2();
		if (d > kSpotReach * kSpotReach) continue;
		if (d > farD) { farD = d; farthest = &s; }
		if (d >= 4.0f * 4.0f && (!best || (int)(s.at - best->at) > 0)) best = &s;
	}
	if (!best) best = farthest;
	if (!best) {
		// No safe spot known (just teleported): where it was a second ago or more.
		for (int k = 10; k <= g.recentN[idx]; k++) {
			const Vector3& p = g.recent[idx][(g.recentNext[idx] + 20 - k) % 20];
			if ((p - from).len2() >= 2.0f * 2.0f) { *out = p; return true; }
		}
		return false;
	}
	*out = best->pos;
	return true;
}

// The newest safe spot at least 0.7 s old (the edge it jumped from is too near
// the pit), else the oldest one known.
bool SafeSpotFor(int idx, Vector3* out) {
	DWORD now = GetTickCount();
	const SafeSpot* best = nullptr;
	const SafeSpot* oldest = nullptr;
	for (int i = 0; i < kSafeSpots; i++) {
		const SafeSpot& s = g.safe[idx][i];
		if (!s.at) continue;
		if (!oldest || (int)(s.at - oldest->at) < 0) oldest = &s;
		if (now - s.at >= 700 && (!best || (int)(s.at - best->at) > 0)) best = &s;
	}
	if (!best) best = oldest;
	if (!best) return false;
	*out = best->pos;
	return true;
}

} // namespace

void IndivNoteLauncherSpot(int hero) {
	if (hero == HERO_ASTERIX || hero == HERO_OBELIX) g.onLauncherAt[hero] = GetTickCount();
}

int IndivFallenHero() {
	if (!g.active || g.gameOver) return -1;
	if (g.fallen[HERO_ASTERIX] != g.fallen[HERO_OBELIX]) return g.fallen[HERO_ASTERIX] ? HERO_ASTERIX : HERO_OBELIX;
	return -1;
}

namespace { void ShowDownHint(int down); }

namespace {
void ForgetRecentSpots(int idx, DWORD now) {
	for (int i = 0; i < kSafeSpots; i++)
		if (g.safe[idx][i].at && now - g.safe[idx][i].at < 500) g.safe[idx][i].at = 0;
	// the trail's newest point, if it was added in that time, goes too
	if (g.trailN[idx] > 0 && now - g.safe[idx][(g.safeNext[idx] + kSafeSpots - 1) % kSafeSpots].at < 500) {
		g.trailNext[idx] = (g.trailNext[idx] + 15) % 16;
		g.trailN[idx]--;
	}
}

void PutBackLog(int up, int down, const char* what, const Vector3& spot) {
	static DWORD s_told = 0;
	DWORD now = GetTickCount();
	if (now - s_told > 2000) {
		s_told = now;
		Log("indiv: %s reached %s with %s down -- back on its last solid ground (%.1f %.1f %.1f)", HeroName(up), what,
		    HeroName(down), spot.x, spot.y, spot.z);
		ShowDownHint(down);
	}
}
}

bool IndivBlockPartnerSpotFrom(const char* what, const float beforeXyz[3]) {
	Vector3 before(beforeXyz[0], beforeXyz[1], beforeXyz[2]);
	int down = IndivFallenHero();
	if (down < 0 || CoopSuspended()) return false;
	int up = Other(down);
	Trio t = GetTrio();
	Hero h = t ? t.GetHero(up) : Hero();
	if (!h) return false;
	Vector3 now = h.Pos();
	// Off its feet (+0x384 bits 0-2: a jump, a fall, down a steep slope), the
	// frame before is in the air: a step back from it at its height went into the
	// slope it came down -- LV2-6, jumped from (48.8 13.2 -389.4) onto the slide's
	// ramp and through zone 0CA8, put at (44.4 8.3 -387.4) and stuck in the rock
	// for good (xxl_coop.lv2-6_stuck.log, zonejump26j). Its last solid ground then.
	if ((*(uint8_t*)(h.raw() + 0x384) & 7) != 0) {
		static int s_told = 0;
		if (s_told++ < 20) Log("indiv: %s came into %s off its feet -- its last solid ground", HeroName(up), what);
		return IndivBlockPartnerSpot(what);
	}
	Vector3 back = before - now;
	back.y = 0.0f;
	float len = std::sqrt(back.len2());
	if (len > 8.0f) {   // not a frame's step: stale (a teleport) -- the trail
		Log("indiv: %s's place before %s is %.0f units back -- the trail", HeroName(up), what, len);
		return IndivBlockPartnerSpot(what);
	}
	if (len < 0.05f && GetTickCount() - g.placedAt[up] < 1000) return true;   // just put here by us: stays
	if (len < 0.05f) {   // not moving in (already there): the trail
		Log("indiv: %s did not move into %s (%.1f %.1f %.1f) -- the trail", HeroName(up), what, now.x, now.y, now.z);
		return IndivBlockPartnerSpot(what);
	}
	Vector3 spot = before;
	if (len > 0.001f) spot = before + back * (1.5f / len);   // a step further back than it came from
	spot.y = before.y + 0.3f;
	ForgetRecentSpots(up, GetTickCount());
	PlaceHero(h, spot);
	g.placedAt[up] = GetTickCount();
	g.noSafeUntil[up] = GetTickCount() + 1000;
	PutBackLog(up, down, what, spot);
	return true;
}

void IndivPutOnSafeGround(int hero) {
	Trio t = GetTrio();
	Hero h = t ? t.GetHero(hero) : Hero();
	Vector3 spot;
	if (!h || !SafeSpotAway(hero, h.Pos(), &spot)) return;
	spot.y += 0.5f;
	PlaceHero(h, spot);
	g.placedAt[hero] = GetTickCount();
	Log("indiv: %s put on its last solid ground (%.1f %.1f %.1f)", HeroName(hero), spot.x, spot.y, spot.z);
}

bool IndivBlockTalk(const char* what) {
	int down = IndivFallenHero();
	if (down < 0 || CoopSuspended()) return false;
	static DWORD s_told = 0;
	DWORD now = GetTickCount();
	if (now - s_told > 2000) {
		s_told = now;
		Log("indiv: %s reached %s with %s down -- it waits (no put-back)", HeroName(Other(down)), what, HeroName(down));
		ShowDownHint(down);
	}
	return true;
}

void IndivNoteLevelLead(int hero) {
	int down = IndivFallenHero();
	if (down < 0 || (hero != HERO_ASTERIX && hero != HERO_OBELIX)) return;
	// (fellLeading stays: a fallen leader out of reach still gets the other brought to it)
	if (g.levelLead[down] == (hero != down)) return;
	g.levelLead[down] = hero != down;
	Log("indiv: the level gave the lead to %s with %s down -- %s leads after the revive", HeroName(hero),
	    HeroName(down), HeroName(hero));
}

bool IndivBlockPartnerSpot(const char* what) {
	int down = IndivFallenHero();
	if (down < 0 || CoopSuspended()) return false;
	int up = Other(down);
	Trio t = GetTrio();
	Hero h = t ? t.GetHero(up) : Hero();
	Vector3 spot;
	if (!h || !SafeSpotAway(up, h.Pos(), &spot)) return false;
	spot.y += 0.5f;
	PlaceHero(h, spot);
	g.placedAt[up] = GetTickCount();
	g.noSafeUntil[up] = GetTickCount() + 1000;
	static DWORD s_told = 0;
	DWORD now = GetTickCount();
	if (now - s_told > 2000) {
		s_told = now;
		Log("indiv: %s reached %s with %s down -- back on its last solid ground (%.1f %.1f %.1f)", HeroName(up), what,
		    HeroName(down), spot.x, spot.y, spot.z);
		ShowDownHint(down);
	}
	return true;
}

namespace {

// The lifter (2,47) whose own moving ground (a field of it pointing at a
// CDynamicGround) the fallen hero lies on, else null. That lifter is no partner
// spot (user): the other hero may come onto it -- the fallen hero waits there
// for it, or it was on it already -- and revive it; the lifter goes on as with
// both heroes on it (IndivHoldsBack), the fallen one riding along.
char* LifterUnderFallen() {
	int down = IndivFallenHero();
	Trio t = GetTrio();
	Hero f = down >= 0 && t ? t.GetHero(down) : Hero();
	char* ground = f ? *(char**)(f.raw() + 0x280) : nullptr;
	if (!ground || IsBadReadPtr(ground, 4) || *(uintptr_t*)ground != 0x6241BC) return nullptr;
	for (unsigned i = 0, n = GetNumLvlClassInstances(2, 47); i < n; i++) {
		char* o = (char*)GetLvlObject(2, 47, i);
		if (!o || IsBadReadPtr(o, 0x100)) continue;
		for (int off = 0x10; off < 0x100; off += 4)
			if (*(char**)(o + off) == ground) return o;
	}
	return nullptr;
}

// The grounds of what needs both heroes: lifters (2,47), the spinning platform
// (CKHkCorkscrew 2,44), cable cars (CKHkTelepher 2,158, CKHkTowedTelepher
// 2,159) -- any field of theirs pointing at the ground the hero stands on --,
// and blue circles (pressure plates 2,21 whose events command the trio: ground
// +0x28).
bool OnPartnerGround(Hero h, const char** what) {
	char* ground = *(char**)(h.raw() + 0x280);
	if (!ground) return false;
	static const bool logGrounds = getenv("XXL_GROUNDLOG") != nullptr;
	if (logGrounds) {
		static DWORD s_last = 0;
		if (GetTickCount() - s_last > 1000) {
			s_last = GetTickCount();
			for (unsigned i = 0, n = GetNumLvlClassInstances(2, 47); i < n; i++) {
				char* o = (char*)GetLvlObject(2, 47, i);
				if (o) Log("indiv: hero ground %p; lifter %u %p +50 %p +54 %p", ground, i, o, *(void**)(o + 0x50), *(void**)(o + 0x54));
			}
		}
	}
	static const struct { int id; const char* what; } kinds[] = {
		{ 47, "a lifter" }, { 44, "the spinning platform" }, { 158, "a cable car" }, { 159, "a cable car" } };
	char* fallenLifter = LifterUnderFallen();
	for (const auto& k : kinds)
		for (unsigned i = 0, n = GetNumLvlClassInstances(2, k.id); i < n; i++) {
			char* o = (char*)GetLvlObject(2, k.id, i);
			if (!o || IsBadReadPtr(o, 0x100) || o == fallenLifter) continue;
			for (int off = 0x10; off < 0x100; off += 4)   // its own moving ground (CDynamicGround), not a station's
				if (*(char**)(o + off) == ground && *(uintptr_t*)ground == 0x6241BC) { *what = k.what; return true; }
		}
	for (unsigned i = 0, n = GetNumLvlClassInstances(2, 21); i < n; i++) {
		char* o = (char*)GetLvlObject(2, 21, i);
		if (!o || *(char**)(o + 0x28) != ground) continue;
		if (ProximityGroupCommandsTrio(*(uint16_t*)(o + 0x30)) || ProximityGroupCommandsTrio(*(uint16_t*)(o + 0x32))) {
			*what = "a blue circle";
			return true;
		}
	}
	return false;
}

} // namespace

bool IndivHoldsBack(char* hook) {
	int down = IndivFallenHero();
	if (down < 0 || !hook || CoopSuspended()) return false;
	Trio t = GetTrio();
	Hero h = t ? t.GetHero(Other(down)) : Hero();
	if (!h) return false;
	uintptr_t vt = *(uintptr_t*)hook;
	char* ground = *(char**)(h.raw() + 0x280);
	const char* what = nullptr;
	if (vt == 0x6261F4) {   // CKHkClueMan: spawned (its node shown) and the hero within its talk (+0x70)
		char* node = *(char**)(hook + 0x10);
		char* fr = node ? *(char**)(node + 4) : nullptr;
		if (!fr || (*(uint16_t*)(node + 0x10) & 2)) return false;
		const float* p = (const float*)(fr + 0x80);
		float r = *(float*)(hook + 0x70);
		if (!(r > 0.5f && r < 50.0f)) r = 5.0f;
		Vector3 d = h.Pos() - Vector3(p[0], p[1], p[2]);
		if (d.len2() > r * r) return false;
		IndivBlockTalk("a Roman agent's talk");
		return true;
	} else if (vt == 0x628EE4) {   // a blue circle (its events command the trio), the hero on it
		if (!ground || *(char**)(hook + 0x28) != ground) return false;
		if (!ProximityGroupCommandsTrio(*(uint16_t*)(hook + 0x30)) && !ProximityGroupCommandsTrio(*(uint16_t*)(hook + 0x32)))
			return false;
		what = "a blue circle";
	} else if (vt == 0x628E0C || vt == 0x625E2C || vt == 0x6293D4 || vt == 0x6262D4) {
		// the spinning platform, a lifter, cable cars: the hero on its own moving ground
		if (!ground || *(uintptr_t*)ground != 0x6241BC) return false;
		bool on = false;
		for (int off = 0x10; off < 0x100 && !on; off += 4) on = *(char**)(hook + off) == ground;
		if (!on) return false;
		if (hook == LifterUnderFallen()) {
			// The fallen hero lies on this lifter: the other may come onto it to revive it,
			// and the lifter goes on as it would with both on it -- carrying the fallen
			// one too. (Held, its ride caught up at once after the revive: 15 units in a
			// frame, liftdie21a.)
			static DWORD s_told = 0;
			if (GetTickCount() - s_told > 2000) {
				s_told = GetTickCount();
				Log("indiv: %s is on the lifter %s lies on -- no put-back", HeroName(Other(down)), HeroName(down));
			}
			return false;
		}
		what = vt == 0x628E0C ? "the spinning platform" : vt == 0x625E2C ? "a lifter" : "a cable car";
	} else {
		return false;
	}
	IndivBlockPartnerSpot(what);
	return true;
}

// A cable car: the hero's situation 2 (handler 0x49AB50, table +0x3E4 indexed by
// +0x3E9; 0x49A670 is situation 1's -- the check against it never matched) boards
// it on a later tick (the car's 0x512B, the trio's 0x4801 / 0x4812) -- once
// boarded he is the car's. Its exit run now (as the chooser would). True: it was in it.
static bool LeaveCableCarSituation(Hero h) {
	uint8_t* sit = (uint8_t*)(h.raw() + 0x3E9);
	typedef char(__fastcall* SitFn)(void* self, void* edx, int mode, int other);
	SitFn* table = *(SitFn**)(h.raw() + 0x3E4);
	if (*sit != 0x02 || !table || (uintptr_t)table[0x02] != 0x49AB50) return false;
	void* owner = *(void**)(h.raw() + 0x3E0);
	if (owner) table[0x02](owner, nullptr, 2, 0xFF);
	*sit = 0xFF;
	return true;
}

bool IndivLeaveCableCar(int hero) {
	Trio t = GetTrio();
	Hero h = t ? t.GetHero(hero) : Hero();
	if (!h || !LeaveCableCarSituation(h)) return false;
	IndivPutOnSafeGround(hero);
	return true;
}

namespace {

void BlockPartnerGrounds() {
	int down = IndivFallenHero();
	if (down < 0 || CoopSuspended()) return;
	Trio t = GetTrio();
	Hero h = t ? t.GetHero(Other(down)) : Hero();
	if (!h) return;
	const char* what = nullptr;
	// a cable car: out of its situation, and put back
	if (LeaveCableCarSituation(h)) {
		IndivBlockPartnerSpot("a cable car");
		return;
	}
	// (The belly slide's slope -- ground +0x2A bit 3, "Slide" -- is no partner
	// spot any more, user: the slide starts at its cross detector, LV2-2's event
	// 0x0028 to CKAsterixSlideFP, which zones hold back with the frame before's
	// put-back since round 65; the slope check dates from before that.)
	if (!OnPartnerGround(h, &what)) return;   // (launchers: in the hero's prompt, coop.cpp)
	IndivBlockPartnerSpot(what);
}

// Roman catapults (CKHkCatapult 2,130) firing where `p` is: the tick
// (FUN_00508E70) fires at the leader from +0x48 units off its node's place
// (+0x10 -> frame +4, world +0x80; nearer: its passive range) to +0x4C (x / z),
// armed (+0x14 bit 0x10) and not destroyed (bit 4); a stone every +0x74 seconds
// (its charge duration, FUN_00509E20: timer +0x70 from 0 at each throw), each
// landing ~2.8-3.0 s after its throw -- so they land +0x74 apart too (catrate31:
// LV3-1's 12 / 50 / 3.5, thrown 3.53 s apart, landing 2.81-2.97 s on). The
// seconds between stones landing there (several catapults: theirs together),
// 0 for none.
float CatapultGap(const Vector3& p) {
	static float s_lastGap = -1.0f;
	static int s_told = 0;
	float rate = 0.0f;
	int seen = -1;
	for (unsigned i = 0, n = GetNumLvlClassInstances(2, 130); i < n; i++) {
		char* c = (char*)GetLvlObject(2, 130, i);
		if (!c || IsBadReadPtr(c, 0x78) || (*(uint8_t*)(c + 0x14) & 0x14) != 0x10) continue;
		char* node = *(char**)(c + 0x10);
		char* fr = node && !IsBadReadPtr(node, 8) ? *(char**)(node + 4) : nullptr;
		if (!fr || IsBadReadPtr(fr, 0x8C)) continue;
		const float* at = (const float*)(fr + 0x80);
		float passive = *(float*)(c + 0x48), reach = *(float*)(c + 0x4C), charge = *(float*)(c + 0x74);
		if (!(reach > 0.0f && reach < 1000.0f && charge > 0.1f && charge < 600.0f)) continue;
		float dx = p.x - at[0], dz = p.z - at[2], d2 = dx * dx + dz * dz;
		if (d2 < passive * passive || d2 >= reach * reach) continue;
		rate += 1.0f / charge;
		seen = (int)i;
	}
	float gap = rate > 0.0f ? 1.0f / rate : 0.0f;
	if (fabsf(gap - s_lastGap) > 0.01f && s_told < 20) {
		s_told++;
		if (gap > 0.0f)
			Log("indiv: (%.1f %.1f %.1f) is under catapult fire (#%d and any others): a stone every %.2f s", p.x, p.y, p.z, seen,
			    gap);
		else if (s_lastGap > 0.0f)
			Log("indiv: (%.1f %.1f %.1f) is under no catapult's fire", p.x, p.y, p.z);
	}
	s_lastGap = gap;
	return gap;
}

// How long the grab is held to revive the hero fallen at `at`: under catapult
// fire, revive_hold_catapult_window of the time between stones (user: LV6-4's
// can't be reached to be beaten, and its stones kept the revive from ever
// ending), never longer than revive_hold_seconds.
float HoldSeconds(const Vector3& at) {
	float hold = g_config.reviveHold, gap = CatapultGap(at);
	if (gap > 0.0f && gap * g_config.reviveHoldCatapultWindow < hold) hold = gap * g_config.reviveHoldCatapultWindow;
	return hold < 0.1f ? 0.1f : hold;
}

int SecondsLeft(int down) {
	float s = g.limitLeft[down];
	return s > 0.0f ? (int)ceilf(s) : 0;
}

void ShowDownHint(int down) {
	int up = Other(down);
	Trio t = GetTrio();
	Hero h = t ? t.GetHero(down) : Hero();
	wchar_t text[200];
	swprintf(text, 200, L"%s is down!\nHold %s by him for %g s to revive", HintHeroName(down),
	         HintButtonName(PlayerOf(up), HB_B), h ? HoldSeconds(h.Pos()) : g_config.reviveHold);
	HintShow(HINT_REVIVE, PlayerOf(up), text, 8000);
}

// The HUD's health shows again and is read afresh (game manager +0x5C bit
// 0x4000, as the game's heal sets it).
void RefreshHud() {
	char* gm = GM();
	if (gm) *(uint32_t*)(gm + 0x5C) |= 0x4000;
}

// Where to put the hero brought beside fallen `down`: one of the fallen hero's
// own last solid grounds 1 to 3 units from where it lies (the nearest to 1.5),
// else where it lies. Not the graph's cells: those of LV2-6's bridge read as
// walkable with the bridge in -- Obelix was put over the pit and fell
// (lv2-6_obelix_tps_to_death.log).
Vector3 SpotBesideFallen(int down, const Vector3& at) {
	const Vector3* best = nullptr;
	float bestD = 1e30f;
	auto consider = [&](const Vector3& p) {
		Vector3 d = p - at;
		float h2 = d.x * d.x + d.z * d.z;
		if (h2 < 1.0f || h2 > 9.0f || fabsf(d.y) > 1.5f) return;
		float score = fabsf(sqrtf(h2) - 1.5f);
		if (score < bestD) { bestD = score; best = &p; }
	};
	for (int i = 0; i < kSafeSpots; i++)
		if (g.safe[down][i].at) consider(g.safe[down][i].pos);
	for (int k = 0; k < g.trailN[down]; k++) consider(g.trail[down][k]);
	return best ? *best : at;
}

} // namespace

bool IndivJoinSpot(int to, Vector3* out) {
	if (!g.active || (to != HERO_ASTERIX && to != HERO_OBELIX)) return false;
	Trio t = GetTrio();
	Hero h = t ? t.GetHero(to) : Hero();
	if (!h) return false;
	Vector3 at = h.Pos();
	char* ground = *(char**)(h.raw() + 0x280);
	bool onFeet = (*(uint8_t*)(h.raw() + 0x384) & 7) == 0 && ground && !IsBadReadPtr(ground, 0x2C) &&
	              !(*(uint8_t*)(ground + 0x2A) & 4);
	if (onFeet) {
		if (*(uintptr_t*)ground != 0x624258) return false;   // a lifter's or a platform's ground: on it, with `to`
		Vector3 p = SpotBesideFallen(to, at);
		if ((p - at).len2() < 0.01f) return false;   // none 1 to 3 units away
		*out = p;
		out->y += 0.5f;
		return true;
	}
	// Off its feet: where it stood last, if that is near.
	const SafeSpot* best = nullptr;
	for (int i = 0; i < kSafeSpots; i++) {
		const SafeSpot& s = g.safe[to][i];
		if (!s.at) continue;
		Vector3 d = s.pos - at;
		if (d.x * d.x + d.z * d.z > 12.0f * 12.0f || fabsf(d.y) > 12.0f) continue;
		if (!best || (int)(s.at - best->at) > 0) best = &s;
	}
	if (!best) return false;
	*out = best->pos;
	out->y += 0.5f;
	return true;
}

namespace {

// While a hero is down the lifters (2,47) stay shut (IndivBlockPartnerSpot: the
// standing hero is put back off one), but the pathfinding graph still leads
// over them -- LV2-1's lifter shaft is a node of its own (53) between the floor
// below and the hearth above, joined by the sidekick's jumps, always on: Obelix
// below, Asterix down by the hearth, "can walk: yes", and no way up
// (xxl_coop.lv2-1_lifter_revival_tp.log). For the revive's own path queries,
// the nodes holding a lifter (its node's place, matrix +0x40) are shut -- all
// but the one the fallen hero lies on (LifterUnderFallen: the way to it).
struct ShutLifters {
	Vector3 at[16];
	int n = 0;
	ShutLifters() {
		char* under = LifterUnderFallen();   // (not the one the fallen hero lies on: that one is its way)
		for (unsigned i = 0, c = GetNumLvlClassInstances(2, 47); i < c && n < 16; i++) {
			KHook* lifter = (KHook*)GetLvlObject(2, 47, i);
			if (!lifter || (char*)lifter == under || !lifter->node || !lifter->node->matrix) continue;
			const float* m = (const float*)((char*)lifter->node->matrix + 0x40);
			if (m[0] == 0.0f && m[1] == 0.0f && m[2] == 0.0f) continue;   // (one of LV2's is at the origin)
			at[n++] = Vector3(m[0], m[1], m[2]);
		}
		Close();
	}
	~ShutLifters() { Open(); }
	void Close() { PfCloseNodesAt(at, n); }
	void Open() { PfCloseNodesAt(nullptr, 0); }
};
const int kReachOnlyLifter = 2;   // reachTold: no way but over a lifter
const int kReachOffGround = 3;    // ... it lies off the graph's walkable cells

// The revive time limit's "auto": the walk to the fallen hero, then the grace.
// The bosses that roll after the heroes along a way of their own (CKHkBoss,
// class (2,177): LV2-10, LV3-6 -- its +0x600 way is only the jump at the run's
// end): where each went, a spot every unit, newest last. A fallen hero lying
// on that trail, the boss rolling on away from it, is left behind the boss: the
// other hero can't get past the boss to it (user: game over then; xxl_coop.
// lv3-6_partner_death_underground.log). A boss that turns back starts its
// trail afresh (one moving to and fro leaves nobody behind).
const int kBossTrail = 512;
const int kBosses = 2;
const float kBossBack = 9.0f;     // this far back along its trail (its half length, 7.3, and some): behind it
const float kBossWide = 8.5f;     // ... and this near the trail's line (its half width: 7.3, the user's run)
const float kBossHalf = 6.0f;     // ... and this far behind its back (its half length about 7)
const DWORD kBossRollMs = 1000;   // moved this lately: still rolling
struct BossTrail {
	char*   obj = nullptr;
	Vector3 at;
	DWORD   movedAt = 0;
	Vector3 pts[kBossTrail];
	int     n = 0, next = 0;
	const Vector3& Pt(int back) const { return pts[(next + kBossTrail - 1 - back) % kBossTrail]; }   // 0: newest
};
BossTrail g_boss[kBosses];
int g_bossLevel = -2;

bool BossPos(char* o, Vector3* out) {
	char* node = o ? *(char**)(o + 0x10) : nullptr;
	char* fr = node ? *(char**)(node + 4) : nullptr;
	if (!fr) return false;
	const float* m = (const float*)(fr + 0x80);
	*out = Vector3(m[0], m[1], m[2]);
	return true;
}

void TrackBosses(int level, DWORD now) {
	if (level != g_bossLevel) {
		g_bossLevel = level;
		for (BossTrail& b : g_boss) b = BossTrail();
	}
	// Only the bosses that chase the heroes (user: LV4-11's, LV5-11's and LV6-5's
	// go to and fro, and stay the game's).
	if (level != 2 && level != 3) return;
	unsigned count = GetNumLvlClassInstances(2, 177);
	for (unsigned i = 0; i < count && i < (unsigned)kBosses; i++) {
		BossTrail& b = g_boss[i];
		char* o = (char*)GetLvlObject(2, 177, i);
		Vector3 p;
		if (!BossPos(o, &p)) continue;
		if (o != b.obj) { b = BossTrail(); b.obj = o; b.at = p; }
		Vector3 d = p - b.at;
		b.at = p;
		if (d.len2() > 20.0f * 20.0f) { b.n = b.next = 0; b.movedAt = 0; continue; }   // put back (a respawn)
		if (d.len2() > 0.0001f) b.movedAt = now;
		if (b.n && (p - b.Pt(0)).len2() < 1.0f) continue;
		if (b.n >= 2) {
			Vector3 a = b.Pt(0) - b.Pt(1), c = p - b.Pt(0);
			if (a.x * c.x + a.z * c.z < -0.5f * sqrtf((a.x * a.x + a.z * a.z) * (c.x * c.x + c.z * c.z))) {
				Vector3 last = b.Pt(0);
				b.n = 1;
				b.next = 1;
				b.pts[0] = last;
				static int s_turned;
				if (s_turned++ < 10) Log("indiv: boss #%u turned back at (%.1f %.1f %.1f) -- its trail afresh", i, p.x, p.y, p.z);
			}
		}
		b.pts[b.next] = p;
		b.next = (b.next + 1) % kBossTrail;
		if (b.n < kBossTrail) b.n++;
	}
}

bool BossRolling(DWORD now) {
	for (const BossTrail& b : g_boss)
		if (b.obj && b.n >= 2 && now - b.movedAt < kBossRollMs) return true;
	return false;
}

// How far back along a rolling boss's trail `p` lies (units from the boss), -1
// not on it (or not kBossBack back).
float BehindBoss(const Vector3& p, DWORD now, int* which) {
	for (int i = 0; i < kBosses; i++) {
		const BossTrail& b = g_boss[i];
		if (!b.obj || b.n < 2 || now - b.movedAt >= kBossRollMs) continue;
		// Behind its back too (its heading over the last 3 units): a body it shoves
		// on round a corner lies by the trail it has just come along, before it
		// (bossdown36, the first corner).
		Vector3 head = b.at - b.Pt(b.n > 3 ? 3 : b.n - 1);
		float hl = sqrtf(head.x * head.x + head.z * head.z);
		if (hl < 0.5f || ((p.x - b.at.x) * head.x + (p.z - b.at.z) * head.z) / hl > -kBossHalf) continue;
		float arc = sqrtf((b.at - b.Pt(0)).len2());
		for (int k = 1; k < b.n; k++) {
			const Vector3& a = b.Pt(k - 1);
			const Vector3& c = b.Pt(k);
			arc += sqrtf((a - c).len2());
			if (arc < kBossBack) continue;
			Vector3 d = p - c;
			if (d.x * d.x + d.z * d.z <= kBossWide * kBossWide && fabsf(d.y) < 3.0f) {
				*which = i;
				return arc;
			}
		}
	}
	return -1.0f;
}

// A rolling boss within `r` units (x / z) of `p`.
int BossNear(const Vector3& p, float r, DWORD now) {
	for (int i = 0; i < kBosses; i++) {
		const BossTrail& b = g_boss[i];
		if (!b.obj || b.n < 2 || now - b.movedAt >= kBossRollMs) continue;
		Vector3 d = p - b.at;
		if (d.x * d.x + d.z * d.z <= r * r) return i;
	}
	return -1;
}

// A body the rolling boss shoves on before it can't be revived either (user:
// it moves as fast as the boss, and its saws kill a hero standing in its way to
// hold the grab): this far dragged with a boss near, the game over.
const float kBossDragNear = 12.0f;
const float kBossDragged = 2.0f;
Vector3 g_downLast[2];
float g_dragged[2];

// A fallen hero's last place on the level's walkable ground, and a body shoved
// off it: the rolling boss pushes a body before it, and at LV3-6's second corner
// (x -317.9, z -110.6) through the floor (user: -311.9 -3.8 -105.5, nobody could
// reach it). Lower than that place by this much and in no pathfinding node: put
// back there.
const float kSunk = 1.5f;
Vector3 g_downGood[2];
bool g_downGoodSet[2];
DWORD g_downGoodAt[2];

void KeepBodyOnGround(int down, Hero h, DWORD now) {
	Vector3 p = h.Pos();
	if (!g_downGoodSet[down]) {   // (on the ground's nodes only: not a death in mid-air)
		if (now - g_downGoodAt[down] < 100) return;
		g_downGoodAt[down] = now;
		if (PfLineInNodes(p, p) != PF_YES) return;
		g_downGood[down] = p;
		g_downGoodSet[down] = true;
		return;
	}
	if (p.y < g_downGood[down].y - kSunk) {
		if (LifterUnderFallen() || PfLineInNodes(p, p) == PF_YES) {   // (a lower floor it can be walked to on)
			g_downGood[down] = p;
			return;
		}
		static int s_sunk;
		if (s_sunk++ < 20)
			Log("indiv: %s's body was shoved off the ground to (%.1f %.1f %.1f), in no pathfinding node -- back on it (%.1f %.1f %.1f)",
			    HeroName(down), p.x, p.y, p.z, g_downGood[down].x, g_downGood[down].y, g_downGood[down].z);
		Vector3 to = g_downGood[down];
		to.y += 0.5f;
		PlaceHero(h, to);
		return;
	}
	if (now - g_downGoodAt[down] < 100 || (p - g_downGood[down]).len2() < 0.25f) return;
	g_downGoodAt[down] = now;
	if (PfLineInNodes(p, p) == PF_YES) g_downGood[down] = p;
}

void SetLimit(int down, float way, const char* what) {
	g.limitLeft[down] = way / kWalkSpeed + g_config.reviveGrace;
	g.limitSet[down] = true;
	Log("indiv: %s has %.0f s to revive %s (%s: %.0f units, %.0f s; + %.0f s)", HeroName(Other(down)), g.limitLeft[down],
	    HeroName(down), what, way, way / kWalkSpeed, g_config.reviveGrace);
}

// Where the hero cut off from its fallen leader is brought: of the places it
// can walk to the fallen hero from (the level's pathfinding), the one nearest
// it -- where either hero walked lately in this sector (solid ground), else
// beside the fallen hero (SpotBesideFallen) -- so it walks the rest of the way
// (user). With a fixed time limit no further than the walk the time left leaves
// before the revive itself (grace). `*way`: that walk's length.
Vector3 CatchUpSpot(int down, Hero h, int up, Hero o, float* way) {
	Vector3 at = h.Pos(), from = o.Pos();
	Vector3 best = SpotBesideFallen(down, at);
	*way = sqrtf((best - at).len2());
	if (g.offGround[down]) {   // (off the graph's cells: the graph's spots are all below it)
		Log("indiv: %s lies off the graph's walkable cells -- %s is brought beside it", HeroName(down), HeroName(up));
		return best;
	}
	if (!PfWayTo(at)) {
		Log("indiv: no pathfinding where %s lies -- %s is brought beside it", HeroName(down), HeroName(up));
		return best;
	}
	float w0 = PfWayFrom(best);
	if (w0 >= 0.0f) *way = w0;
	float ceiling = 1e30f;
	if (g_config.reviveLimit > 0.0f && g.limitSet[down]) {
		float t = g.limitLeft[down] - g_config.reviveGrace;
		ceiling = t > 0.0f ? t * kWalkSpeed : 0.0f;
	}
	float bestD = (best - from).len2();
	int sector = LoadedSector(), nearer = 0, cut = 0, tooFar = 0;
	for (int k = 0; k < 2; k++) {
		int who = k ? up : down;
		for (int n = 0; n < g.pathN[who]; n++) {
			const PathSpot& s = g.path[who][n];
			Vector3 d = s.pos - at;
			if (s.sector != sector || d.x * d.x + d.z * d.z < 1.0f) continue;   // (on the fallen hero)
			float dd = (s.pos - from).len2();
			if (dd >= bestD) continue;
			nearer++;
			float w = PfWayFrom(s.pos);
			if (w < 0.0f) { cut++; continue; }
			if (w > ceiling) { tooFar++; continue; }
			best = s.pos;
			bestD = dd;
			*way = w;
		}
	}
	Log("indiv: %s's catch-up spot: %.1f units from it, a %.0f-unit walk to %s (%d nearer spots walked: %d with no way, "
	    "%d past the time limit's %.0f units)", HeroName(up), sqrtf(bestD), *way, HeroName(down), nearer, cut, tooFar,
	    ceiling < 1e29f ? ceiling : -1.0f);
	return best;
}

// A fallen hero the other can't walk to (the level's pathfinding graph,
// pathfind.cpp: LV2-6's pit before its bridge is out, LV2-3's hearth room once
// its door has shut): the other hero is brought to the nearest place it can walk
// to it from (CatchUpSpot), as the AI sidekick is brought to the player in single
// player -- whoever led (user: the heroes are apart from then on either way;
// round 83 had a fallen sidekick wait for the level, and LV2-6's slide ramp left
// Asterix above Obelix for good, xxl_coop.lv2-6_unreachable.log). Asked every
// 0.5 s from 2 s after the fall; three noes in a row (not a hero in mid-jump over
// a gap's cells) before the move. After the grab's revive it goes back where it
// came from (the fallen hero's place may be one it can't leave: LV2-3's hearth
// room) -- not when only a lifter (the mod keeps them shut, ShutLifters) or its
// own way since the fall (it could walk there after the fall, then cut itself
// off: a drop jumped down) parted them. The first answer that it has a way sets
// the time limit's "auto" from that walk.
void WayToFallen(int down, Hero h, int up, Hero o, DWORD now) {
	if (CoopSuspended() || now - g.fellAt[down] < 2000 || now - g.reachAt[down] < 500) return;
	if (!HeroModePlayable(o.Flags18())) return;   // (on the machine gun: brought once off it)
	g.reachAt[down] = now;
	ShutLifters shut;
	int r = PfReachable(o.Pos(), h.Pos());
	bool byLifter = false;
	if (r == PF_NO && shut.n) {
		shut.Open();
		byLifter = PfReachable(o.Pos(), h.Pos()) == PF_YES;
		shut.Close();
	}
	// It lies where the graph has walls only (2+ cells to its nearest walkable
	// one): ground the AI's grid knows nothing of -- LV6-5's ledge (830.3 69.6
	// -254.1) over the floor of the same node, 5.5 lower, read as "yes, 12 units"
	// from below and nobody could get up (xxl_coop.lv6-5_death_stuck.log). No way
	// known then, unless the other hero is beside it already -- or on its height
	// (2 units: a ledge apart; level 2's start has wall cells round the floor the
	// heroes stand on, spsecond2: Obelix 1.2 lower was brought for nothing).
	bool offGround = false;
	if (r == PF_YES) {
		Vector3 d = o.Pos() - h.Pos();
		bool beside = (d.x * d.x + d.z * d.z < 4.0f * 4.0f && fabsf(d.y) < kReviveHeight) || fabsf(d.y) <= 2.0f;
		int ring = beside ? 0 : PfSeedRing(h.Pos());
		if (ring >= 2) { r = PF_NO; offGround = true; }
	}
	int told = byLifter ? kReachOnlyLifter : offGround ? kReachOffGround : r;
	if (told != g.reachTold[down]) {
		static int s_told;
		if (s_told++ < 40)
			Log("indiv: can %s walk to fallen %s (%s when it fell)? %s", HeroName(up), HeroName(down),
			    g.fellLeading[down] ? "leading" : "not leading",
			    r == PF_YES ? "yes" : byLifter ? "no -- only over a lifter, shut while it is down"
			    : offGround ? "no -- it lies off the graph's walkable cells (ground the AI's grid has as walls)"
			    : r == PF_NO ? "no" : "not known");
		g.reachTold[down] = told;
	}
	if (r != PF_NO) {
		g.cutOff[down] = 0;
		if (r == PF_YES) g.hadWay[down] = true;
		if (g_config.reviveLimit < 0.0f && !g.limitSet[down]) {
			float w = r == PF_YES ? PfWalk(o.Pos(), h.Pos()) : -1.0f;
			if (w >= 0.0f) SetLimit(down, w, "its walk there");
			else SetLimit(down, sqrtf((h.Pos() - o.Pos()).len2()), "no walk known, the straight line");
		}
		return;
	}
	bool cutItself = !byLifter && !offGround && g.hadWay[down];
	g.offGround[down] = offGround;
	if (BossRolling(now)) {
		// The boss is between them, or shoving the body on before it: no bringing
		// the hero there (bossdown36: it was put in the boss's way and died too).
		static int s_rolling;
		if (g.cutOff[down] == 0 && s_rolling++ < 10)
			Log("indiv: %s not brought to %s -- a boss is rolling", HeroName(up), HeroName(down));
		g.cutOff[down] = 1;
		return;
	}
	if (++g.cutOff[down] < 3) return;
	g.cutOff[down] = 0;
	g.broughtStays[up] = byLifter || cutItself;
	float way = 0.0f;
	Vector3 spot = CatchUpSpot(down, h, up, o, &way);
	spot.y += 0.5f;
	if (!g.brought[up]) {   // the first move's start: where it goes back to
		g.brought[up] = true;
		g.broughtFrom[up] = o.Pos();
	}
	g.broughtTo[up] = spot;
	PlaceHero(o, spot);
	g.placedAt[up] = now;
	Log("indiv: %s can't walk to %s%s -- brought where it can walk to it from (%.1f %.1f %.1f)", HeroName(up),
	    HeroName(down),
	    byLifter ? " but over a lifter, shut while it is down" : cutItself ? " any more (it could since the fall)"
	    : g.fellLeading[down] ? ", who led when it fell" : " (not since the fall)",
	    spot.x, spot.y, spot.z);
	if (g_config.reviveLimit < 0.0f && !g.limitSet[down]) SetLimit(down, way, "its walk from where it was brought");
}

void Revive(int idx, Hero h, const char* how, bool byGrab = false) {
	Stats& s = Current(idx);
	int hp = (s.shields * 5 + 1) / 2;   // half the health back (1.5 of 3 shields)
	s.hp = (uint8_t)(hp > 0 ? hp : 1);
	Commit(idx);
	RefreshHud();
	Raise(h);
	g.fallen[idx] = false;
	g.revive[idx] = 0;
	// Single player (second chance): up as the AI companion again -- kept in the
	// player variant for its death (coop.cpp); the lead comes back to it below
	// if it led when it fell.
	Trio tr = GetTrio();
	uint8_t f18 = h.Flags18();
	if (g.solo && tr && tr.Leader() != idx && HeroModePlayable(f18) && HeroIsPlayerVariant(f18)) {
		*(uint8_t*)(h.raw() + 0x18) = HeroAiVariant(f18);
		Log("indiv: %s up as the AI companion (single player)", HeroName(idx));
	}
	// It led when it fell: the lead went to the other hero meanwhile, and comes
	// back to it (next frame -- a launcher's revive is inside a hero's tick).
	if (g.fellLeading[idx] && !g.levelLead[idx]) g.leadBack = idx;
	g.offGround[idx] = false;
	if (g.solo) { g.parkRevived = idx; g.parkRevivedAt = GetTickCount(); }
	g.fellLeading[idx] = false;
	g.levelLead[idx] = false;
	g.limitSet[idx] = false;
	// The other hero was brought to it and revived it by the grab: back where it
	// came from (the leader's place can be one only Asterix gets out of) -- unless
	// it goes on from where it revived it. A revive at a sector change or a
	// launcher leaves it where it is; so does one brought past a lifter (the
	// heroes together again is what the lifter was for) or one that had cut
	// itself off (it can go back the way it went).
	int up = Other(idx);
	if (g.brought[up]) {
		Trio t = GetTrio();
		Hero o = t ? t.GetHero(up) : Hero();
		if (byGrab && o && !g.broughtStays[up]) {
			g.sendBack = up;
			g.broughtTo[up] = o.Pos();
		} else g.brought[up] = false;
	}
	HintHide(HINT_REVIVE);
	Log("indiv: %s is back on its feet (%s), health %d", HeroName(idx), how, s.hp);
}

// The revive time ran out: the game over, the fallen hero's death (single
// player's death sequence on it: it leads, it dies again from its first frame
// as the chooser picks the death once more). The hero still standing keeps its
// own health for its own business (IndivEnter's spare scope): it does not die
// beside it.
void TimeOut(int down, Hero h, int up, const char* why = "was not revived in time") {
	Log("indiv: %s %s -- game over, the death is %s's (%s stays on its feet)", HeroName(down), why,
	    HeroName(down), HeroName(up));
	g.gameOver = true;
	g.spared = up;
	CoopDropSoloPark("game over");
	g.limitSet[down] = false;
	g.drain[down] = g.drain[up] = false;
	Use(down);   // the game manager sees its 0
	HintHide(HINT_REVIVE);
	Raise(h);
	CoopDeathIs(down, "its revive time ran out");
}

// The revive time limit of fallen `down`, counted while the game runs (no pause
// menu, merchant, scripted mode) and the other hero is not known to be cut off
// from it; the countdown in the fallen hero's player's box only, the grab's bar
// or not (user). True: it ran out (the game over) -- not while the grab is held
// by it.
bool TimeLimit(int down, Hero h, int up, bool holding, float dt, DWORD now) {
	if (!g.limitSet[down]) return false;
	char* gm = GM();
	bool paused = gm && (*(uint8_t*)(gm + 0xCC) & 3) == 2;
	bool counting = !paused && !CoopSuspended() && CoopShopper() < 0 && g.reachTold[down] != PF_NO &&
	                g.reachTold[down] != kReachOnlyLifter && g.reachTold[down] != kReachOffGround;
	if (counting) g.limitLeft[down] -= dt;
	if (counting && !holding && g.limitLeft[down] <= 0.0f) {
		TimeOut(down, h, up);
		return true;
	}
	if (now - g.limitHintAt < 250) return false;
	g.limitHintAt = now;
	wchar_t text[160];
	if (g.solo) swprintf(text, 160, L"Revive %s\n%d s left", HintHeroName(down), SecondsLeft(down));
	else swprintf(text, 160, L"%s comes to revive you\n%d s left", HintHeroName(up), SecondsLeft(down));
	// (Single player: in the left box -- in player 1's, with the revive's own
	// hints and bar there, the two took turns every 250 ms: user.)
	HintShow(HINT_REVIVE, g.solo ? HINT_LEFT_BOX : PlayerOf(down), text, 600);
	return false;
}

// After a game over: both heroes as the respawn left the game's values.
void Respawned() {
	Stats s;
	Read(s);
	g.st[0] = g.st[1] = s;
	g.gameOver = false;
	g.spared = -1;
	g.sparing = false;
	Trio t = GetTrio();
	g.leadBack = g.sendBack = -1;   // the respawn's lead and places are the game's
	for (int i = 0; i < 2; i++) {
		g.limitSet[i] = false;
		g.brought[i] = false;
		g.fallen[i] = false;
		g.drain[i] = false;
		g.fellLeading[i] = false;
		g.levelLead[i] = false;
		g.revive[i] = 0;
		Hero h = t ? t.GetHero(i) : Hero();
		if (h) Raise(h);
	}
	HintHide(HINT_REVIVE);
	RefreshHud();
	Log("indiv: respawn -- both heroes at health %d, %d shields", s.hp, s.shields);
}

void Activate(Trio t) {
	Stats s;
	if (!Read(s)) return;
	g.st[0] = g.st[1] = s;
	g.ctx = t.Leader() == HERO_OBELIX ? HERO_OBELIX : HERO_ASTERIX;
	g.depth = 0;
	g.active = true;
	g.gameOver = false;
	g.spared = -1;
	g.sparing = false;
	memset(g.fallen, 0, sizeof(g.fallen));
	memset(g.drain, 0, sizeof(g.drain));
	memset(g.revive, 0, sizeof(g.revive));
	memset(g.limitSet, 0, sizeof(g.limitSet));
	memset(g.safe, 0, sizeof(g.safe));
	g.p2Hero = CoopP2Hero();
	if (g.kept && g.keptLevel == g.level && g.keptHero == g.p2Hero && g.p2Hero >= 0) {
		// Player 2 back in the same level: their hero as they left it.
		g.st[g.p2Hero] = g.keptStats;
		if (g.ctx == g.p2Hero) Write(g.keptStats);
		if (g.keptFallen || g.keptStats.hp == 0) {
			g.fallen[g.p2Hero] = true;
			ShowDownHint(g.p2Hero);
		}
		Log("indiv: individual heroes on -- %s (player 2) as they left it: health %d, %d shields%s; %s health %d",
		    HeroName(g.p2Hero), g.keptStats.hp, g.keptStats.shields, g.fallen[g.p2Hero] ? ", down" : "",
		    HeroName(Other(g.p2Hero)), s.hp);
		g.kept = false;
		return;
	}
	if (!g.solo) g.kept = false;   // (single player: player 2's stats still kept for their return)
	Log("indiv: individual heroes on%s -- each starts with health %d, %d shields, combo hits %u",
	    g.solo ? " (single player: second chance)" : "", s.hp, s.shields, s.hits);
}

void Deactivate(Trio t) {
	// The heroes are one again: the leader's stats stay (player 1's hero leads
	// now); a fallen hero gets up as the AI companion.
	int lead = t ? t.Leader() : -1;
	if (g.p2Hero == HERO_ASTERIX || g.p2Hero == HERO_OBELIX) {
		g.keptStats = Current(g.p2Hero);
		g.keptFallen = g.fallen[g.p2Hero];
		g.keptHero = g.p2Hero;
		g.keptLevel = g.level;
		g.kept = true;
		Log("indiv: %s's stats kept for player 2's return: health %d, %d shields%s", HeroName(g.p2Hero),
		    g.keptStats.hp, g.keptStats.shields, g.keptFallen ? ", down" : "");
	}
	if (lead == HERO_ASTERIX || lead == HERO_OBELIX) Use(lead);
	for (int i = 0; i < 2 && t; i++) {
		if (!g.fallen[i]) continue;
		Hero h = t.GetHero(i);
		if (h) Raise(h);
		g.fallen[i] = false;
	}
	memset(g.drain, 0, sizeof(g.drain));
	memset(g.limitSet, 0, sizeof(g.limitSet));
	g.spared = -1;
	g.sparing = false;
	g.active = false;
	g.ctx = -1;
	HintHide(HINT_REVIVE);
	Log("indiv: individual heroes off (%s's stats are the game's)", lead >= 0 ? HeroName(lead) : "nobody");
}

// The combo bar runs down in the game manager's tick, for the stats in place
// there (the leader's); the other hero's runs down here the same way.
void RunDownCombo(int idx) {
	char* gm = GM();
	char* clock = gm ? *(char**)(gm + 0xEC) : nullptr;
	if (!clock || idx == g.ctx) return;
	float dt = *(float*)(clock + 0xC);
	if (!(dt > 0.0f && dt < 0.5f)) return;
	Stats& s = g.st[idx];
	if (!s.full) {
		if (s.decay <= 0.0f) s.chain = 0;
		else if (s.decay <= dt) {
			s.decay = *(float*)(gm + 0x9C);
			if (s.hits) s.hits--;
		} else s.decay -= dt;
	} else {
		// Run out: the game's tick also ends the "full" state (FUN_00504080: the
		// stats' +0x10 bit 14 cleared). Left on, the hero's hits went to an empty
		// gauge -- the bar never showed again, or never got to "Combo", until the
		// hero led once more.
		if (s.gauge <= 0.0f) {
			s.chain = s.hits = s.c4 = 0;
			s.gauge = 0.0f;
			s.full = false;
			Log("indiv: %s's full combo bar ran out (not leading)", HeroName(idx));
		}
		else if (s.gauge <= dt) s.gauge = 0.0f;
		else s.gauge -= dt;
	}
}

// A fallen hero's combo bar drains to empty, as the HUD's bar (FUN_00518190)
// can show it. Full (its state 4), the bar draws a fill of its own, counted down
// at the gauge's pace from the full height and read from the gauge (game manager
// +0xA4 of +0xA0) only on +0x160 bit 2 -- zeroed stats left it full, running down
// for its whole time; while its "Combo" word plays it even sets the gauge back to
// +0xA0. So the gauge runs out in kDrainSeconds with bit 2 set at each step, and
// at 0 the bar ends its full state the game's way (hits, chain off). Not full (its
// state 2), a level's glow goes out only as the hits go under that level with
// some still in -- straight to 0 left one or two lit: one hit a frame.
const float kDrainSeconds = 0.5f;
void DrainCombo(int idx, float dt) {
	char* gm = GM();
	if (!g.drain[idx] || !gm) return;
	Stats& s = Current(idx);
	if (s.full) {
		float max = *(float*)(gm + 0xA0);
		s.gauge -= (max > 0.0f ? max : 1.0f) * dt / kDrainSeconds;
		s.barFlags |= 4;
		if (s.gauge <= 0.0f) {   // the gm tick's run-out (FUN_00504080)
			s.gauge = 0.0f;
			s.hits = s.chain = s.c4 = 0;
			s.full = false;
		}
	} else if (s.hits) {
		s.hits--;
		s.decay = 0.0f;
	} else {
		s.level = 0;
		g.drain[idx] = false;
		Log("indiv: %s's combo bar is empty", HeroName(idx));
	}
	Commit(idx);
}

// Bonuses that heal (FUN_004BB260, thiscall(game manager, points), ret 4) or
// add a shield (FUN_00507210, thiscall(game manager, points), ret 4): the
// bonus's tick runs with the trio's leader byte on the hero that picked it up
// (proximity slicing), and the merchant's with the shopper leading.
typedef char(__fastcall* GmFn)(char* gm, void* edx, int arg);
const uintptr_t kHeal = 0x4BB260, kAddShield = 0x507210;
const uint8_t   kGmPrologue[7] = { 0x56, 0x8B, 0x71, 0x40, 0x8A, 0x46, 0x50 };
GmFn g_origHeal = nullptr, g_origAddShield = nullptr;

// A bonus collected: CKHkBasicBonus's tick hands it to its pool's vf40
// (CKGrpAsterixBonusPool 0x506E90, thiscall(pool, hero), ret 4) with the hero
// it flew to; the pool applies it (heal, shield, helmets...). That hero's stats.
// vf40 first asks vf39, which refuses the potion (type 3) unless the trio's
// leader is Asterix: a flask that reached Asterix while Obelix led (the LV4-4
// lever's spitter) was thrown away. Asterix's flask is applied with him leading.
int g_collector = -1;
int g_potionHero = -1;     // the last potion flask's hero, and when
DWORD g_potionAt = 0;
typedef void(__fastcall* CollectFn)(char* pool, void* edx, char* hero);
CollectFn g_origCollect = nullptr;
void __fastcall CollectHook(char* pool, void* edx, char* hero) {
	Trio t = GetTrio();
	int who = -1;
	for (int i = HERO_ASTERIX; t && i <= HERO_OBELIX; i++)
		if (t.GetHero(i).raw() == hero) who = i;
	int prevCollector = g_collector;
	g_collector = who;
	int prev = IndivEnter(who);
	uint8_t* tr = t ? (uint8_t*)t.raw() : nullptr;
	bool slice = tr && who == HERO_ASTERIX && *(int*)(pool + 0x24) == 3 && tr[0x2C] != HERO_ASTERIX;
	uint8_t leader = tr ? tr[0x2C] : 0, companion = tr ? tr[0x2D] : 0;
	if (slice) {
		tr[0x2C] = HERO_ASTERIX;
		tr[0x2D] = HERO_OBELIX;
		Log("indiv: Asterix's magic potion applied with him as the leader (%s leads)", HeroName(leader));
	}
	if (who >= 0 && *(int*)(pool + 0x24) == 3) {
		g_potionHero = who;
		g_potionAt = GetTickCount();
	}
	g_origCollect(pool, edx, hero);
	if (slice && tr[0x2C] == HERO_ASTERIX && tr[0x2D] == HERO_OBELIX) {
		tr[0x2C] = leader;
		tr[0x2D] = companion;
	}
	IndivLeave(prev);
	g_collector = prevCollector;
}

// "May this hero collect" (CKGrpAsterixBonusPool::vf39, 0x507320, thiscall(pool,
// hero), ret 4: the pool update asks it for the leader, the mod's for the other
// player's hero) only refuses the potion pool (type 3) in the game -- a ham at
// full health or a seventh shield was taken and wasted. With a health each, it
// went to a hero it did nothing for, under the nose of the one who needed it:
// a ham (pool type 5) only for a hero short of its own shields' health, a
// shield (type 4) only below six shields, nothing for a fallen hero.
typedef char(__fastcall* MayCollectFn)(char* pool, void* edx, char* hero);
MayCollectFn g_origMayCollect = nullptr;
char __fastcall MayCollectHook(char* pool, void* edx, char* hero) {
	char r = g_origMayCollect(pool, edx, hero);
	if (!r || !g.active || !pool) return r;
	int type = *(int*)(pool + 0x24);
	if (type != 4 && type != 5) return r;
	Trio t = GetTrio();
	int who = -1;
	for (int i = HERO_ASTERIX; t && i <= HERO_OBELIX; i++)
		if (t.GetHero(i).raw() == hero) who = i;
	if (who < 0) return r;
	if (g.fallen[who]) return 0;
	Stats& st = Current(who);
	if (type == 5 && st.hp >= st.shields * 5) return 0;
	if (type == 4 && st.shields >= 6) return 0;
	return r;
}

int Picker() {
	if (g_collector >= 0) return g_collector;
	if (g.depth > 0) return g.ctx;   // inside one hero's business: that hero's
	Trio t = GetTrio();
	int l = t ? t.Leader() : -1;
	return l == HERO_ASTERIX || l == HERO_OBELIX ? l : -1;
}
char __fastcall HealHook(char* gm, void* edx, int arg) {
	int who = Picker();
	int prev = IndivEnter(who);
	char r = g_origHeal(gm, edx, arg);
	if (prev >= 0 || g.active) {
		char* cb = CB();
		if (who >= 0) LogV("indiv: %s heals %d (health %d)", HeroName(who), arg & 0xFF, cb ? *(uint8_t*)(cb + 0x50) : -1);
	}
	IndivLeave(prev);
	return r;
}
char __fastcall AddShieldHook(char* gm, void* edx, int arg) {
	int who = Picker();
	int prev = IndivEnter(who);
	char r = g_origAddShield(gm, edx, arg);
	if (g.active && who >= 0) {
		char* cb = CB();
		Log("indiv: %s gets a shield (%d shields, health %d)", HeroName(who), cb ? *(uint8_t*)(cb + 0x51) : -1,
		    cb ? *(uint8_t*)(cb + 0x50) : -1);
	}
	IndivLeave(prev);
	return r;
}

} // namespace

int IndivPotionDrinker() {
	return g_potionHero >= 0 && GetTickCount() - g_potionAt < 3000 ? g_potionHero : -1;
}

bool IndivActive() { return g.active; }

int IndivEnter(int hero) {
	// A revive time run out (TimeOut): the hero still standing has its own
	// health in place for its own business, and the game's back after it
	// (Hero_PreLifeTick would see the fallen hero's 0 and kill it too).
	if (g.active && g.gameOver && hero == g.spared && hero >= 0 && !g.sparing) {
		char* cb = CB();
		if (cb) {
			g.sparedPlace = *(uint8_t*)(cb + 0x50);
			uint8_t hp = g.st[hero].hp;
			*(uint8_t*)(cb + 0x50) = hp ? hp : 1;
			g.sparing = true;
			return kSpareScope;
		}
	}
	// Game over: one health for both again (0, then what the respawn gives) --
	// a hero ticking with its own 0 after the respawn would die once more.
	if (!g.active || g.gameOver || (hero != HERO_ASTERIX && hero != HERO_OBELIX)) return -1;
	int prev = g.ctx;
	Use(hero);
	if (prev < 0) prev = hero;   // a scope is open either way (IndivLeave closes it)
	g.depth++;
	return prev;
}

// What the game sees outside any one hero's business: a hero still standing.
static int StandingOr(int idx) {
	if (idx < 0 || g.gameOver || !g.fallen[idx] || g.fallen[Other(idx)]) return idx;
	return Other(idx);
}

void IndivLeave(int prev) {
	if (prev == kSpareScope) {
		char* cb = CB();
		if (g.sparing && cb) *(uint8_t*)(cb + 0x50) = g.sparedPlace;
		g.sparing = false;
		return;
	}
	if (!g.active || prev < 0) return;
	if (g.depth > 0) g.depth--;
	// Still inside another hero's business: that hero's own, fallen or not --
	// player 1's fallen hero's HUD tick went on with the standing hero's stats
	// after the shields' refresh (ShieldsShownHook, a scope of its own): its
	// combo bar saw the other hero's hits.
	Use(g.depth > 0 ? prev : StandingOr(prev));
}

bool IndivFallen(int hero) { return g.active && (hero == HERO_ASTERIX || hero == HERO_OBELIX) && g.fallen[hero]; }

bool IndivDropEvent(int hero, int event) {
	return IndivFallen(hero) && ((event >> 8) & 0xFF) == 0x03;
}

bool IndivStats(int hero, int* hp, int* shields, int* hits, float* gauge) {
	if (!g.active || (hero != HERO_ASTERIX && hero != HERO_OBELIX)) return false;
	Stats& s = Current(hero);
	*hp = s.hp;
	*shields = s.shields;
	if (hits) *hits = (int)s.hits;
	if (gauge) *gauge = s.full ? s.gauge : -1.0f;
	return true;
}

void IndivEachComboCopy(IndivComboFn fn) {
	if (g.active)
		for (int i = HERO_ASTERIX; i <= HERO_OBELIX; i++)
			if (i != g.ctx) fn(&g.st[i].hits, &g.st[i].gauge, &g.st[i].decay, g.st[i].full);
	if (g.kept) fn(&g.keptStats.hits, &g.keptStats.gauge, &g.keptStats.decay, g.keptStats.full);
}

// A launcher needs both heroes and may stand where the fallen one can't be
// reached: the hero at it revives its partner there and then.
void IndivReviveAtLauncher(int up) {
	if (!g.active || g.gameOver || (up != HERO_ASTERIX && up != HERO_OBELIX)) return;
	int down = Other(up);
	if (!g.fallen[down] || g.fallen[up]) return;
	Trio t = GetTrio();
	Hero h = t ? t.GetHero(down) : Hero();
	Hero o = t ? t.GetHero(up) : Hero();
	if (!h || !o) return;
	Vector3 p = o.Pos();
	p.y += 1.0f;
	PlaceHero(h, p);
	Revive(down, h, "its partner is at a launcher");
}

bool IndivHeroDown(int hero, const char* how) {
	if (!g.active || (hero != HERO_ASTERIX && hero != HERO_OBELIX)) return false;
	if (g.fallen[hero]) return true;
	int other = Other(hero);
	Trio t = GetTrio();
	Hero h = t ? t.GetHero(hero) : Hero();
	if (!h) return false;
	// Single player's second chance: a fall on the belly slide or the boat (either
	// hero in mode 3 / 1 -- a ride both are on) is the game over at once, as in
	// co-op's scripted modes (user: the slide's death started a revive).
	Hero o = t.GetHero(other);
	bool soloRide = g.solo && o && (HeroMode(h.Flags18()) == 1 || HeroMode(h.Flags18()) == 3 ||
	                                HeroMode(o.Flags18()) == 1 || HeroMode(o.Flags18()) == 3);
	if (CoopSuspended() || soloRide || g.fallen[other] || Current(other).hp == 0) {
		// Both down (or a fall on a ride): the game over of single player, with
		// the game manager seeing no health whoever leads.
		g.gameOver = true;
		g.st[other].hp = 0;
		Commit(other);
		Log("indiv: %s is down too (%s)%s -- game over", HeroName(hero), how,
		    CoopSuspended() || soloRide ? " during a scripted mode" : "");
		HintHide(HINT_REVIVE);
		return false;
	}
	g.fallen[hero] = true;
	g.revive[hero] = 0;
	g_downGoodSet[hero] = false;
	g_downLast[hero] = h.Pos();
	g_dragged[hero] = 0.0f;
	{
		// Its combo bar empties, "full" and all, as the game's respawn empties it
		// (CKAsterixGameManager::vf13) -- a fallen hero kept a full bar through its
		// revive (user). Drained, not zeroed: DrainCombo.
		Stats& s = Current(hero);
		s.chain = s.c4 = 0;
		s.decay = 0.0f;
		Commit(hero);
		g.drain[hero] = s.full || s.hits > 0;
		if (g.drain[hero]) Log("indiv: %s's combo bar drains (down: %s, %u hits)", HeroName(hero), s.full ? "full" : "not full", s.hits);
	}
	g.fellLeading[hero] = g.lastLead == hero;
	g.levelLead[hero] = false;
	g.fellAt[hero] = GetTickCount();
	g.cutOff[hero] = 0;
	g.reachTold[hero] = -2;
	g.hadWay[hero] = false;
	// A fixed revive time limit counts from the fall; "auto" from the first way
	// known (WayToFallen).
	g.limitSet[hero] = g_config.reviveLimit > 0.0f;
	g.limitLeft[hero] = g_config.reviveLimit;
	if (g.limitSet[hero]) Log("indiv: %s has %.0f s to revive %s", HeroName(other), g.limitLeft[hero], HeroName(hero));
	Vector3 spot;
	if (OnDeathGround(h) && SafeSpotFor(hero, &spot)) {
		Log("indiv: %s fell into a pit at (%.1f %.1f %.1f) -- laid on its last solid ground (%.1f %.1f %.1f)",
		    HeroName(hero), h.Pos().x, h.Pos().y, h.Pos().z, spot.x, spot.y, spot.z);
		spot.y += 0.5f;
		PlaceHero(h, spot);
	}
	Log("indiv: %s is down (%s)%s -- %s plays on and can revive it", HeroName(hero), how,
	    g.fellLeading[hero] ? " leading" : "", HeroName(other));
	// It led only because the undone sequence's event gave it the lead (the
	// spinning platform's circle: 4801): no lead back after the revive -- the
	// circle answers only the leader, and Asterix's stayed dead.
	int before = CoopUndoLockFor(hero);
	if (g.fellLeading[hero] && before >= 0 && before != hero) {
		g.fellLeading[hero] = false;
		Log("indiv: %s led only for the undone sequence -- no lead back after the revive", HeroName(hero));
	}
	ShowDownHint(hero);
	return true;
}

void IndivAfterFallenTick(int hero) {
	if (!IndivFallen(hero) || g.gameOver) return;
	// Its death's end (situation 0x0E with no health) asks the game manager for
	// the game over (+0x2FD); only the game over of both is one.
	char* gm = GM();
	if (gm) *(uint8_t*)(gm + 0x2FD) = 0;
}

void IndivAfterHeroTick(int hero) {
	int down = IndivFallenHero();
	if (down >= 0 && hero != down) BlockPartnerGrounds();
}

void IndivFrame() {
	{
		Trio tf = GetTrio();
		for (int i = 0; tf && i < 2; i++) {
			Hero h = tf.GetHero(i);
			if (h) g.frameStart[i] = h.Pos();
		}
	}
	{
		DWORD now = GetTickCount();
		Trio t = GetTrio();
		if (t && now - g.recentAt >= 100) {
			g.recentAt = now;
			for (int i = 0; i < 2; i++) {
				Hero h = t.GetHero(i);
				if (!h || g.fallen[i]) continue;
				g.recent[i][g.recentNext[i]] = h.Pos();
				g.recentNext[i] = (g.recentNext[i] + 1) % 20;
				if (g.recentN[i] < 20) g.recentN[i]++;
			}
		}
	}
	BlockPartnerGrounds();
	DWORD now = GetTickCount();
	float dt = g.lastFrame ? (now - g.lastFrame) / 1000.0f : 0.0f;
	if (dt > 0.1f) dt = 0.1f;
	g.lastFrame = now;
	char* gm = GM();
	int level = gm ? *(int*)(gm + ax::kGM_currentLevel) : -1;
	if (level != g.level) {
		// A new level: the old heroes are gone, the game sets the health itself.
		g.level = level;
		g.active = false;
		g.ctx = -1;
		g.gameOver = false;
		memset(g.fallen, 0, sizeof(g.fallen));
		memset(g.drain, 0, sizeof(g.drain));
		memset(g.fellLeading, 0, sizeof(g.fellLeading));
		memset(g.levelLead, 0, sizeof(g.levelLead));
		memset(g.brought, 0, sizeof(g.brought));
		memset(g.limitSet, 0, sizeof(g.limitSet));
		memset(g.pathN, 0, sizeof(g.pathN));
		g.spared = -1;
		g.sparing = false;
		g.leadBack = g.sendBack = -1;
	}
	Trio t = GetTrio();
	// Co-op: individual_heroes. Single player: sp_second_chance, from the moment
	// Obelix is with Asterix (level 1: hero +0x1A until he is found). Co-op begun
	// or ended: afresh, as the other way of playing.
	bool solo = !CoopP2Joined();
	Hero ob = t ? t.GetHero(HERO_OBELIX) : Hero();
	bool want = (solo ? g_config.spSecondChance : g_config.individual) && t && t.GetHero(HERO_ASTERIX) && ob &&
	            (g.active || *(uint8_t*)(ob.raw() + 0x1A) == 0);
	if (g.active && g.solo != solo) Deactivate(t);
	if (want && !g.active) { g.solo = solo; Activate(t); }
	if (!want && g.active) Deactivate(t);
	if (!g.active) {
		// A game over in single player starts everyone afresh.
		char* cb0 = CB();
		if (g.kept && cb0 && *(uint8_t*)(cb0 + 0x50) == 0) {
			g.kept = false;
			Log("indiv: game over -- player 2's kept stats dropped");
		}
		return;
	}
	if (!t) return;
	if (CoopP2Hero() >= 0) g.p2Hero = CoopP2Hero();
	TrackBosses(level, GetTickCount());

	int lead = t.Leader();
	if (lead != HERO_ASTERIX && lead != HERO_OBELIX) return;
	g.lastLead = lead;
	char* cb = CB();
	if (g.gameOver) {   // the game's values are everyone's until the respawn
		if (cb && *(uint8_t*)(cb + 0x50) > 0) Respawned();
		return;
	}
	Use(StandingOr(lead));   // the game manager, the HUD and the rest see the leader's (standing)
	RunDownCombo(Other(lead));
	for (int i = 0; i < 2; i++) DrainCombo(i, dt);

	// The hero brought to its fallen leader goes back once it has revived it by
	// the grab -- unless it has gone on more than 10 units from where it did.
	// Waits out a suspension (the revive by the grab suspends co-op for a frame as
	// the hero gets up -- lv2-3_tp_back.log: dropped there).
	if (g.sendBack >= 0 && !CoopSuspended()) {
		int who = g.sendBack;
		g.sendBack = -1;
		g.brought[who] = false;
		Hero b = t.GetHero(who);
		Vector3 d = b ? b.Pos() - g.broughtTo[who] : Vector3();
		if (!b || g.fallen[who]) {
			Log("indiv: %s stays where it was brought", HeroName(who));
		} else if (d.len2() > 10.0f * 10.0f) {
			Log("indiv: %s has gone on from where it revived its leader (%.0f units) -- stays", HeroName(who), sqrtf(d.len2()));
		} else {
			Vector3 back = g.broughtFrom[who];
			back.y += 0.5f;
			PlaceHero(b, back);
			g.placedAt[who] = now;
			Log("indiv: %s revived its partner -- back where it was brought from (%.1f %.1f %.1f)", HeroName(who), back.x,
			    back.y, back.z);
		}
	}
	// A hero revived that led when it fell gets the lead back.
	if (g.leadBack >= 0 && !CoopSuspended()) {
		int back = g.leadBack;
		g.leadBack = -1;
		if (!g.fallen[back] && lead == Other(back)) {
			CoopGiveLead(back, "revived, it led when it fell");
			lead = t.Leader();
			Use(StandingOr(lead));
		}
	}
	// Single player: the companion's wait the fall undid, back once the revived
	// hero leads (coop.cpp CoopRestoreSoloPark) -- after its send-back.
	if (g.parkRevived >= 0 && !CoopSuspended() && g.sendBack < 0 && g.leadBack < 0) {
		int back = g.parkRevived;
		if (g.fallen[back]) g.parkRevived = -1;
		else if (lead == back) {
			g.parkRevived = -1;
			if (CoopRestoreSoloPark(back)) g.brought[Other(back)] = false;
		} else if (now - g.parkRevivedAt > 2000) {
			g.parkRevived = -1;
			CoopDropSoloPark("the revived hero does not lead");
		}
	}

	// The lead stays with a hero still standing -- once it is in a mode a player
	// drives (Asterix alone on the machine gun, mode 2, not leading: the switch
	// takes no hero out of it; he gets it when he leaves the gun).
	Hero standing = t.GetHero(Other(lead));
	if (g.fallen[lead] && !g.fallen[Other(lead)] && !CoopSuspended() && standing && HeroModePlayable(standing.Flags18())) {
		CoopGiveLead(Other(lead), "the leader is down");
		lead = t.Leader();
		Use(lead);
	}

	CKYellowPages* yp = YellowPages();
	char* lvl = yp ? *(char**)((char*)yp + 0x90) : nullptr;
	int sector = lvl ? *(int*)(lvl + 0x24) : -1;
	bool sectorChanged = g.lastSector >= 0 && sector > 0 && sector != g.lastSector;
	if (sector > 0) g.lastSector = sector;

	for (int i = 0; i < 2; i++) {
		Hero h = t.GetHero(i);
		if (!h) continue;
		if (!g.fallen[i]) {
			NoteSafeSpot(i, h, now);
			continue;
		}
		int up = Other(i);
		Hero o = t.GetHero(up);
		if (!o || g.fallen[up]) continue;
		if (sectorChanged) {
			Vector3 p = CoopJoinSpot(up);
			PlaceHero(h, p);
			Revive(i, h, "the other hero went on into another area");
			continue;
		}
		KeepBodyOnGround(i, h, now);
		{
			// Left behind a rolling boss (its trail between them), the other hero not
			// behind it too: the game over at once (user).
			int which = -1, w2 = -1;
			float back = BehindBoss(h.Pos(), now, &which);
			if (back >= 0.0f && !CoopSuspended() && BehindBoss(o.Pos(), now, &w2) < 0.0f) {
				char why[96];
				sprintf(why, "lies behind the rolling boss #%d (%.0f units back on its way)", which, back);
				TimeOut(i, h, up, why);
				return;
			}
			// Dragged on by it.
			Vector3 m = h.Pos() - g_downLast[i];
			g_downLast[i] = h.Pos();
			int nearBoss = BossNear(h.Pos(), kBossDragNear, now);
			if (nearBoss >= 0 && m.len2() < 5.0f * 5.0f) g_dragged[i] += sqrtf(m.x * m.x + m.z * m.z);
			if (nearBoss >= 0 && g_dragged[i] >= kBossDragged && !CoopSuspended()) {
				char why[96];
				sprintf(why, "is dragged on by the rolling boss #%d (%.1f units)", nearBoss, g_dragged[i]);
				TimeOut(i, h, up, why);
				return;
			}
		}
		WayToFallen(i, h, up, o, now);
		Vector3 d = h.Pos() - o.Pos();
		bool close = d.x * d.x + d.z * d.z < kReviveRange * kReviveRange && fabsf(d.y) < kReviveHeight;
		bool holding = close && CoopHoldTime(PlayerOf(up), 6) > 0.0f;
		float hold = HoldSeconds(h.Pos());
		if (holding) {
			g.revive[i] += dt;
			wchar_t text[128], bar[24];
			int n = (int)(g.revive[i] / hold * 20.0f);
			if (n > 20) n = 20;
			// The game's font has '=' and '-' (no '|', '#' or '%').
			for (int k = 0; k < 20; k++) bar[k] = k < n ? L'=' : L'-';
			bar[20] = 0;
			swprintf(text, 128, L"Reviving %s\n%s", HintHeroName(i), bar);
			HintShow(HINT_REVIVE, PlayerOf(up), text, 500);
			if (g.revive[i] >= hold) {
				Revive(i, h, hold < g_config.reviveHold ? "revived by a catapult" : "revived", true);
				continue;
			}
		} else {
			if (g.revive[i] > 0.0f) HintHide(HINT_REVIVE, PlayerOf(up));
			g.revive[i] = 0.0f;
			if (close && now - g.hintAt > 400) {
				g.hintAt = now;
				wchar_t text[128];
				swprintf(text, 128, L"Hold %s to revive %s", HintButtonName(PlayerOf(up), HB_B), HintHeroName(i));
				HintShow(HINT_REVIVE, PlayerOf(up), text, 600);
			}
		}
		if (TimeLimit(i, h, up, holding, dt, now)) return;   // the game over
	}
}

bool IndivInstallHooks() {
	bool ok = InstallDetour(kHeal, (void*)HealHook, (void**)&g_origHeal, kGmPrologue, sizeof(kGmPrologue),
	                        "heal (individual heroes)");
	ok &= PatchVtableSlot(0x62808C, 39, 0x507320, (void*)MayCollectHook, (void**)&g_origMayCollect,
	                      "CKGrpAsterixBonusPool::vf39 (only a hero the health bonus is of use to)");
	ok &= PatchVtableSlot(0x62808C, 40, 0x506E90, (void*)CollectHook, (void**)&g_origCollect,
	                      "CKGrpAsterixBonusPool::vf40 (a bonus collected: the collector's stats)");
	ok &= InstallDetour(kAddShield, (void*)AddShieldHook, (void**)&g_origAddShield, kGmPrologue, sizeof(kGmPrologue),
	                    "add a shield (individual heroes)");
	return ok;
}
