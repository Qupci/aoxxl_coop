#include "config.h"
#include "proximity.h"
#include "coop.h"
#include "detour.h"
#include "klusters.h"
#include "log.h"
#include "trio.h"

#include <cmath>
#include <cstdlib>
#include <intrin.h>
#include <cstring>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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
const uintptr_t kTrioGroupLife   = 0x625984;   // CKGrpTrioLife: the trio itself, always the real leader
const uintptr_t kCheckpointLife  = 0x628B2C;   // CKGrpAsterixCheckpointLife: see CheckpointHook
// Pressure plates (CKHkPressionStone::vf15) react to the object the trio's
// leader stands on (trio vf29). The blue circles that hand control to the other
// hero are such plates, and their event sequence assumes the leader stepped on
// them ("switch to Obelix, Asterix waits here"): set off by player 2's Obelix
// it left Asterix waiting off the platform for good. Plates keep to the real
// leader, as in single player (the AI companion never presses them either).
const uintptr_t kPressionStoneVtbl = 0x628EE4;   // CKHkPressionStone (2,21)
// The spinning platform's blue circle is not a pressure stone but part of the
// platform itself, CKHkCorkscrew: its tick asks the trio about the leader
// standing on it (trio events 0x0B01, 0x0C09) and then switches to Obelix with
// "Asterix waits here". Ticked with player 2's Obelix posing as the leader, it
// switched when Obelix stepped on it and left Asterix waiting off the platform
// -- a softlock. It keeps to the real leader too.
const uintptr_t kCorkscrewVtbl = 0x628E0C;       // CKHkCorkscrew (2,44)
// The spies (CKHkClueMan, 2,161). Their tick (vf15, 0x4B8FB0) spawns the spy
// (FUN_004B12F0, node at hook +0x10) when the trio leader comes within +0x68
// of its spot (+0x12C/+0x130/+0x134 x/y/z) and removes it (FUN_004B1790) when
// the leader goes out of that range. While no spy is spawned there is no node,
// so its tick was never sliced: only the leader made it appear.
const uintptr_t kClueManVtbl = 0x6261F4;

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
		if (*(uintptr_t*)owner == kClueManVtbl) {
			// Spawned, the spy offers its talk (trio 0x511F) to a leader within +0x70
			// of where it stands -- which after a first dialogue can be far from its
			// spot (level 4 sector 2): the hero nearest the spy itself is asked.
			if (NodePos(*(void**)(owner + 0x10), out)) return true;
			const float* p = (const float*)(owner + 0x12C);
			if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
			*out = Vector3(p[0], p[1], p[2]);
			return true;
		}
		return NodePos(*(void**)(owner + 0x10), out);
	}
	for (char* hook = *(char**)(owner + 0x20); hook; hook = *(char**)(hook + 4))
		if (NodePos(*(void**)(hook + 0x10), out)) return true;
	return false;
}

int GroupSwitchTarget(unsigned ev);
bool GroupMessagesTrio(unsigned ev);

// Plain pressure plates (CKHkPressionStone, flags +8: bit 0x10 "activatable by
// hero" -- pressed while the object under the trio leader's feet, trio vf29, is
// the plate's +0x28 --, bit 0x40 by a pushed object) answer either player's
// hero: ticked as usual when the leader is on it, otherwise with the
// non-leading player's hero posing as the leader (level 3 sector 1's plate
// stayed up under player 2's Obelix). Plates whose events (+0x30, +0x32) talk
// to the trio -- the blue circles: "switch to Obelix, Asterix waits here" --
// keep to the real leader, as above.
bool PlateForEither(char* plate) {
	if (!(*(uint32_t*)(plate + 8) & 0x10)) return false;
	unsigned evs[2] = { *(uint16_t*)(plate + 0x30), *(uint16_t*)(plate + 0x32) };
	for (unsigned ev : evs)
		if (GroupMessagesTrio(ev)) return false;
	return true;
}

volatile long g_p2Plates = 0;
char* g_plateLogged = nullptr;

void TickPlate(int* life, void* dt, char* plate, Trio t) {
	TickFn tick = (TickFn)(*(void***)life)[17];
	int nl = CoopNonLeaderPlayer();
	typedef int(__fastcall* GroundFn)(void* trio, void* edx);
	GroundFn ground = (GroundFn)(*(void***)t.raw())[29];
	if (nl < 0 || CoopHeroHeld(nl) || ground(t.raw(), nullptr) == *(int*)(plate + 0x28)) {
		tick(life, nullptr, dt);
		return;
	}
	uint8_t* tr = (uint8_t*)t.raw();
	uint8_t leader = tr[0x2C], companion = tr[0x2D];
	tr[0x2C] = (uint8_t)nl;
	tr[0x2D] = leader;
	bool on = ground(t.raw(), nullptr) == *(int*)(plate + 0x28);
	tick(life, nullptr, dt);
	if (tr[0x2C] == (uint8_t)nl) { tr[0x2C] = leader; tr[0x2D] = companion; }
	if (on) {
		InterlockedIncrement(&g_p2Plates);
		if (g_plateLogged != plate) {
			g_plateLogged = plate;
			Log("proximity: pressure plate %p pressed by %s (not leading)", plate, HeroName(nl));
		}
	}
}

void TickLife(int* life, void* dt, bool isGroup, Trio t, int p1, int p2, const Vector3& pos1,
              const Vector3& pos2) {
	TickFn tick = (TickFn)(*(void***)life)[17];
	uintptr_t vt = *(uintptr_t*)life;
	Vector3 at;
	char* hook = isGroup ? nullptr : *(char**)((char*)life + 0x10);
	if (hook && *(uintptr_t*)hook == kPressionStoneVtbl && PlateForEither(hook)) {
		TickPlate(life, dt, hook, t);
		return;
	}
	bool plate = hook && (*(uintptr_t*)hook == kPressionStoneVtbl || *(uintptr_t*)hook == kCorkscrewVtbl);
	if (vt == kAsterixLifeVtbl || vt == kObelixLifeVtbl || vt == kIdefixLifeVtbl ||
	    vt == kBonusGroupLife || vt == kTrioGroupLife || vt == kCheckpointLife || plate ||
	    !LifePos(life, isGroup, &at)) {
		tick(life, nullptr, dt);
		return;
	}
	float d1 = (at - pos1).len2(), d2 = (at - pos2).len2();
	int nearest = d2 < d1 ? p2 : p1;
	uint8_t* tr = (uint8_t*)t.raw();
	float range2 = kMaxRange2;
	if (hook && *(uintptr_t*)hook == kClueManVtbl) {
		float r = *(const float*)(hook + 0x68);
		if (std::isfinite(r) && r * r > range2) range2 = r * r;
	}
	if ((d1 > range2 && d2 > range2) || tr[0x2C] == nearest) {
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
	// A hero the game has borrowed for a scripted order (Asterix riding the cable
	// car Obelix pulls) is not a player's: objects see the real leader.
	if (!CoopP2Joined() || CoopSuspended() || !h1 || !h2 || CoopHeroHeld(p1) || CoopHeroHeld(p2)) {
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

// Roman squads. They are not ticked one by one: the enemy group (CKGrpEnemy,
// its Life CKGrpEnemyLife, vtable 0x628938, slot 17 -> FUN_0052AC70) asks the
// trio for the leader's position, and for each squad (group +0x0C, chained at
// +4, isSubclass 0x604) runs FUN_0052D340, then the squad's AI (vf31, +0x7C)
// only if the leader is inside the squad's box (squad +0x11C..+0x124 max x/y/z,
// +0x128..+0x130 min); otherwise a squad with +0x152 bit 1 gets vf31 and vf35
// (+0x8C), and the rest just count idle time (+0x138). The non-leading player
// walked through Roman camps unnoticed. Re-implemented: a squad whose box holds
// only the non-leading player's hero runs its AI with that hero as the leader.
typedef void(__fastcall* EnemyTickFn)(char* life, void* edx, char* dt);
const uintptr_t kEnemyLifeVtbl = 0x628938, kEnemyLifeTick = 0x530930, kSquadPrep = 0x52D340;
const size_t    kEnemyLifeSlot = 17;
EnemyTickFn g_origEnemyTick = nullptr;
volatile long g_p2Squads = 0, g_keptSquads = 0;

bool InSquadBox(const char* sq, const Vector3& p) {
	const float* f = (const float*)sq;
	return f[0x4A] < p.x && p.x <= f[0x47] && f[0x4C] < p.z && p.z <= f[0x49] && f[0x4B] < p.y && p.y <= f[0x48];
}

void __fastcall EnemyTickHook(char* life, void* edx, char* dt) {
	Trio t = GetTrio();
	int nl = CoopNonLeaderPlayer();
	char* grp = *(char**)(life + 0x0C);
	Hero lead = t ? t.GetHero(t.Leader()) : Hero(), other = t && nl >= 0 ? t.GetHero(nl) : Hero();
	if (!grp || !lead || !other || !CoopP2Joined() || CoopSuspended() || CoopHeroHeld(nl)) {
		g_origEnemyTick(life, edx, dt);
		return;
	}
	typedef char(__fastcall* IsSubFn)(void* self, void* edx, int id);
	typedef void(__fastcall* VoidFn)(void* self, void* edx);
	typedef void(__fastcall* DtFn)(void* self, void* edx, char* dt);
	Vector3 pl = lead.Pos(), po = other.Pos();
	uint8_t* tr = (uint8_t*)t.raw();
	for (char* sq = *(char**)(grp + 0x0C); sq; sq = *(char**)(sq + 4)) {
		void** vt = *(void***)sq;
		if (!((IsSubFn)vt[1])(sq, nullptr, 0x604)) continue;
		((VoidFn)kSquadPrep)(sq, nullptr);
		if (*(uint8_t*)(grp + 0x10) & 4) continue;
		bool inL = InSquadBox(sq, pl), inO = InSquadBox(sq, po);
		if (inL && inO) {   // big boxes hold both: the hero nearer the box's middle
			const float* f = (const float*)sq;
			Vector3 c((f[0x47] + f[0x4A]) * 0.5f, (f[0x48] + f[0x4B]) * 0.5f, (f[0x49] + f[0x4C]) * 0.5f);
			if ((po - c).len2() < (pl - c).len2()) inL = false;
		}
		if (inL) {
			((DtFn)vt[31])(sq, nullptr, dt);
		} else if (inO) {
			uint8_t leader = tr[0x2C], companion = tr[0x2D];
			tr[0x2C] = (uint8_t)nl;
			tr[0x2D] = leader;
			*(uint32_t*)(tr + 0x3F4) &= ~1u;   // the trio's enemy cache is per leader
			((DtFn)vt[31])(sq, nullptr, dt);
			if (tr[0x2C] == (uint8_t)nl) { tr[0x2C] = leader; tr[0x2D] = companion; }
			*(uint32_t*)(tr + 0x3F4) &= ~1u;
			if (InterlockedIncrement(&g_p2Squads) % 300 == 1)
				Log("proximity: squad %p active for %s (not leading)", sq, HeroName(nl));
		} else if (*(uint8_t*)(sq + 0x152) & 2) {
			((DtFn)vt[31])(sq, nullptr, dt);
			// vf35 resets an active squad nobody is near once its box is out of the
			// game camera's frustum (vf34: every Roman back to the pool), so that
			// they come back later at their spawn markers unseen. Player 2 was
			// looking right at them: they vanished and popped back in. Kept while
			// the other player's view has the box in sight.
			const float* f = (const float*)sq;
			float c[3] = { (f[0x47] + f[0x4A]) * 0.5f, (f[0x48] + f[0x4B]) * 0.5f, (f[0x49] + f[0x4C]) * 0.5f };
			float hx = (f[0x47] - f[0x4A]) * 0.5f, hy = (f[0x48] - f[0x4B]) * 0.5f, hz = (f[0x49] - f[0x4C]) * 0.5f;
			if (KlusterSphereInOtherViews(c, sqrtf(hx * hx + hy * hy + hz * hz))) {
				if (InterlockedIncrement(&g_keptSquads) % 600 == 1)
					Log("proximity: squad %p kept: out of the game camera's view, in the other player's", sq);
			} else {
				((VoidFn)vt[35])(sq, nullptr);
			}
		} else {
			*(float*)(sq + 0x138) += *(float*)(dt + 0xC);
		}
	}
}

// Checkpoints (the druid's cauldrons: light pillar and jingle). The checkpoint
// group's update (CKGrpAsterixCheckpoint::vf29, 0x536DD0) asks the trio for the
// leader's position (vf27) and, within a checkpoint's radius, records it and
// flags its event (game manager +0x332 + 2n) -- leader only. As a group it was
// sliced to whichever hero stood nearest its first checkpoint, an arbitrary
// one: in co-op even Asterix walked past checkpoints. Now it runs for the real
// leader, then once more with the non-leading player's hero as the leader, so
// either player reaches a checkpoint.
typedef void(__fastcall* CheckpointFn)(char* self, void* edx, int arg);   // ret 4
const uintptr_t kCheckpointVtbl = 0x628AB4, kCheckpointUpdate = 0x536DD0;
const size_t    kCheckpointSlot = 29;
CheckpointFn g_origCheckpoint = nullptr;

void __fastcall CheckpointHook(char* self, void* edx, int arg) {
	g_origCheckpoint(self, edx, arg);
	Trio t = GetTrio();
	int nl = CoopNonLeaderPlayer();
	if (!t || nl < 0 || !CoopP2Joined() || CoopSuspended() || CoopHeroHeld(nl)) return;
	uint8_t* tr = (uint8_t*)t.raw();
	uint8_t leader = tr[0x2C], companion = tr[0x2D];
	tr[0x2C] = (uint8_t)nl;
	tr[0x2D] = leader;
	g_origCheckpoint(self, edx, arg);
	if (tr[0x2C] == (uint8_t)nl) { tr[0x2C] = leader; tr[0x2D] = companion; }
}

// Fight mode (music, fight camera). The trio's tick (CKGrpTrioLife::vf17 ->
// FUN_00481950, thiscall, one argument) looks through its cached list of
// enemies near the leader (trio vf62..67, built from the leader's position by
// FUN_0047CB40 while +0x3F4 bit 0 is clear): an enemy updated this frame within
// +0x400 starts the fight -- the fight's music event group (+0x40E + 2 x the
// enemy's answer to 0x4C1A), the camera's fight bits, +0x3F4 bit 0x40 -- and
// refreshes +0x408; the fight ends +0x404 s after the last such enemy. Player
// 2's Obelix fought Romans in silence (level 5 sector 1). The non-leading
// player's hero gets a pass first, as the leader, so its fight starts and keeps
// the fight alive; then the game's own pass for the real leader.
typedef void(__fastcall* FightFn)(char* trio, void* edx, void* time);
const uintptr_t kFightMode = 0x481950;
const uint8_t   kFightModePrologue[7] = { 0x51, 0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x06 };
FightFn g_origFight = nullptr;
volatile long g_p2Fights = 0;

void __fastcall FightHook(char* trio, void* edx, void* time) {
	Trio t = GetTrio();
	int nl = CoopNonLeaderPlayer();
	if (t && trio == t.raw() && nl >= 0 && CoopP2Joined() && !CoopSuspended() && !CoopHeroHeld(nl)) {
		uint8_t* tr = (uint8_t*)trio;
		uint8_t leader = tr[0x2C], companion = tr[0x2D];
		bool wasOn = (*(uint32_t*)(tr + 0x3F4) & 0x40) != 0;
		tr[0x2C] = (uint8_t)nl;
		tr[0x2D] = leader;
		*(uint32_t*)(tr + 0x3F4) &= ~1u;   // the enemy cache is the leader's
		g_origFight(trio, edx, time);
		if (tr[0x2C] == (uint8_t)nl) { tr[0x2C] = leader; tr[0x2D] = companion; }
		*(uint32_t*)(tr + 0x3F4) &= ~1u;
		if (!wasOn && (*(uint32_t*)(tr + 0x3F4) & 0x40)) {
			InterlockedIncrement(&g_p2Fights);
			Log("proximity: fight mode (music) started by %s (not leading)", HeroName(nl));
		}
	}
	g_origFight(trio, edx, time);
}

// CKGrpBonusPool vf39 "may this hero collect" ignores its argument and asks the
// trio's leader instead (the potion pool, type 3: leader must be Asterix), so it
// is asked with the non-leading player's hero leading. "Player 2" in the names
// below means that hero: player 2's Obelix at the start of level 2, player 1's
// Asterix once Obelix leads (after the belly slide).
bool P2MayCollect(char* pool, Trio t, Hero h2) {
	typedef char(__fastcall* CanFn)(void* pool, void* edx, void* hero);
	uint8_t* tr = (uint8_t*)t.raw();
	uint8_t leader = tr[0x2C], companion = tr[0x2D];
	int p2 = CoopNonLeaderPlayer();
	tr[0x2C] = (uint8_t)p2;
	tr[0x2D] = (uint8_t)(p2 == CoopP1Hero() ? CoopP2Hero() : CoopP1Hero());
	bool ok = ((CanFn)(*(void***)pool)[39])(pool, nullptr, h2.hook) != 0;
	tr[0x2C] = leader;
	tr[0x2D] = companion;
	return ok;
}

void AttractToPlayer2(char* pool) {
	Trio t = GetTrio();
	int nl = CoopNonLeaderPlayer();
	Hero h2 = t && nl >= 0 ? t.GetHero(nl) : Hero();
	if (!h2 || !CoopP2Joined() || CoopSuspended() || CoopHeroHeld(nl)) return;
	typedef char(__fastcall* BoolFn)(void* self, void* edx);
	typedef void(__fastcall* AttractFn)(void* self, void* edx, void* hero, int zero);
	if (!P2MayCollect(pool, t, h2)) return;
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

// Placed bonuses (the helmets hanging in the air) are beacons, not pool
// members: every frame CKGrpBonusPool::vf27 walks the beacons of the live
// sectors, gives each uncollected one a bonus from the free list (vf21 places it,
// vf15 updates it) and, when the leader is within the pool's radius, calls
// bonus->vf18(leader, &beacon), which sends it flying and marks the beacon
// collected. Bonuses nobody claimed go back to the free list next frame. That
// distance check (0x54C6C6..0x54C719) is replaced: same test for the leader,
// then the same for player 2's hero.
const uintptr_t kBeaconCheck     = 0x54C6C6;   // mov al,[esp+13h]; test al,al; jz ...
const uintptr_t kBeaconCheckDone = 0x54C71A;
const uint8_t   kBeaconCheckBytes[10] = { 0x8A, 0x44, 0x24, 0x13, 0x84, 0xC0, 0x74, 0x4C, 0xD9, 0x44 };
volatile long g_p2Beacons = 0;

// `frame` is vf27's ESP inside the beacon loop: +0x13 the leader may collect,
// +0x1C the beacon descriptor, +0x20 the leader's hook, +0x3C leader position,
// +0x48 beacon position.
void __cdecl BeaconCheck(char* pool, char* bonus, char* frame) {
	typedef void(__fastcall* AttractFn)(void* self, void* edx, void* hero, void* beacon);
	typedef char(__fastcall* CanFn)(void* pool, void* edx, void* hero);
	AttractFn attract = (AttractFn)(*(void***)bonus)[18];
	float r2 = *(float*)(*(char**)(pool + 0x3C) + 4);
	const float* b = (const float*)(frame + 0x48);
	void* beacon = frame + 0x1C;
	if (*(uint8_t*)(frame + 0x13)) {
		const float* l = (const float*)(frame + 0x3C);
		float dx = b[0] - l[0], dy = b[1] - l[1], dz = b[2] - l[2];
		if (dx * dx + dy * dy + dz * dz <= r2) {
			attract(bonus, nullptr, *(void**)(frame + 0x20), beacon);
			return;
		}
	}
	if (!CoopP2Joined() || CoopSuspended()) return;
	Trio t = GetTrio();
	int nl = CoopNonLeaderPlayer();
	Hero h2 = t && nl >= 0 ? t.GetHero(nl) : Hero();
	if (!h2 || CoopHeroHeld(nl) || !P2MayCollect(pool, t, h2)) return;
	Vector3 p = h2.Pos();
	float dx = b[0] - p.x, dy = b[1] - p.y, dz = b[2] - p.z;
	if (dx * dx + dy * dy + dz * dz <= r2) {
		attract(bonus, nullptr, h2.hook, beacon);
		InterlockedIncrement(&g_p2Beacons);
	}
}

__declspec(naked) void BeaconCheckThunk() {
	__asm {
		pushad
		lea  eax, [esp + 36]   // vf27's ESP: 8 saved registers + our return address
		push eax
		push edi               // the bonus just placed on the beacon
		push esi               // the pool
		call BeaconCheck
		add  esp, 12
		popad
		ret
	}
}

bool PatchBeaconCheck() {
	uint8_t* code = (uint8_t*)kBeaconCheck;
	if (memcmp(code, kBeaconCheckBytes, sizeof(kBeaconCheckBytes)) != 0) {
		Log("proximity: beacon check at %08X does not match -- not patching", (unsigned)kBeaconCheck);
		return false;
	}
	DWORD old = 0;
	if (!VirtualProtect(code, sizeof(kBeaconCheckBytes), PAGE_EXECUTE_READWRITE, &old)) return false;
	code[0] = 0xE8;   // call BeaconCheckThunk
	*(uint32_t*)(code + 1) = (uint32_t)((uintptr_t)BeaconCheckThunk - (kBeaconCheck + 5));
	code[5] = 0xE9;   // jmp past the original check
	*(uint32_t*)(code + 6) = (uint32_t)(kBeaconCheckDone - (kBeaconCheck + 10));
	VirtualProtect(code, sizeof(kBeaconCheckBytes), old, &old);
	FlushInstructionCache(GetCurrentProcess(), code, sizeof(kBeaconCheckBytes));
	Log("proximity: beacon bonus check %08X -> %p", (unsigned)kBeaconCheck, (void*)BeaconCheckThunk);
	return true;
}

void __fastcall PoolUpdateHook(void* self, void* edx, void* dt) {
	g_origPoolUpdate(self, edx, dt);
	AttractToPlayer2((char*)self);
}

void __fastcall AsterixPoolUpdateHook(void* self, void* edx, void* dt) {
	g_origAsterixPoolUpdate(self, edx, dt);
	AttractToPlayer2((char*)self);
}

// Collision contacts. CKSrvCollision's tick hands out what its tests found:
//  * FUN_00447E30, shape-pair tests: receiver->sendEvent(0x300, list), list
//    entries {next, test}; a test starts with its two shapes.
//  * FUN_00447FF0, projectiles (punches, thrown things) against targets:
//    sendEvent(0x301, list) to each target's owner and to each projectile's
//    owner; entries {next, u8 record, u8 projectile, u16 target, u8 sub}, the
//    projectile's shape being at service+0x24 + 8*projectile.
// A shape's owner hook is at +0x20. Interactive objects only accept what comes
// from the trio's leader: CKHkBumper launches the leader if the touching shape
// is the leader's; an iron crate (filter 0x4B0DF0) breaks only if the punch is
// the leader's and the leader is not Asterix. Those calls are redirected here,
// and when the contact comes from player 2's hero alone (or the receiver is that
// hero), the leader is player 2's hero for the length of the call.
typedef int(__fastcall* SendEventFn)(void* self, void* edx, int event, void* param);
const uintptr_t kPairCallSite = 0x447FD1;          // mov edx,[ecx]; call [edx+14h]
const uint8_t   kPairCallBytes[5] = { 0x8B, 0x11, 0xFF, 0x52, 0x14 };
const uintptr_t kTargetCallSite = 0x4483C5;        // mov edx,[ecx]; call [edx+14h]
const uint8_t   kTargetCallBytes[5] = { 0x8B, 0x11, 0xFF, 0x52, 0x14 };
const uintptr_t kProjectileCallSite = 0x448405;    // mov edx,[ecx]; push eax; push 301h; call [edx+14h]
const uint8_t   kProjectileCallBytes[11] = { 0x8B, 0x11, 0x50, 0x68, 0x01, 0x03, 0x00, 0x00,
                                             0xFF, 0x52, 0x14 };
volatile long g_p2Contacts = 0;

// Calls receiver->sendEvent(event, list) with `actor` as the trio's leader.
int SendAs(char* receiver, int event, void* list, Trio t, int actor, int other, int p2) {
	SendEventFn send = (SendEventFn)(*(void***)receiver)[5];
	uint8_t* tr = (uint8_t*)t.raw();
	if (actor < 0 || tr[0x2C] == actor) return send(receiver, nullptr, event, list);
	uint8_t leader = tr[0x2C], companion = tr[0x2D];
	tr[0x2C] = (uint8_t)actor;
	tr[0x2D] = (uint8_t)other;
	if (actor == p2) InterlockedIncrement(&g_p2Contacts);
	int r = send(receiver, nullptr, event, list);
	if (tr[0x2C] == (uint8_t)actor && tr[0x2D] == (uint8_t)other) {
		tr[0x2C] = leader;
		tr[0x2D] = companion;
	}
	return r;
}

// Safe sequence (ini safe_sequence, on by default). A lever (CKHkActivator,
// vtable 0x628DA0; events at +0x28 "on push" and +0x2A "on animation end")
// whose event group tells the trio to switch to a hero (0x4800/0x48B7
// Asterix, 0x4801/0x48B8 Obelix) is meant to be worked by the other hero, while
// he leads: level 2's lever behind the iron crates (#45, "switch to Asterix")
// is Obelix's, with Asterix to light the hearth after it (#428 then has Obelix
// lead and sends Asterix to wait at a marker). Hit by the non-leading Obelix
// while Asterix already led, the hearth came next with Asterix as the leader
// -- he was sent off to his marker for good. Such a lever only answers the
// non-leading player when its switch would change the leader.
char* g_heldBackLogged = nullptr;   // the lever last reported

int GroupSwitchTarget(unsigned ev) {   // the hero an event's group switches to, or -1
	unsigned group = (ev & 0xFFFF) >> 3;
	CKYellowPages* yp = YellowPages();
	char* es = yp ? *(char**)((char*)yp + 0x6C) : nullptr;
	if (!es || group == 0x1FFF) return -1;
	unsigned n = (unsigned)*(uint16_t*)(es + 0x1C) + *(uint16_t*)(es + 0x1E) + *(uint16_t*)(es + 0x20);
	if (group >= n) return -1;
	uint8_t* tab = *(uint8_t**)(es + 0xC);
	char** objs = *(char***)(es + 0x10);
	uint16_t* msgs = *(uint16_t**)(es + 0x14);
	unsigned first = 0;
	for (unsigned g = 0; g < group; g++) first += tab[g * 3];
	for (unsigned k = 0; k < tab[group * 3]; k++) {
		char* o = objs[first + k];
		if (!o || *(uintptr_t*)o != 0x6256A0) continue;   // CKGrpTrio
		unsigned m = msgs[first + k];
		if (m == 0x4800 || m == 0x48B7) return HERO_ASTERIX;
		if (m == 0x4801 || m == 0x48B8) return HERO_OBELIX;
	}
	return -1;
}

bool GroupMessagesTrio(unsigned ev) {   // does event `ev`'s group message the trio at all?
	unsigned group = (ev & 0xFFFF) >> 3;
	CKYellowPages* yp = YellowPages();
	char* es = yp ? *(char**)((char*)yp + 0x6C) : nullptr;
	if (!es || group == 0x1FFF) return false;
	unsigned n = (unsigned)*(uint16_t*)(es + 0x1C) + *(uint16_t*)(es + 0x1E) + *(uint16_t*)(es + 0x20);
	if (group >= n) return false;
	uint8_t* tab = *(uint8_t**)(es + 0xC);
	char** objs = *(char***)(es + 0x10);
	unsigned first = 0;
	for (unsigned g = 0; g < group; g++) first += tab[g * 3];
	for (unsigned k = 0; k < tab[group * 3]; k++)
		if (objs[first + k] && *(uintptr_t*)objs[first + k] == 0x6256A0) return true;
	return false;
}

bool LeverHeldBack(char* receiver, Trio t) {
	if (!g_config.safeSequence || *(uintptr_t*)receiver != 0x628DA0) return false;
	int lead = t.Leader();
	unsigned evs[2] = { *(uint16_t*)(receiver + 0x28), *(uint16_t*)(receiver + 0x2A) };
	for (unsigned ev : evs)
		if (GroupSwitchTarget(ev) == lead) return true;
	return false;
}

// `maskOf(entry, h1, h2)`: bit 0 if player 1's hero is in that contact, bit 1
// for player 2's. Entries are chained through their first word. A contact list
// with both heroes in it (both fighting the same Roman) is delivered in two
// calls: everything but player 2's contacts as player 1, then player 2's as
// player 2.
template <class MaskOf>
int DispatchAs(char* receiver, int event, void* list, MaskOf maskOf) {
	SendEventFn send = (SendEventFn)(*(void***)receiver)[5];
	Trio t = GetTrio();
	if (!t || !CoopP2Joined() || CoopSuspended()) return send(receiver, nullptr, event, list);
	int p1 = CoopP1Hero(), p2 = CoopP2Hero();
	Hero h1 = t.GetHero(p1), h2 = t.GetHero(p2);
	if (!h1 || !h2 || CoopHeroHeld(p1) || CoopHeroHeld(p2)) return send(receiver, nullptr, event, list);
	if (receiver == h1.raw()) return SendAs(receiver, event, list, t, p1, p2, p2);
	if (receiver == h2.raw()) return SendAs(receiver, event, list, t, p2, p1, p2);

	void* entries[64];
	int masks[64], n = 0, all = 0;
	for (void* e = list; e; e = *(void**)e) {
		if (n == 64) return send(receiver, nullptr, event, list);
		entries[n] = e;
		masks[n] = maskOf(e, h1, h2);
		all |= masks[n++];
	}
	if (all == 0) return send(receiver, nullptr, event, list);
	int nl = CoopNonLeaderPlayer();
	int nlMask = nl == p1 ? 1 : nl == p2 ? 2 : 0;
	if (nlMask && (all & nlMask) && LeverHeldBack(receiver, t)) {
		// The non-leader's contacts go nowhere; the rest as the game made them.
		void* head = nullptr; void** tail = &head;
		bool dropped = false;
		for (int i = 0; i < n; i++) {
			if (masks[i] & nlMask) { dropped = true; continue; }
			*tail = entries[i];
			tail = (void**)entries[i];
		}
		*tail = nullptr;
		if (dropped && g_heldBackLogged != receiver) {
			g_heldBackLogged = receiver;
			Log("proximity: safe sequence: lever %p ignores %s -- it switches to %s, who leads already",
			    receiver, HeroName(nl), HeroName(t.Leader()));
		}
		int r = head ? send(receiver, nullptr, event, head) : 0;
		for (int i = 0; i < n; i++) *(void**)entries[i] = i + 1 < n ? entries[i + 1] : nullptr;
		return r;
	}
	if (all == 1) return SendAs(receiver, event, list, t, p1, p2, p2);
	bool mixed = false;
	for (int i = 0; i < n; i++) if (!(masks[i] & 2)) mixed = true;
	if (!mixed) return SendAs(receiver, event, list, t, p2, p1, p2);

	// Split, deliver, then put the chain back the way the service built it.
	void* headA = nullptr; void** tailA = &headA;
	void* headB = nullptr; void** tailB = &headB;
	for (int i = 0; i < n; i++) {
		void*** tail = (masks[i] & 2) ? &tailB : &tailA;
		**tail = entries[i];
		*tail = (void**)entries[i];
	}
	*tailA = nullptr;
	*tailB = nullptr;
	int r = SendAs(receiver, event, headA, t, p1, p2, p2);
	SendAs(receiver, event, headB, t, p2, p1, p2);
	for (int i = 0; i < n; i++) *(void**)entries[i] = i + 1 < n ? entries[i + 1] : nullptr;
	return r;
}

int __fastcall PairContactDispatch(char* receiver, void*, int event, int* list) {
	return DispatchAs(receiver, event, list, [](void* e, Hero h1, Hero h2) {
		char** test = (char**)((int*)e)[1];
		int m = 0;
		for (int k = 0; test && k < 2; k++) {
			char* owner = test[k] ? *(char**)(test[k] + 0x20) : nullptr;
			if (owner == h1.raw()) m |= 1;
			else if (owner == h2.raw()) m |= 2;
		}
		return m;
	});
}

int __fastcall ProjectileContactDispatch(char* receiver, void*, int event, uint8_t* list) {
	CKYellowPages* yp = YellowPages();
	char* srv = yp ? *(char**)((char*)yp + 0x50) : nullptr;
	unsigned count = srv ? *(uint8_t*)(srv + 0x1B6) : 0;
	return DispatchAs(receiver, event, list, [srv, count](void* e, Hero h1, Hero h2) {
		unsigned idx = ((uint8_t*)e)[5];
		if (!srv || idx >= count) return 0;
		char* shape = *(char**)(srv + 0x24 + 8 * idx);
		char* owner = shape ? *(char**)(shape + 0x20) : nullptr;
		return owner == h1.raw() ? 1 : owner == h2.raw() ? 2 : 0;
	});
}

// The projectile-owner loop loads the vtable before its pushes, so the patch
// there rewrites the whole sequence: push eax; push 301h; call dispatcher.
bool PatchProjectileOwnerCall() {
	uint8_t* code = (uint8_t*)kProjectileCallSite;
	if (memcmp(code, kProjectileCallBytes, sizeof(kProjectileCallBytes)) != 0) {
		Log("proximity: projectile owner call site %08X does not match -- not patching",
			(unsigned)kProjectileCallSite);
		return false;
	}
	DWORD old = 0;
	if (!VirtualProtect(code, sizeof(kProjectileCallBytes), PAGE_EXECUTE_READWRITE, &old)) return false;
	code[0] = 0x50;                                   // push eax (the list)
	code[1] = 0x68; *(uint32_t*)(code + 2) = 0x301;   // push 301h
	code[6] = 0xE8;                                   // call ProjectileContactDispatch
	*(uint32_t*)(code + 7) = (uint32_t)((uintptr_t)ProjectileContactDispatch - (kProjectileCallSite + 11));
	VirtualProtect(code, sizeof(kProjectileCallBytes), old, &old);
	FlushInstructionCache(GetCurrentProcess(), code, sizeof(kProjectileCallBytes));
	Log("proximity: projectile hits, owners (0x301) call site %08X -> %p", (unsigned)kProjectileCallSite,
		(void*)ProjectileContactDispatch);
	return true;
}

// Trigger zones. CKSrvDetection (yellow pages +0x54, vtable 0x62504C) tests
// zones against the nodes of specific heroes: detected objects at +0x18 (count
// +0x20, Obelix's node and Asterix's), their positions last frame at +0x1C (12
// bytes each). Each test (8 bytes: +0 shape, +2 object, +4 bit 0 enabled, +5
// bit 0 "inside" wanted, +6 event) is bound to one object:
//   +0x2C/+0x30 boxes, +0x34/+0x38 spheres, +0x3C/+0x40 planes: the event
//     fires (FUN_00460C20) when the object enters / crosses this frame;
//   +0x44/+0x48 boxes, +0x4C/+0x50 spheres: the event flag is set every frame
//     the object is inside (or outside, +5 bit 0 clear).
// In single player the AI companion stays at the leader's side, so which hero
// a zone is bound to does not matter -- it fires where the pair goes. In co-op
// the two heroes go their own ways: a zone bound to Asterix never saw player
// 2's Obelix (Romans near the lifter did not appear until player 1 came).
// After the engine's own pass, the tests bound to either hero run again with
// the two heroes' nodes and last positions swapped, so every hero zone answers
// to both players. "While outside" tests are left out of that second pass:
// they mean the pair is away, which needs both heroes outside, not either.
typedef void(__fastcall* DetectTickFn)(char* self, void* edx, int arg);
const uintptr_t kDetectVtbl = 0x62504C;
const size_t    kDetectTickSlot = 17;
const uintptr_t kDetectTick = 0x46BE90;
DetectTickFn g_origDetectTick = nullptr;
volatile long g_swappedZones = 0;
bool g_inSwappedPass = false;
char* g_tickingDet = nullptr;   // the detection service inside DetectTickHook

// Which hero set off zone event `ev`: the enabled test carrying it, and the
// node it is bound to right now (the swapped pass has the nodes swapped).
int ZoneEventHero(unsigned ev) {
	char* det = g_tickingDet;
	Trio t = GetTrio();
	if (!det || !t) return -1;
	char** nodes = *(char***)(det + 0x18);
	unsigned nn = *(uint16_t*)(det + 0x20);
	const int bases[5] = { 0x2C, 0x34, 0x3C, 0x44, 0x4C };
	for (int base : bases) {
		char* e = *(char**)(det + base);
		unsigned n = *(uint16_t*)(det + base + 4);
		for (unsigned i = 0; e && i < n; i++, e += 8) {
			if (*(uint16_t*)(e + 6) != (uint16_t)ev || !(e[4] & 1)) continue;
			unsigned obj = *(uint16_t*)(e + 2);
			if (!nodes || obj >= nn) return -1;
			for (int h = 0; h < HERO_COUNT; h++) {
				Hero hero = t.GetHero(h);
				if (hero && (char*)hero.hook->node == nodes[obj]) return h;
			}
			return -1;
		}
	}
	return -1;
}

int NodeIndex(char* det, Hero h) {
	char** nodes = *(char***)(det + 0x18);
	unsigned n = *(uint16_t*)(det + 0x20);
	for (unsigned i = 0; nodes && h && i < n; i++)
		if (nodes[i] == (char*)h.hook->node) return (int)i;
	return -1;
}

void __fastcall DetectTickHook(char* det, void* edx, int arg) {
	Trio t = GetTrio();
	bool coop = t && CoopP2Joined() && !CoopSuspended() && *(uint8_t*)(det + 0x2A) &&
	            !CoopHeroHeld(HERO_ASTERIX) && !CoopHeroHeld(HERO_OBELIX);
	int ia = coop ? NodeIndex(det, t.GetHero(HERO_ASTERIX)) : -1;
	int io = coop ? NodeIndex(det, t.GetHero(HERO_OBELIX)) : -1;
	g_tickingDet = det;
	if (ia < 0 || io < 0) { g_origDetectTick(det, edx, arg); g_tickingDet = nullptr; return; }

	float* prev = *(float**)(det + 0x1C);
	float prevA[3], prevO[3];
	memcpy(prevA, prev + 3 * ia, 12);
	memcpy(prevO, prev + 3 * io, 12);
	g_origDetectTick(det, edx, arg);

	// Second pass: only enabled tests bound to a hero, minus "while outside".
	struct List { int base, count; bool level; } lists[5] = {
		{ 0x2C, 0x30, false }, { 0x34, 0x38, false }, { 0x3C, 0x40, false },
		{ 0x44, 0x48, true }, { 0x4C, 0x50, true } };
	static uint8_t saved[4096];
	size_t k = 0;
	bool any = false;
	for (const List& l : lists) {
		char* e = *(char**)(det + l.base);
		unsigned n = *(uint16_t*)(det + l.count);
		for (unsigned i = 0; e && i < n; i++, e += 8) {
			if (k < sizeof(saved)) saved[k] = (uint8_t)e[4];
			k++;
			unsigned obj = *(uint16_t*)(e + 2);
			bool keep = (obj == (unsigned)ia || obj == (unsigned)io) && !(l.level && !(e[5] & 1));
			if (keep && (e[4] & 1)) any = true;
			else e[4] &= ~1;
		}
	}
	if (any && k <= sizeof(saved)) {
		char** nodes = *(char***)(det + 0x18);
		float curA[3], curO[3];
		memcpy(curA, prev + 3 * ia, 12);
		memcpy(curO, prev + 3 * io, 12);
		std::swap(nodes[ia], nodes[io]);
		memcpy(prev + 3 * ia, prevO, 12);
		memcpy(prev + 3 * io, prevA, 12);
		g_inSwappedPass = true;
		g_origDetectTick(det, edx, arg);
		g_inSwappedPass = false;
		std::swap(nodes[ia], nodes[io]);
		memcpy(prev + 3 * ia, curA, 12);
		memcpy(prev + 3 * io, curO, 12);
	}
	k = 0;
	for (const List& l : lists) {
		char* e = *(char**)(det + l.base);
		unsigned n = *(uint16_t*)(det + l.count);
		for (unsigned i = 0; e && i < n; i++, e += 8, k++)
			if (k < sizeof(saved)) e[4] = (char)saved[k];
	}
	g_tickingDet = nullptr;
}

// Zone events: logged while player 2 is in (the second pass marked as such).
typedef void(__fastcall* FlagEventFn)(void* self, void* edx, unsigned eventId);
const uintptr_t kFlagEvent = 0x460C20;
const uint8_t   kFlagEventPrologue[7] = { 0x8B, 0xD1, 0x66, 0x8B, 0x4C, 0x24, 0x04 };
const uintptr_t kDetectTickBegin = 0x46BE90, kDetectTickEnd = 0x46C0CE;   // CKSrvDetection::vf17
FlagEventFn g_origFlagEvent = nullptr;

// Diagnostics: events flagged into a group that messages the sound manager
// (music), logged with the flagging code's address.
bool IsMusicGroup(unsigned group) {
	static void* cachedFor = nullptr;
	static uint8_t music[1024 / 8];
	CKYellowPages* yp = YellowPages();
	char* ev = yp ? *(char**)((char*)yp + 0x6C) : nullptr;
	if (!ev) return false;
	if (cachedFor != *(void**)(ev + 0xC)) {
		cachedFor = *(void**)(ev + 0xC);
		memset(music, 0, sizeof(music));
		unsigned n = (unsigned)*(uint16_t*)(ev + 0x1C) + *(uint16_t*)(ev + 0x1E) + *(uint16_t*)(ev + 0x20);
		uint8_t* tab = *(uint8_t**)(ev + 0xC);
		char** objs = *(char***)(ev + 0x10);
		unsigned first = 0;
		for (unsigned gi = 0; gi < n && gi < 1024; first += tab[gi * 3], gi++)
			for (unsigned k = 0; k < tab[gi * 3]; k++)
				if (objs[first + k] && *(uintptr_t*)objs[first + k] == 0x624058) music[gi >> 3] |= 1 << (gi & 7);
	}
	return group < 1024 && (music[group >> 3] & (1 << (group & 7)));
}

void __fastcall FlagEventHook(void* self, void* edx, unsigned eventId) {
	uintptr_t from = (uintptr_t)_ReturnAddress();
	unsigned grp = (eventId & 0xFFFF) >> 3;
	static const bool logAll = getenv("XXL_LOGFLAGS") != nullptr;   // diagnostics: every flagged event
	if (logAll) {
		static unsigned last[64];
		static int nLast = 0;
		bool seen = false;
		static const bool all = getenv("XXL_LOGFLAGS")[0] == '2';
		for (int i = 0; i < nLast && !all; i++) if (last[i] == ((eventId & 0xFFFF) ^ (unsigned)from << 16)) seen = true;
		if (!seen) {
			if (nLast < 64) last[nLast++] = (eventId & 0xFFFF) ^ (unsigned)from << 16;
			Trio t = GetTrio();
			Log("proximity: event %04X (group %03X) flagged from %p, leader %s", eventId & 0xFFFF, grp, (void*)from,
			    t ? HeroName(t.Leader()) : "?");
		}
	}
	if (IsMusicGroup(grp) || grp == 0x74 || grp == 0x77) {
		Trio t = GetTrio();
		Log("proximity: music event %04X flagged from %p (leader %s)", eventId & 0xFFFF, (void*)from,
		    t ? HeroName(t.Leader()) : "?");
	}
	if (from >= kDetectTickBegin && from < kDetectTickEnd) {
		if (g_inSwappedPass) InterlockedIncrement(&g_swappedZones);
		int zh = ZoneEventHero(eventId & 0xFFFF);
		if (zh >= 0) CoopNoteZoneHero(zh);
		Log("proximity: zone event %04X%s", eventId & 0xFFFF,
		    g_inSwappedPass ? " (the other hero's zone, for co-op)" : "");
	}
	g_origFlagEvent(self, edx, eventId);
}

} // namespace

long ProximityP2Contacts() { return g_p2Contacts; }
int ProximityLeverSwitchTarget(void* lever) {
	char* l = (char*)lever;
	int a = GroupSwitchTarget(*(uint16_t*)(l + 0x28));
	return a >= 0 ? a : GroupSwitchTarget(*(uint16_t*)(l + 0x2A));
}
long ProximitySwappedZones() { return g_swappedZones; }
long ProximityP2Squads() { return g_p2Squads; }
long ProximityP2Beacons() { return g_p2Beacons; }
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
	ok &= PatchCallSite(kPairCallSite, kPairCallBytes, sizeof(kPairCallBytes),
	                    (void*)PairContactDispatch, "collision contacts (0x300)");
	ok &= PatchCallSite(kTargetCallSite, kTargetCallBytes, sizeof(kTargetCallBytes),
	                    (void*)ProjectileContactDispatch, "projectile hits, targets (0x301)");
	ok &= PatchProjectileOwnerCall();
	ok &= PatchBeaconCheck();
	ok &= PatchVtableSlot(kCheckpointVtbl, kCheckpointSlot, kCheckpointUpdate, (void*)CheckpointHook,
	                      (void**)&g_origCheckpoint, "CKGrpAsterixCheckpoint::update");
	ok &= PatchVtableSlot(kEnemyLifeVtbl, kEnemyLifeSlot, kEnemyLifeTick, (void*)EnemyTickHook,
	                      (void**)&g_origEnemyTick, "CKGrpEnemyLife::tick");
	ok &= PatchVtableSlot(kDetectVtbl, kDetectTickSlot, kDetectTick, (void*)DetectTickHook,
	                      (void**)&g_origDetectTick, "CKSrvDetection::tick");
	ok &= InstallDetour(kFightMode, (void*)FightHook, (void**)&g_origFight, kFightModePrologue,
	                    sizeof(kFightModePrologue), "trio fight mode (music) for the non-leader");
	ok &= InstallDetour(kFlagEvent, (void*)FlagEventHook, (void**)&g_origFlagEvent, kFlagEventPrologue,
	                    sizeof(kFlagEventPrologue), "event flag (zones)");
	return ok;
}
