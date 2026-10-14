#include "individual.h"
#include "coop.h"
#include "config.h"
#include "detour.h"
#include "hints.h"
#include "kal.h"
#include "life.h"
#include "log.h"
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
};

struct SafeSpot { Vector3 pos; DWORD at = 0; };
const int kSafeSpots = 8;

const float kReviveSeconds = 10.0f;
const float kReviveRange = 3.0f;      // horizontally
const float kReviveHeight = 2.5f;

struct State {
	bool     active = false;
	int      level = -2;
	Stats    st[2];
	int      ctx = -1;                 // whose stats are in the game's place (-1: the game's own)
	bool     fallen[2] = {};
	float    revive[2] = {};           // seconds the other player has held the grab button by it
	bool     gameOver = false;
	SafeSpot safe[2][kSafeSpots];
	int      safeNext[2] = {};
	int      lastSector = -1;
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
int PlayerOf(int hero) { return hero == CoopP1Hero() ? 0 : 1; }

void PlaceHero(Hero h, const Vector3& p) {
	h.Pos() = p;
	if (h.hook->node && h.hook->node->matrix) {
		float* m = (float*)((char*)h.hook->node->matrix + 0x40);
		m[0] = p.x; m[1] = p.y; m[2] = p.z;
	}
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

// Where a hero stood last on solid ground: a static ground (CGround, not a
// lifter's CDynamicGround), not a death pit, on its feet (+0x384 bits 0-2).
void NoteSafeSpot(int idx, Hero h, DWORD now) {
	if (now - g.safe[idx][(g.safeNext[idx] + kSafeSpots - 1) % kSafeSpots].at < 250) return;
	char* ground = *(char**)(h.raw() + 0x280);
	if (!ground || IsBadReadPtr(ground, 0x2C) || *(uintptr_t*)ground != 0x624258) return;
	if ((*(uint8_t*)(ground + 0x2A) & 4) || (*(uint8_t*)(h.raw() + 0x384) & 7) != 0) return;
	if (*(uint8_t*)(h.raw() + 0x3E9) != 0) return;
	SafeSpot& s = g.safe[idx][g.safeNext[idx]];
	s.pos = h.Pos();
	s.at = now;
	g.safeNext[idx] = (g.safeNext[idx] + 1) % kSafeSpots;
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

void ShowDownHint(int down) {
	int up = Other(down);
	wchar_t text[160];
	swprintf(text, 160, L"%s (P%d) is down! %s (P%d):\nhold %s by him for %d s to revive",
	         down == HERO_ASTERIX ? L"Asterix" : L"Obelix", PlayerOf(down) + 1, up == HERO_ASTERIX ? L"Asterix" : L"Obelix",
	         PlayerOf(up) + 1, HintButtonName(PlayerOf(up), HB_B), (int)kReviveSeconds);
	HintShow(HINT_REVIVE, text, 8000);
}

void Revive(int idx, Hero h, const char* how) {
	Stats& s = Current(idx);
	int hp = s.shields * 5 < 5 ? s.shields * 5 : 5;   // one shield back
	s.hp = (uint8_t)(hp > 0 ? hp : 5);
	Commit(idx);
	Raise(h);
	g.fallen[idx] = false;
	g.revive[idx] = 0;
	HintHide(HINT_REVIVE);
	Log("indiv: %s is back on its feet (%s), health %d", HeroName(idx), how, s.hp);
}

// After a game over: both heroes as the respawn left the game's values.
void Respawned() {
	Stats s;
	Read(s);
	g.st[0] = g.st[1] = s;
	g.gameOver = false;
	Trio t = GetTrio();
	for (int i = 0; i < 2; i++) {
		g.fallen[i] = false;
		g.revive[i] = 0;
		Hero h = t ? t.GetHero(i) : Hero();
		if (h) Raise(h);
	}
	HintHide(HINT_REVIVE);
	Log("indiv: respawn -- both heroes at health %d, %d shields", s.hp, s.shields);
}

void Activate(Trio t) {
	Stats s;
	if (!Read(s)) return;
	g.st[0] = g.st[1] = s;
	g.ctx = t.Leader() == HERO_OBELIX ? HERO_OBELIX : HERO_ASTERIX;
	g.active = true;
	g.gameOver = false;
	memset(g.fallen, 0, sizeof(g.fallen));
	memset(g.revive, 0, sizeof(g.revive));
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
	g.kept = false;
	Log("indiv: individual heroes on -- each starts with health %d, %d shields, combo hits %u", s.hp, s.shields, s.hits);
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
		if (s.gauge <= 0.0f) { s.chain = s.hits = s.c4 = 0; s.gauge = 0.0f; }
		else if (s.gauge <= dt) s.gauge = 0.0f;
		else s.gauge -= dt;
	}
}

// Bonuses that heal (FUN_004BB260, thiscall(game manager, points), ret 4) or
// add a shield (FUN_00507210, thiscall(game manager, points), ret 4): the
// bonus's tick runs with the trio's leader byte on the hero that picked it up
// (proximity slicing), and the merchant's with the shopper leading.
typedef char(__fastcall* GmFn)(char* gm, void* edx, int arg);
const uintptr_t kHeal = 0x4BB260, kAddShield = 0x507210;
const uint8_t   kGmPrologue[7] = { 0x56, 0x8B, 0x71, 0x40, 0x8A, 0x46, 0x50 };
GmFn g_origHeal = nullptr, g_origAddShield = nullptr;

int Picker() {
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

bool IndivActive() { return g.active; }

int IndivEnter(int hero) {
	if (!g.active || (hero != HERO_ASTERIX && hero != HERO_OBELIX)) return -1;
	int prev = g.ctx;
	Use(hero);
	return prev;
}

// What the game sees outside any one hero's business: a hero still standing.
static int StandingOr(int idx) {
	if (idx < 0 || g.gameOver || !g.fallen[idx] || g.fallen[Other(idx)]) return idx;
	return Other(idx);
}

void IndivLeave(int prev) {
	if (!g.active || prev < 0) return;
	Use(StandingOr(prev));
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

bool IndivHeroDown(int hero, const char* how) {
	if (!g.active || (hero != HERO_ASTERIX && hero != HERO_OBELIX)) return false;
	if (g.fallen[hero]) return true;
	int other = Other(hero);
	Trio t = GetTrio();
	Hero h = t ? t.GetHero(hero) : Hero();
	if (!h) return false;
	if (CoopSuspended() || g.fallen[other] || Current(other).hp == 0) {
		// Both down (or a fall on a ride): the game over of single player, with
		// the game manager seeing no health whoever leads.
		g.gameOver = true;
		g.st[other].hp = 0;
		Commit(other);
		Log("indiv: %s is down too (%s)%s -- game over", HeroName(hero), how,
		    CoopSuspended() ? " during a scripted mode" : "");
		HintHide(HINT_REVIVE);
		return false;
	}
	g.fallen[hero] = true;
	g.revive[hero] = 0;
	Vector3 spot;
	if (OnDeathGround(h) && SafeSpotFor(hero, &spot)) {
		Log("indiv: %s fell into a pit at (%.1f %.1f %.1f) -- laid on its last solid ground (%.1f %.1f %.1f)",
		    HeroName(hero), h.Pos().x, h.Pos().y, h.Pos().z, spot.x, spot.y, spot.z);
		spot.y += 0.5f;
		PlaceHero(h, spot);
	}
	Log("indiv: %s is down (%s) -- %s plays on and can revive it", HeroName(hero), how, HeroName(other));
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

void IndivFrame() {
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
	}
	Trio t = GetTrio();
	bool want = g_config.individual && CoopP2Joined() && t && t.GetHero(HERO_ASTERIX) && t.GetHero(HERO_OBELIX);
	if (want && !g.active) Activate(t);
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

	int lead = t.Leader();
	if (lead != HERO_ASTERIX && lead != HERO_OBELIX) return;
	Use(StandingOr(lead));   // the game manager, the HUD and the rest see the leader's (standing)
	char* cb = CB();
	if (g.gameOver) {
		if (cb && *(uint8_t*)(cb + 0x50) > 0) Respawned();
		return;
	}
	RunDownCombo(Other(lead));

	// The lead stays with a hero still standing.
	if (g.fallen[lead] && !g.fallen[Other(lead)] && !CoopSuspended()) {
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
			Vector3 p = o.Pos();
			p.y += 1.0f;
			PlaceHero(h, p);
			Revive(i, h, "the other hero went on into another area");
			continue;
		}
		Vector3 d = h.Pos() - o.Pos();
		bool close = d.x * d.x + d.z * d.z < kReviveRange * kReviveRange && fabsf(d.y) < kReviveHeight;
		bool holding = close && CoopHoldTime(PlayerOf(up), 6) > 0.0f;
		if (holding) {
			g.revive[i] += dt;
			wchar_t text[128], bar[24];
			int n = (int)(g.revive[i] / kReviveSeconds * 20.0f);
			if (n > 20) n = 20;
			// The game's font has '=' and '-' (no '|', '#' or '%').
			for (int k = 0; k < 20; k++) bar[k] = k < n ? L'=' : L'-';
			bar[20] = 0;
			swprintf(text, 128, L"Reviving %s\n%s", i == HERO_ASTERIX ? L"Asterix" : L"Obelix", bar);
			HintShow(HINT_REVIVE, text, 500);
			if (g.revive[i] >= kReviveSeconds) Revive(i, h, "revived");
		} else {
			if (g.revive[i] > 0.0f) HintHide(HINT_REVIVE);
			g.revive[i] = 0.0f;
			if (close && now - g.hintAt > 400) {
				g.hintAt = now;
				wchar_t text[128];
				swprintf(text, 128, L"%s (P%d): hold %s to revive %s", up == HERO_ASTERIX ? L"Asterix" : L"Obelix",
				         PlayerOf(up) + 1, HintButtonName(PlayerOf(up), HB_B), i == HERO_ASTERIX ? L"Asterix" : L"Obelix");
				HintShow(HINT_REVIVE, text, 600);
			}
		}
	}
}

bool IndivInstallHooks() {
	bool ok = InstallDetour(kHeal, (void*)HealHook, (void**)&g_origHeal, kGmPrologue, sizeof(kGmPrologue),
	                        "heal (individual heroes)");
	ok &= InstallDetour(kAddShield, (void*)AddShieldHook, (void**)&g_origAddShield, kGmPrologue, sizeof(kGmPrologue),
	                    "add a shield (individual heroes)");
	return ok;
}
