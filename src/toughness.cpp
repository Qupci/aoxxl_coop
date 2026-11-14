#include "toughness.h"
#include "experimental.h"
#include "guard.h"
#include "individual.h"
#include "config.h"
#include "coop.h"
#include "kal.h"
#include "trio.h"
#include "log.h"
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <unordered_map>

namespace {

// --- The enemies ---------------------------------------------------------
// A squad's pool records (squad +0x144, 0x1C each, count +0x14C) hold the
// pool's enemy component at +4, shared by its Romans (a Roman's hook +0x54):
//   CKEnemyCpnt          +0x04 flags (0x10 has a shield, 0x40 infinite shield,
//                        0x08 can be disarmed), +0x08 health (a Roman's hook
//                        takes it as it is sent in: Shared_Class_2_88_vf16 ->
//                        hook +0x48)
//   CKSeizableEnemyCpnt  +0xF0 the disarmed daze (the editor's seComboStunTime:
//                        hook +0x24 bit 0x8000 -- no weapon, no shield -- ends
//                        once hook +0x128 passes it, vf52 0x4CD260), +0x110
//                        shield points, +0x111 hits that land after them
//   CKBasicEnemyCpnt     +0x154 its wind-up (beChargeDuration: reaction 0x2A
//                        while hook +0x260 is below it, FUN_004E3F60; the
//                        animation at 0.9 / it, CKHkBasicEnemy::vf36)
// The hit code (Shared_Class_2_84_vf28 0x4CF970, a front hit, the component
// with a shield, the Roman not reacting 1..5): its hits in a row (+0x148) up
// to the points are blocked; past points + extra only with the infinite flag;
// never while disarmed (+0x24 bit 0x8000) or a strong blow (kind 3).
const uintptr_t kBasicEnemyCpnt  = 0x627BD0;
const uintptr_t kLeaderCpnt      = 0x627C30;
const uintptr_t kRocketCpnt      = 0x627C60;
const uintptr_t kJumpingCpnt     = 0x627CC0;
const uintptr_t kArcherCpnt      = 0x627CF0;
const int       kShieldPoints    = 2;   // a shield given to a Roman without one (as a guard's: a 3-hit combo gets through)
const int       kInfiniteExtra   = 2;   // ... an infinite one: LV2-3 squad 25's (flags 0x50, points 0, 2 extra)

bool Seizable(uintptr_t vt) { return vt == kBasicEnemyCpnt || vt == kLeaderCpnt || vt == kRocketCpnt || vt == kJumpingCpnt || vt == kArcherCpnt; }
bool Basic(uintptr_t vt) { return vt == kBasicEnemyCpnt || vt == kLeaderCpnt || vt == kRocketCpnt; }

struct Fields {
	uint32_t flags; uint8_t health, points, extra; float disarm, charge;
	bool operator==(const Fields& o) const {
		return flags == o.flags && health == o.health && points == o.points && extra == o.extra && disarm == o.disarm && charge == o.charge;
	}
};
struct Cpnt {
	uintptr_t vt;
	Fields orig, set;   // as the level has them; as put in last
	int shieldKind;     // a shield may be given: 1 yes, 0 no (lions, pirates, villagers, the tamer...), -1 not known yet
};
std::unordered_map<char*, Cpnt> g_cpnts;
int g_told = 0;

Fields Read(char* c, uintptr_t vt) {
	Fields f = {};
	f.flags = *(uint32_t*)(c + 4);
	f.health = *(uint8_t*)(c + 8);
	if (Seizable(vt)) {
		f.points = *(uint8_t*)(c + 0x110);
		f.extra = *(uint8_t*)(c + 0x111);
		f.disarm = *(float*)(c + 0xF0);
	}
	if (Basic(vt)) f.charge = *(float*)(c + 0x154);
	return f;
}

void Put(char* c, uintptr_t vt, const Fields& f) {
	*(uint32_t*)(c + 4) = f.flags;
	*(uint8_t*)(c + 8) = f.health;
	if (Seizable(vt)) {
		*(uint8_t*)(c + 0x110) = f.points;
		*(uint8_t*)(c + 0x111) = f.extra;
		*(float*)(c + 0xF0) = f.disarm;
	}
	if (Basic(vt)) *(float*)(c + 0x154) = f.charge;
}

// A Roman of pool record `rec`: one sent in, else the pool's first free one (pool vf12: its list).
char* PoolRoman(char* rec) {
	if (*(char**)(rec + 8)) return *(char**)(rec + 8);
	char* pool = *(char**)rec;
	if (!pool) return nullptr;
	__try {
		typedef char** (__fastcall* ListFn)(char* self, void* edx);
		char** head = ((ListFn)(*(void***)pool)[12])(pool, nullptr);
		return head ? *head : nullptr;
	} __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// May the pool's enemies have a shield: Romans (basic, leaders), not the
// ones that only share their component's class -- by the model's textures.
int ShieldKind(char* rec, uintptr_t vt) {
	if (vt != kBasicEnemyCpnt && vt != kLeaderCpnt) return 0;
	char* e = PoolRoman(rec);
	if (!e || IsBadReadPtr(e, 0x60)) return -1;
	char tex[256] = {};
	GuardModelTextures(e, tex, sizeof(tex));
	if (!tex[0]) return -1;
	static const char* const kNot[] = { "lion", "pirate", "normand", "dompt", "barberousse" };
	for (const char* n : kNot)
		if (strstr(tex, n)) return 0;
	return 1;
}

Fields Wanted(const Cpnt& c, bool on) {
	Fields f = c.orig;
	if (!on) return f;
	if (g_config.romanHealth != 1.0f) {
		int h = (int)ceilf(c.orig.health * g_config.romanHealth - 0.001f);
		f.health = (uint8_t)(h < 1 ? 1 : h > 255 ? 255 : h);
	}
	if (Seizable(c.vt)) {
		// A shield the level gave already counts as it is (a breakable one
		// made infinite); a Roman without one gets a breakable shield of
		// kShieldPoints, or LV2-3 squad 25's infinite one (no points, 2 extra:
		// two blows of a row land, the rest are blocked).
		bool own = (c.orig.flags & 0x10) && (c.orig.points || (c.orig.flags & 0x40));
		if (g_config.romanShield && c.shieldKind == 1) {
			f.flags |= 0x10;
			if (g_config.romanShield == 2) {
				f.flags |= 0x40;
				if (!own) { f.points = 0; f.extra = kInfiniteExtra; }
			} else if (!own) {
				f.points = kShieldPoints;
				f.extra = 0;
			}
		}
		if ((f.flags & 0x10) && f.points && g_config.romanShieldHits != 1.0f) {
			int p = (int)floorf(f.points * g_config.romanShieldHits + 0.5f);
			f.points = (uint8_t)(p < 1 ? 1 : p > 255 ? 255 : p);
		}
		if (g_config.romanDisarmedTime != 1.0f) f.disarm = c.orig.disarm * g_config.romanDisarmedTime;
	}
	if (Basic(c.vt) && g_config.romanChargeSpeed > 1.0f) f.charge = c.orig.charge / g_config.romanChargeSpeed;
	return f;
}

bool AnyEnemyRule() {
	return g_config.romanChargeSpeed != 1.0f || g_config.romanShield || g_config.romanHealth != 1.0f ||
	       g_config.romanShieldHits != 1.0f || g_config.romanDisarmedTime != 1.0f;
}

void EnemyFrame() {
	if (!KalLevelIsLoaded()) {
		g_cpnts.clear();
		return;
	}
	// (Once anything was put in, it is kept up: rules back to 1 put the level's values back.)
	if (g_cpnts.empty() && !AnyEnemyRule()) return;
	bool on = ExperimentalMode() == 2;
	static int s_on = -1;
	if ((int)on != s_on && g_told++ < 60)
		Log("toughness: enemies %s", on ? "with the rules" : "as the level has them");
	s_on = on;
	int shielded = 0, notRomans = 0;
	const unsigned classes[2] = { 26, 64 };
	for (unsigned cl : classes)
		for (unsigned i = 0, k = GetNumLvlClassInstances(4, cl); i < k; i++) {
			char* sq = (char*)GetLvlObject(4, cl, i);
			char* recs = sq ? *(char**)(sq + 0x144) : nullptr;
			for (unsigned r = 0, nr = recs ? *(unsigned*)(sq + 0x14C) : 0; r < nr && r < 8; r++) {
				char* rec = recs + 0x1C * r;
				char* cp = *(char**)(rec + 4);
				if (!cp || IsBadReadPtr(cp, 0x174)) continue;
				uintptr_t vt = *(uintptr_t*)cp;
				if (EnemyKindKeptAsIs(cp)) continue;   // turtles, the mobile tower, jetpack Romans
				Fields now = Read(cp, vt);
				auto it = g_cpnts.find(cp);
				if (it == g_cpnts.end() || it->second.vt != vt || !(it->second.set == now)) {
					// New (or loaded again): what it holds is the level's.
					Cpnt c = { vt, now, now, -1 };
					it = g_cpnts.insert_or_assign(cp, c).first;
				}
				Cpnt& c = it->second;
				if (c.shieldKind < 0 && on && g_config.romanShield) {
					c.shieldKind = ShieldKind(rec, vt);
					if (c.shieldKind >= 0) (c.shieldKind ? shielded : notRomans)++;
				}
				Fields w = Wanted(c, on);
				if (!(w == now)) Put(cp, vt, w);
				c.set = w;
			}
		}
	if ((shielded || notRomans) && g_told++ < 60)
		Log("toughness: %d Roman pools given shields, %d pools of basic enemies that are no Romans (lions, pirates, villagers...) left without", shielded, notRomans);
}

// Test aid (env XXL_TOUGHWATCH): the Romans (2,93) within 15 units of a hero --
// their health (hook +0x48) as it drops, a blow blocked (reaction +0x1A4 0x1B),
// each wind-up (reaction 0x2A) and disarmed spell (+0x24 bit 0x8000) with how
// long it lasted. Bounded.
struct Watch { char* roman; uint8_t health, react; bool disarmed, blocking; DWORD chargeAt, disarmAt; float dazeMax; DWORD seen; };
Watch g_watch[32];
int g_nWatch = 0, g_watchTold = 0;

void WatchFrame() {
	static int s_on = -1;
	if (s_on < 0) s_on = getenv("XXL_TOUGHWATCH") != nullptr;
	if (!s_on || !KalLevelIsLoaded() || g_watchTold > 400) return;
	DWORD now = GetTickCount();
	CKYellowPages* yp = YellowPages();
	if (!yp) return;
	for (int k = 0; k < g_nWatch; k++)   // (gone 2 s: its slot freed)
		if (now - g_watch[k].seen > 2000) g_watch[k--] = g_watch[--g_nWatch];
	for (unsigned i = 0, n = GetNumLvlClassInstances(2, 93); i < n; i++) {
		char* e = (char*)GetLvlObject(2, 93, i);
		char* node = e ? *(char**)(e + 0x10) : nullptr;
		char* fr = node ? *(char**)(node + 4) : nullptr;
		if (!fr || (*(uint8_t*)(node + 0x10) & 2) || (*(uint8_t*)(e + 8) & 1) || !*(char**)(e + 0x54)) continue;
		const float* p = (const float*)(fr + 0x80);
		bool close = false;
		Trio t = GetTrio();
		for (int h = HERO_ASTERIX; t && h <= HERO_OBELIX; h++) {
			Hero hh = t.GetHero(h);
			if (!hh) continue;
			float dx = hh.Pos().x - p[0], dz = hh.Pos().z - p[2];
			close |= dx * dx + dz * dz < 15.0f * 15.0f;
		}
		int k = 0;
		while (k < g_nWatch && g_watch[k].roman != e) k++;
		if (k == g_nWatch) {
			if (!close || g_nWatch == 32) continue;
			g_watch[g_nWatch++] = { e, *(uint8_t*)(e + 0x48), *(uint8_t*)(e + 0x1A4), false, false, 0, 0, 0.0f, now };
			char* c = *(char**)(e + 0x54);
			if (g_watchTold++ < 40) Log("toughwatch: Roman %p health %u (cpnt %p: health %u, flags %08X, shield %u +%u, charge %.2f, disarmed %.2f)", e,
			    *(uint8_t*)(e + 0x48), c, *(uint8_t*)(c + 8), *(uint32_t*)(c + 4), *(uint8_t*)(c + 0x110), *(uint8_t*)(c + 0x111),
			    *(uintptr_t*)c == kBasicEnemyCpnt ? *(float*)(c + 0x154) : 0.0f, *(float*)(c + 0xF0));
		}
		Watch& w = g_watch[k];
		w.seen = now;
		uint8_t hp = *(uint8_t*)(e + 0x48), re = *(uint8_t*)(e + 0x1A4);
		bool dis = (*(uint32_t*)(e + 0x24) & 0x8000) != 0;
		bool blk = (*(uint32_t*)(e + 0x24) & 0x10000) != 0;   // the hit code's "blocked" (cover)
		if (hp != w.health) { g_watchTold++; Log("toughwatch: Roman %p health %u -> %u (hits in a row %u)", e, w.health, hp, *(uint8_t*)(e + 0x148)); }
		if (blk && !w.blocking) { g_watchTold++; Log("toughwatch: Roman %p blocks (hits in a row %u)", e, *(uint8_t*)(e + 0x148)); }
		w.blocking = blk;
		if (re != w.react) {
			if (re == 0x2A) w.chargeAt = now;
			if (w.react == 0x2A && w.chargeAt) {
				g_watchTold++;
				Log("toughwatch: Roman %p wind-up %.2f s (-> %02X)", e, (now - w.chargeAt) / 1000.0f, re);
			}
		}
		if (dis && *(float*)(e + 0x128) > w.dazeMax) w.dazeMax = *(float*)(e + 0x128);   // the daze state's clock (vf52)
		if (dis != w.disarmed) {
			if (dis) { w.disarmAt = now; w.dazeMax = 0.0f; }
			g_watchTold++;
			if (dis) Log("toughwatch: Roman %p disarmed", e);
			else Log("toughwatch: Roman %p armed again after %.2f s (its daze clock reached %.2f s)", e, (now - w.disarmAt) / 1000.0f, w.dazeMax);
		}
		w.health = hp;
		w.react = re;
		w.disarmed = dis;
	}
}

// --- The heroes' health bar ----------------------------------------------
// Game manager +0x40 -> +0x50 health points (5 a shield), +0x51 shields. A
// hero has 3 shields for good: one bought or picked up over them is lost as
// its part empties (the hero's hit, FUN_00495FA0: shields > 3 and health at
// shields * 5 - 5 or less -> one fewer), up to 6. The game's 3 (that floor;
// CKAsterixGameManager::vf12 at the start, vf13 the respawn, vf18 a new game:
// health 15 there) and 6 (add a shield FUN_00507210, the bonus pool's
// FUN_004BAFB0, the merchant FUN_004BFC50) are immediates, put in as the
// options change. A pit (FUN_0049DD30) takes a shield whatever: left so.
struct Imm { uintptr_t at; uint8_t orig; int what; };   // what: 0 the shields to start with, 1 their health, 2 the most
const Imm kImms[] = {
	{ 0x501467, 0x0F, 1 }, { 0x50146B, 0x03, 0 },   // vf12: health 15, 3 shields
	{ 0x501B2F, 0x0F, 1 }, { 0x501B5F, 0x03, 0 },   // vf13 (respawn): health 15, 3 shields
	{ 0x4FE170, 0x03, 0 }, { 0x4FE174, 0x0F, 1 },   // vf18 (a new game): 3 shields, health 15
	{ 0x49611E, 0x03, 0 },                          // a hit: a shield lost only over 3
	{ 0x507232, 0x06, 2 }, { 0x507238, 0x06, 2 },   // add a shield: below 6 +1, else 6
	{ 0x4BB050, 0x06, 2 }, { 0x4BB0A9, 0x06, 2 }, { 0x4BB0AF, 0x06, 2 },   // bonus pool: a shield only below 6
	{ 0x4BFD5E, 0x06, 2 }, { 0x4C001F, 0x06, 2 }, { 0x4C0025, 0x06, 2 },   // merchant: none to sell at 6, bought below 6
};
const int kNumImms = sizeof(kImms) / sizeof(kImms[0]);
bool g_immsOk = false;
int g_base = 3, g_max = 6;

void PutImm(uintptr_t at, uint8_t v) {
	uint8_t* p = (uint8_t*)at;
	if (*p == v) return;
	DWORD old;
	if (!VirtualProtect(p, 1, PAGE_EXECUTE_READWRITE, &old)) return;
	*p = v;
	VirtualProtect(p, 1, old, &old);
	FlushInstructionCache(GetCurrentProcess(), p, 1);
}

int g_clampBase = 3, g_clampMax = 6;
bool g_clamped = false;
bool g_raise = false;   // ... up to the shields to start with too (the option changed, a level begun)
int g_clampTold = 0;
void Clamp(uint8_t* hp, uint8_t* shields) {
	int s = *shields, h = *hp;
	if (s < g_clampBase && g_raise) s = g_clampBase;
	if (s > g_clampMax) s = g_clampMax;
	if (h > s * 5) h = s * 5;
	if (s != *shields || h != *hp) {
		if (g_clampTold++ < 40)
			Log("toughness: a health bar brought within %d..%d shields: %u shields, health %u -> %d shields, health %d",
			    g_clampBase, g_clampMax, *shields, *hp, s, h);
		*shields = (uint8_t)s;
		*hp = (uint8_t)h;
		g_clamped = true;
	}
}

void HeroFrame() {
	bool on = ExperimentalMode() != 0;
	int base = on ? (int)g_config.heroShields : 3;
	int extra = on ? (int)g_config.heroExtraShields : 3;
	base = base < 1 ? 1 : base > 6 ? 6 : base;
	int most = base + (extra < 0 ? 0 : extra);
	most = most > 6 ? 6 : most;   // (the HUD has six)
	if (g_immsOk && (base != g_base || most != g_max)) {
		for (const Imm& m : kImms)
			PutImm(m.at, (uint8_t)(m.what == 0 ? base : m.what == 1 ? base * 5 : most));
		Log("toughness: the heroes' health bar: %d shields to start with, %d at most%s", base, most, on ? "" : " (the game's)");
	}
	static bool s_inLevel = false;
	bool raise = base != g_base || !s_inLevel;
	g_base = base;
	g_max = most;
	s_inLevel = KalLevelIsLoaded();
	if (!s_inLevel) return;
	CKYellowPages* yp = YellowPages();
	char* gm = yp ? (char*)yp->gameManager : nullptr;
	char* cb = gm ? *(char**)(gm + 0x40) : nullptr;
	if (!cb) { s_inLevel = false; return; }
	// Each frame no more than the most (the HUD has six; a lower most as it
	// changes); up to the shields to start with only as they change or a level
	// begins -- a pit's lost shield stays lost, as in the game.
	g_raise = raise && on;   // (the game's own: as it has them)
	g_clampBase = base;
	g_clampMax = most;
	g_clamped = false;
	Clamp((uint8_t*)(cb + 0x50), (uint8_t*)(cb + 0x51));
	if (g_clamped) *(uint32_t*)(gm + 0x5C) |= 0x4000;   // the HUD reads the health again
	IndivEachShieldCopy(Clamp);
}

} // namespace

int ToughnessHeroShieldsMax() { return g_max; }

void ToughnessFrame() {
	EnemyFrame();
	WatchFrame();
	HeroFrame();
}

bool ToughnessInstallHooks() {
	g_immsOk = true;
	for (const Imm& m : kImms)
		if (*(const uint8_t*)m.at != m.orig) {
			g_immsOk = false;
			Log("toughness: %08X holds %02X, not %02X -- the heroes' shields stay the game's", (unsigned)m.at, *(const uint8_t*)m.at, m.orig);
			break;
		}
	return g_immsOk;
}
