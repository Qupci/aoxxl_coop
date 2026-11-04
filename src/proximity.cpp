#include "config.h"
#include "proximity.h"
#include "coop.h"
#include "individual.h"
#include "detour.h"
#include "klusters.h"
#include "log.h"
#include "guard.h"
#include "nolock.h"
#include "p2cam.h"
#include "trio.h"

#include <cmath>
#include <cstdlib>
#include <intrin.h>
#include <cstring>
#include <unordered_map>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// A hero the game has borrowed (scripted order), or one that is down
// (individual heroes): objects deal with the other hero, the real leader.
static bool HeroOut(int idx) { return CoopHeroHeld(idx) || IndivFallen(idx); }

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
const uintptr_t kTorchVtbl = 0x625F08;           // CKHkTorch (2,32)
const uintptr_t kBoatVtbl = 0x627EB8;            // CKHkBoat (2,39)
// Lifters (CKHkLifter::vf15, 0x4AD670) waiting at the bottom with "wait for
// trio" (flags +8 bit 0) count the time the leader stands on them without the
// companion (+0x34); past the game manager's limit they send their "sidekick
// not present" event (+0x70: teleport Idefix and the sidekick onto the
// lifter). Ticked with player 2's Obelix posing as the leader, it dropped the
// real leader's companion -- Obelix himself -- onto the lifter every 9 s (level 5
// sector 8). In co-op nobody is teleported: the lifter waits for both players.
const uintptr_t kLifterVtbl = 0x625E2C;          // CKHkLifter (2,47)
// The spies (CKHkClueMan, 2,161). Their tick (vf15, 0x4B8FB0) spawns the spy
// (FUN_004B12F0, node at hook +0x10) when the trio leader comes within +0x68
// of its spot (+0x12C/+0x130/+0x134 x/y/z) and removes it (FUN_004B1790) when
// the leader goes out of that range. While no spy is spawned there is no node,
// so its tick was never sliced: only the leader made it appear.
const uintptr_t kClueManVtbl = 0x6261F4;
const uintptr_t kShopVtbl = 0x6263CC;      // CKHkAsterixShop (2,172)
// Blocks to push (CKHkPushPullAsterix, 2,147). Their tick (vf15, 0x4B5140)
// lets go of the hero pushing them (+0x18 bit 4 cleared, vf27 0x6C) when the
// trio's leader (trio vfA0) is a hero the block is not for (+0x14 bits 6/7).
// Ticked with Asterix posing as the leader -- walking past the big block LV5-3
// while Obelix pushed it --, the block dropped Obelix in the middle of the
// level's "the block slides on its own" (09A7: the pusher's +0x3F0 bit 6, sent
// through the trio as 090C): the second 09A7 found nobody pushing and never
// sent the 090C(0) that ends it, and Obelix pushed in place for good. A block
// a hero pushes ticks with that hero as the leader.
const uintptr_t kPushPullVtbl = 0x626110;
// Bonus spitters (CKHkBonusSpitter, 2,194; LV4-4's lever spits a magic potion
// flask). Their tick (vf15, 0x54A780) spits the next item -- a bonus pool type,
// +0x18, or +0x1C while +0x18 is -1 -- when the trio's leader (trio vf25) is
// within +0x24, and sends it after that leader (CKHkBasicBonus_FollowHero).
// Ticked as Obelix's nearest, the flask flew to Obelix and filled the potion.
// A spitter whose next item is the potion (type 3) ticks with Asterix as the
// leader: it waits for him, as the potion pool does.
const uintptr_t kBonusSpitterVtbl = 0x6294B8;
const int       kPotionPoolType   = 3;
static bool SpitsPotion(char* spitter) {
	int item = *(int*)(spitter + 0x18);
	if (item == -1) item = *(int*)(spitter + 0x1C);
	return item == kPotionPoolType;
}
// Roman catapults (CKHkCatapult, 2,130; LV3-1). A hero's blow (contact 0x0300
// on its volume of kind 3) gives the hitter "a hit landed" (0x4C0E) and sets
// +0x14 bit 0x40; the catapult's next tick then puts the hit on the combo bar
// once more itself (FUN_005093A0: game manager +0xB0 / +0xB4, or the full
// gauge) -- two a blow in single player. Ticked with the leader's stats in
// place, the non-leader's blows went on both heroes' bars (catapult_shared_combo.log).
// It ticks with the hitter's: the newer of its two attackers (volumes +0x94 /
// +0x98, owner at +0x20), whose timer (+0x30 / +0x34, dt added each tick) a
// blow sets to 0.
const uintptr_t kCatapultVtbl = 0x628238;
static int CatapultHitter(char* cat, Trio t) {
	if (!(*(uint8_t*)(cat + 0x14) & 0x40)) return -1;
	char* by = nullptr;
	float newest = 1e30f;
	for (int k = 0; k < 2; k++) {
		char* vol = *(char**)(cat + 0x94 + k * 4);
		float since = *(float*)(cat + 0x30 + k * 4);
		if (vol && !IsBadReadPtr(vol, 0x24) && since < newest) { newest = since; by = *(char**)(vol + 0x20); }
	}
	for (int i = HERO_ASTERIX; by && i <= HERO_OBELIX; i++)
		if (t.GetHero(i).raw() == by) return i;
	return -1;
}
// The one hero a block is for (+0x14 bit 6 Asterix, bit 7 Obelix; it offers its
// push or pull, 0x900 to trio vf25, only while that hero leads), else -1. LV2-8's
// platform under a light pillar is Obelix's to pull with Asterix on the pillar:
// in no-lock mode Asterix stands there as a player's hero, the nearest, and the
// platform ticked with him as the leader never offered Obelix its "Pull" (with
// nolock off he is the game's, and nothing is sliced).
static int BlockHero(char* block) {
	uint8_t f = *(uint8_t*)(block + 0x14) & 0xC0;
	return f == 0x40 ? HERO_ASTERIX : f == 0x80 ? HERO_OBELIX : -1;
}

// The hero pushing `block`: its push state (hero +0x3EC bits 25-30, FUN_0049A670)
// is on and its pushed object (+0x484, which it sends 0x0901 to) is the block.
static int PusherOf(char* block, Trio t, int p1, int p2) {
	for (int i : { p1, p2 }) {
		Hero h = t.GetHero(i);
		if (h && *(char**)(h.raw() + 0x484) == block && ((*(uint32_t*)(h.raw() + 0x3EC) >> 25) & 0x3F) != 0) return i;
	}
	return -1;
}

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
bool GroupCommandsTrio(unsigned ev);
int GroupTargets(unsigned ev, char** objsOut, uint16_t* msgsOut, int max);

// Plain pressure plates (CKHkPressionStone, flags +8: bit 0x10 "activatable by
// hero" -- pressed while the object under the trio leader's feet, trio vf29, is
// the plate's +0x28 --, bit 0x40 by a pushed object) answer either player's
// hero: ticked as usual when the leader is on it, otherwise with the
// non-leading player's hero posing as the leader (level 3 sector 1's plate
// stayed up under player 2's Obelix). Plates whose events (+0x30, +0x32)
// command the trio -- the blue circles: "switch to Obelix, Asterix waits here"
// -- keep to the real leader, as above. Other trio messages do not count: level
// 2 sector 6's plate (5.7 2.8 -389.2: a rolling stone, a camera sector) also
// sends the trio 0x2000, "reset the fight mode timer".
bool PlateForEither(char* plate) {
	if (!(*(uint32_t*)(plate + 8) & 0x10)) return false;
	unsigned evs[2] = { *(uint16_t*)(plate + 0x30), *(uint16_t*)(plate + 0x32) };
	for (unsigned ev : evs)
		if (GroupCommandsTrio(ev)) return false;
	return true;
}

volatile long g_p2Plates = 0;
char* g_plateLogged = nullptr;

void TickPlate(int* life, void* dt, char* plate, Trio t) {
	TickFn tick = (TickFn)(*(void***)life)[17];
	int nl = CoopNonLeaderPlayer();
	typedef int(__fastcall* GroundFn)(void* trio, void* edx);
	GroundFn ground = (GroundFn)(*(void***)t.raw())[29];
	if (nl < 0 || HeroOut(nl) || ground(t.raw(), nullptr) == *(int*)(plate + 0x28)) {
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

// A corkscrew's circle (CKHkCorkscrew 2,44: LV2-1's spinning platform, LV3-6's
// light pillars), armed (+8 bit 1) and not landed on (bit 0), sets off its
// "landing" event (+0x40) when the trio's leader (trio vf25: its node) is within
// +0x3C of its circle. That event gives the lead away and tells the companion to
// wait there (LV2-1's 0050: 4801 + 4802): it is meant for the hero it makes wait
// -- Asterix for "Obelix leads" -- who is launched once the turn is done (its
// check: the companion's ground, trio 0xB01, is the corkscrew's; then 0x0C09 ->
// 4805, the companion's AI order 2). With Obelix leading, Asterix on the circle
// set nothing off, and Obelix on it set it off for Asterix wherever he stood
// (locked there by nolock_except_corkscrews when near). Its landing is now checked
// with that hero posing as the leader; after the event the trio has him as the
// companion anyway, as in single player.
int CorkscrewHero(char* cork) {
	char* objs[32];
	uint16_t msgs[32];
	int n = GroupTargets(*(uint16_t*)(cork + 0x40), objs, msgs, 32), to = -1;
	bool wait = false;
	for (int k = 0; k < n; k++) {
		if (!objs[k] || *(uintptr_t*)objs[k] != 0x6256A0) continue;
		unsigned m = msgs[k];
		if (m == 0x4802 || m == 0x4849) wait = true;
		if (m == 0x4800 || m == 0x48B7) to = HERO_ASTERIX;
		if (m == 0x4801 || m == 0x48B8) to = HERO_OBELIX;
	}
	if (!wait || to < 0) return -1;
	return to == HERO_ASTERIX ? HERO_OBELIX : HERO_ASTERIX;   // the one who waits
}

bool TickCorkscrew(int* life, void* dt, char* cork, Trio t) {
	if ((*(uint32_t*)(cork + 8) & 3) != 2 || !CoopP2Joined() || CoopSuspended()) return false;
	int who = CorkscrewHero(cork);
	if (who < 0 || who == t.Leader() || (who != CoopP1Hero() && who != CoopP2Hero()) || HeroOut(who)) return false;
	TickFn tick = (TickFn)(*(void***)life)[17];
	uint8_t* tr = (uint8_t*)t.raw();
	uint8_t leader = tr[0x2C], companion = tr[0x2D];
	tr[0x2C] = (uint8_t)who;
	tr[0x2D] = leader;
	tick(life, nullptr, dt);
	if (tr[0x2C] == (uint8_t)who) { tr[0x2C] = leader; tr[0x2D] = companion; }
	if (*(uint32_t*)(cork + 8) & 1) {
		static int s_told = 0;
		if (s_told++ < 20)
			Log("proximity: corkscrew %p landed on by %s (not leading) -- its event %04X", cork, HeroName(who),
			    *(uint16_t*)(cork + 0x40));
	}
	return true;
}

// Single player with the lead changing hands ([single_player] sp_hero_switch,
// sp_second_chance: either hero may lead anywhere now), what the level leaves to
// one hero answers only that hero. A corkscrew's circle is landed on only with
// the hero it makes wait leading (CorkscrewHero) -- otherwise its radius (+0x3C,
// its box and sphere test against the leader's node) is out of reach for the
// tick: Obelix leading on Asterix's circle set it off for Asterix wherever he
// was. A spitter's potion flask waits for Asterix (ticked with him as the
// leader: it flies to him even as the AI companion -- his to drink).
bool SoloLeadFree() { return !CoopP2Joined() && (g_config.spHeroSwitch || g_config.spSecondChance); }

// Logged once per object and leader (40 lines at most).
bool SoloToldOnce(char* hook, int leader) {
	static struct { char* hook; int leader; } s_told[16];
	static int s_n = 0, s_lines = 0;
	for (int i = 0; i < s_n; i++)
		if (s_told[i].hook == hook) {
			if (s_told[i].leader == leader) return false;
			s_told[i].leader = leader;
			return s_lines++ < 40;
		}
	if (s_n < 16) s_told[s_n++] = { hook, leader };
	return s_lines++ < 40;
}

bool SoloTick(int* life, void* dt, char* hook, Trio t) {
	TickFn tick = (TickFn)(*(void***)life)[17];
	uintptr_t vt = *(uintptr_t*)hook;
	if (vt == kCorkscrewVtbl && (*(uint32_t*)(hook + 8) & 3) == 2) {
		int who = CorkscrewHero(hook);
		if (who < 0 || who == t.Leader()) return false;
		float r = *(float*)(hook + 0x3C);
		*(float*)(hook + 0x3C) = -1.0f;
		tick(life, nullptr, dt);
		*(float*)(hook + 0x3C) = r;
		if (SoloToldOnce(hook, t.Leader())) {
			Log("proximity: corkscrew %p is %s's (its landing makes that hero wait) -- shut while %s leads", hook,
			    HeroName(who), HeroName(t.Leader()));
		}
		return true;
	}
	if (vt == kBonusSpitterVtbl && SpitsPotion(hook) && t.Leader() != HERO_ASTERIX) {
		uint8_t* tr = (uint8_t*)t.raw();
		uint8_t leader = tr[0x2C], companion = tr[0x2D];
		tr[0x2C] = HERO_ASTERIX;
		tr[0x2D] = HERO_OBELIX;
		tick(life, nullptr, dt);
		if (tr[0x2C] == HERO_ASTERIX) { tr[0x2C] = leader; tr[0x2D] = companion; }
		if (SoloToldOnce(hook, leader)) {
			Log("proximity: spitter %p's next is the magic potion -- ticks with Asterix as the leader (%s leads)", hook,
			    HeroName(leader));
		}
		return true;
	}
	return false;
}

// Romans passed from one player to the other (LB): flying (state +0x21 == 4),
// they tick with the receiver as the trio leader.
struct Pass { char* roman; int receiver; int thrower; DWORD since; };
Pass g_passes[4];

int PassReceiver(char* hook) {
	for (Pass& p : g_passes) {
		if (!p.roman || p.roman != hook) continue;
		bool flying = *(uint8_t*)(hook + 0x21) == 4;
		static const bool passLog = getenv("XXL_PASSLOG") != nullptr;   // diagnostics: the flight, tick by tick
		if (passLog) {
			char* node = *(char**)(hook + 0x10);
			char* fr = node ? *(char**)(node + 4) : nullptr;
			const float* q = fr ? (const float*)(fr + 0x80) : nullptr;
			Log("proximity: passed Roman %p state %d flags %08X aim %p at (%.1f %.1f %.1f)", hook, *(uint8_t*)(hook + 0x21),
			    *(uint32_t*)(hook + 0x24), *(void**)(hook + 0x1A0), q ? q[0] : 0.0f, q ? q[1] : 0.0f, q ? q[2] : 0.0f);
		}
		if ((flying || GetTickCount() - p.since < 300) && GetTickCount() - p.since < 6000) return p.receiver;
		{
			char* node = *(char**)(hook + 0x10);
			char* fr = node ? *(char**)(node + 4) : nullptr;
			const float* q = fr ? (const float*)(fr + 0x80) : nullptr;
			Log("proximity: passed Roman %p done (state %d) at (%.1f %.1f %.1f)", hook, *(uint8_t*)(hook + 0x21),
			    q ? q[0] : 0.0f, q ? q[1] : 0.0f, q ? q[2] : 0.0f);
		}
		p.roman = nullptr;
	}
	return -1;
}

// Diagnostics (XXL_PASSLOG): events sent to a passed Roman, with the caller.
typedef int(__fastcall* RomanSendFn)(char* self, void* edx, int ev, void* param);
const uintptr_t kRomanVtbls[5] = { 0x627240, 0x62745C, 0x62756C, 0x627738, 0x627844 };
const uintptr_t kRomanSends[5] = { 0x4DA3F0, 0x4DC840, 0x4DE8B0, 0x4E3F00, 0x4E5F90 };
RomanSendFn g_origRomanSend[5] = {};

int __fastcall RomanSendHook(char* self, void* edx, int ev, void* param) {
	uintptr_t vt = *(uintptr_t*)self;
	RomanSendFn orig = nullptr;
	for (int i = 0; i < 5; i++) if (kRomanVtbls[i] == vt) orig = g_origRomanSend[i];
	if (!orig) return 0;
	for (Pass& p : g_passes)
		if (p.roman == self && GetTickCount() - p.since < 3000 && (ev & 0xFFFF) != 0x4C01 && (ev & 0xFF00) != 0x0300)
			Log("proximity: passed Roman %p gets event %04X (%p) from %p, state %d", self, ev & 0xFFFF, param,
			    _ReturnAddress(), *(uint8_t*)(self + 0x21));
	return orig(self, edx, ev, param);
}

// A launcher (CKHkTrioCatapult, 2,190, vf15) offers itself (trio 0x5130: the
// leader's +0x544 = it, +0x3F0 bit 0x4000 -- the hero's "Activate", its X then
// starts it) to the leader standing on its platform (+0x48: the trio's vf29, the
// leader's ground) in its sector (+0x18 == level +0x24), while it is ready (+0x14
// bits 3, 6, 7 clear) and not counting its flight (+0x24 != +0x38). Ticked as its
// nearest hero's, the other player's hero on it too had no prompt and its X did
// nothing: it gets the same offer.
const uintptr_t kTrioCatapultVtbl = 0x6264A4;
void LauncherOfferBoth(char* l, Trio t, int p1, int p2) {
	if (!l || *(uintptr_t*)l != kTrioCatapultVtbl || !t) return;
	uint8_t st = *(uint8_t*)(l + 0x14);
	if ((st & 0xC8) || *(float*)(l + 0x24) == *(float*)(l + 0x38)) return;
	CKYellowPages* yp = YellowPages();
	char* lvl = yp ? *(char**)((char*)yp + 0x90) : nullptr;
	char* plat = *(char**)(l + 0x48);
	if (!lvl || !plat || *(int8_t*)(l + 0x18) != (int)*(uint8_t*)(lvl + 0x24)) return;
	const int heroes[2] = { p1, p2 };
	for (int i : heroes) {
		Hero h = t.GetHero(i);
		if (!h || HeroOut(i) || *(char**)(h.raw() + 0x280) != plat || (*(uint32_t*)(h.raw() + 0x384) & 7)) continue;
		uint32_t& f = *(uint32_t*)(h.raw() + 0x3F0);
		if ((f & 0x4000) && *(char**)(h.raw() + 0x544) == l) continue;
		*(char**)(h.raw() + 0x544) = l;
		f |= 0x4000;
		static char* told[2] = {};
		if (told[i & 1] != l) Log("proximity: launcher %p offered to %s on it too (its prompt, its X)", l, HeroName(i));
		told[i & 1] = l;
	}
}

void TickLife(int* life, void* dt, bool isGroup, Trio t, int p1, int p2, const Vector3& pos1,
              const Vector3& pos2) {
	TickFn tick = (TickFn)(*(void***)life)[17];
	uintptr_t vt = *(uintptr_t*)life;
	Vector3 at;
	char* hook = isGroup ? nullptr : *(char**)((char*)life + 0x10);
	struct HitterStats {   // a catapult's blow on its hitter's combo bar (individual heroes)
		int prev = -1;
		~HitterStats() { IndivLeave(prev); }
	} hitterStats;
	if (hook && *(uintptr_t*)hook == kCatapultVtbl) {
		int by = CatapultHitter(hook, t);
		if (by >= 0) hitterStats.prev = IndivEnter(by);
		static int s_told = 0;
		if (by >= 0 && by != t.Leader() && IndivActive() && s_told++ < 20)
			Log("proximity: catapult %p hit by %s (not leading) -- its tick puts the blow on that hero's combo bar", hook,
			    HeroName(by));
	}
	// Individual heroes: what needs both heroes skips its tick while the standing
	// hero is at it with the other down (the hero is put back too): it cannot start.
	if (hook && IndivHoldsBack(hook)) return;
	if (hook && *(uintptr_t*)hook == kLifterVtbl) *(float*)(hook + 0x34) = 0.0f;
	if (hook && *(uintptr_t*)hook == kPressionStoneVtbl && PlateForEither(hook)) {
		TickPlate(life, dt, hook, t);
		return;
	}
	if (hook && *(uintptr_t*)hook == kCorkscrewVtbl && TickCorkscrew(life, dt, hook, t)) return;
	bool plate = hook && (*(uintptr_t*)hook == kPressionStoneVtbl || *(uintptr_t*)hook == kCorkscrewVtbl);
	// A carried torch (CKHkTorch 2,32, +0x3E state 1) burns down only while the
	// trio's leader is Asterix (trio vf40, its tick at 0x4AF790); the HUD reads the
	// real leader. Ticked as Asterix's nearest with him posing as the leader, it
	// burned at his rate while Obelix led (single player: it waits).
	bool carriedTorch = hook && *(uintptr_t*)hook == kTorchVtbl && (*(uint8_t*)(hook + 0x3E) & 3) == 1;
	if (vt == kAsterixLifeVtbl || vt == kObelixLifeVtbl || vt == kIdefixLifeVtbl ||
	    vt == kBonusGroupLife || vt == kTrioGroupLife || vt == kCheckpointLife || plate || carriedTorch ||
	    !LifePos(life, isGroup, &at)) {
		tick(life, nullptr, dt);
		return;
	}
	float d1 = (at - pos1).len2(), d2 = (at - pos2).len2();
	int nearest = d2 < d1 ? p2 : p1;
	uint8_t* tr = (uint8_t*)t.raw();
	float range2 = kMaxRange2;
	int passTo = hook ? PassReceiver(hook) : -1;
	if (passTo >= 0) {
		nearest = passTo;
		range2 = 1e30f;
		// Leaving the thrower's hands it touches the thrower, which -- with the
		// receiver as the leader -- reads as a hit (+0x24 bit 0x80; the Roman's
		// state chooser, Class_2_84::vf26, then drops the flight for state 2).
		Hero th = t.GetHero(passTo == p1 ? p2 : p1);
		if (th && *(uint8_t*)(hook + 0x21) == 4 && (at - th.Pos()).len2() < 3.0f * 3.0f)
			*(uint32_t*)(hook + 0x24) &= ~0x80u;
	}
	int pusher = hook && *(uintptr_t*)hook == kPushPullVtbl ? PusherOf(hook, t, p1, p2) : -1;
	if (pusher >= 0) {
		nearest = pusher;
		range2 = 1e30f;
	} else if (hook && *(uintptr_t*)hook == kPushPullVtbl) {
		// (Only near that hero: a block far from it ticked as its -- LV4's at -913 37
		// -469 from the lever by the spitter at -734 14 -241 -- and the lever there
		// missed player 2's hits: before, it ticked with the real leader.)
		int only = BlockHero(hook);
		if ((only == p1 || only == p2) && (only == p1 ? d1 : d2) <= range2) {   // (a player's: both are in this pass)
			static char* s_seen[16];
			static int s_n = 0;
			bool seen = false;
			for (int i = 0; i < s_n; i++) seen |= s_seen[i] == hook;
			if (!seen && s_n < 16) {
				s_seen[s_n++] = hook;
				Log("proximity: block %p at (%.1f %.1f %.1f) is %s's (+14 %02X): ticks with it as the leader (%s %.1f away, %s %.1f)",
				    hook, at.x, at.y, at.z, HeroName(only), *(uint8_t*)(hook + 0x14), HeroName(p1), sqrtf(d1), HeroName(p2), sqrtf(d2));
			}
			nearest = only;
			range2 = 1e30f;
		}
	}
	if (hook && *(uintptr_t*)hook == kBonusSpitterVtbl && SpitsPotion(hook)) {
		nearest = HERO_ASTERIX;
		range2 = 1e30f;
	}
	// The merchant at its hero's (the one at it, leading or not): its states
	// ask the trio for the shopper (a purchase's 0x907, its camera's target).
	// A purchase writes the health and shields in the game's place (FUN_004BFC50:
	// a ham heals, a shield is added; "full" refusals read them): the shopper's
	// (individual heroes) -- they were the leader's, who got what the other bought.
	struct ShopStats {
		int prev = -1, hero = -1;
		uint8_t hp = 0, shields = 0;
		uint32_t helmets = 0;
		static char* Block() {
			CKYellowPages* yp = YellowPages();
			char* gm = yp ? (char*)yp->gameManager : nullptr;
			return gm ? *(char**)(gm + 0x40) : nullptr;
		}
		void Enter(int h) {
			hero = h;
			prev = IndivEnter(h);
			if (char* cb = Block()) hp = cb[0x50], shields = cb[0x51], helmets = *(uint32_t*)(cb + 0x68);
		}
		~ShopStats() {
			if (hero < 0) return;
			char* cb = Block();
			if (cb && *(uint32_t*)(cb + 0x68) != helmets)
				Log("proximity: %s bought from the merchant: helmets %u -> %u, health %u/%u -> %u/%u", HeroName(hero), helmets,
				    *(uint32_t*)(cb + 0x68), hp, shields, (uint8_t)cb[0x50], (uint8_t)cb[0x51]);
			IndivLeave(prev);
		}
	} shopStats;
	if (hook && *(uintptr_t*)hook == kShopVtbl) {
		int sh = CoopShopHero();
		if (sh == p1 || sh == p2) {
			nearest = sh;
			range2 = 1e30f;
			shopStats.Enter(sh);
		}
	}
	if (hook && *(uintptr_t*)hook == kClueManVtbl) {
		float r = *(const float*)(hook + 0x68);
		if (std::isfinite(r) && r * r > range2) range2 = r * r;
	}
	bool pushed = hook && *(uintptr_t*)hook == kPushPullVtbl && (*(uint8_t*)(hook + 0x18) & 0x10);
	static const bool pushLog = getenv("XXL_PUSHLOG") != nullptr;   // diagnostics: who a pushed block ticks with
	if (pushed && pushLog) {
		static DWORD lastLog = 0;
		if (GetTickCount() - lastLog > 500) {
			lastLog = GetTickCount();
			Log("proximity: pushed block %p at (%.2f %.2f %.2f): %s %.2f away, %s %.2f; ticks as %s", hook, at.x, at.y, at.z,
			    HeroName(p1), sqrtf(d1), HeroName(p2), sqrtf(d2), HeroName(nearest));
		}
	}
	struct LetGo {   // a block letting go of its pusher, logged (a few times)
		char* hook; bool was; uint8_t leader;
		~LetGo() {
			static int n = 0;
			if (was && !(*(uint8_t*)(hook + 0x18) & 0x10) && n < 20) {
				n++;
				Log("proximity: block %p let go of its pusher (ticked with %s as the leader)", hook, HeroName(leader));
			}
		}
	} letGo{ hook, pushed, 0 };
	if ((d1 > range2 && d2 > range2) || tr[0x2C] == nearest) {
		letGo.leader = tr[0x2C];
		tick(life, nullptr, dt);
		LauncherOfferBoth(hook, t, p1, p2);
		return;
	}
	letGo.leader = (uint8_t)nearest;
	uint8_t leader = tr[0x2C], companion = tr[0x2D];
	int other = nearest == p1 ? p2 : p1;
	tr[0x2C] = (uint8_t)nearest;
	tr[0x2D] = (uint8_t)other;
	InterlockedIncrement(&g_slices);
	tick(life, nullptr, dt);
	LauncherOfferBoth(hook, t, p1, p2);
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
	if (!CoopP2Joined() || CoopSuspended() || !h1 || !h2 || HeroOut(p1) || HeroOut(p2)) {
		// Individual heroes with one down (a hero out; co-op or single player's
		// second chance): the same ticks, but what needs both heroes skips its
		// tick while the standing hero is at it. Single player with the lead
		// changing hands: what answers one hero only (SoloTick).
		bool solo = t && SoloLeadFree();
		if ((!CoopSuspended() && IndivFallenHero() >= 0) || solo) {
			int* groupLife = *(int**)(bundle + 0x0C);
			if (groupLife && (groupLife[1] & 3) == 0) ((TickFn)(*(void***)groupLife)[17])(groupLife, nullptr, dt);
			for (int* life = *(int**)(bundle + 0x10); life; life = (int*)life[2]) {
				if (*(uint8_t*)(life + 1) & 2) continue;
				char* hook = *(char**)((char*)life + 0x10);
				if (hook && IndivHoldsBack(hook)) continue;
				if (solo && hook && SoloTick(life, dt, hook, t)) continue;
				// (second chance: a catapult's blow on its hitter's combo bar, as in co-op)
				int by = t && hook && IndivActive() && *(uintptr_t*)hook == kCatapultVtbl ? CatapultHitter(hook, t) : -1;
				int prev = by >= 0 ? IndivEnter(by) : -1;
				((TickFn)(*(void***)life)[17])(life, nullptr, dt);
				if (by >= 0) IndivLeave(prev);
			}
			return;
		}
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

// Reinforcements. A squad replaces its fallen Romans (vf37) at a point from
// FUN_00530060 (thiscall, out position, ret 4): just behind the game camera --
// camera service +0x5B0 position, minus +0x15C, plus a random share of +0x144
// sideways -- dropped onto the ground, inside the squad's box, reachable. With
// player 2's Obelix fighting alone, the game camera (player 1's) was far away:
// the point was never in the box and nobody came back (level 4 sector 1). And
// right behind player 1's camera is often in plain view of player 2's.
// For a squad run for the non-leading player the point is taken behind that
// player's own camera; any point a player can see is turned down (the squad
// asks again next frame).
typedef char(__fastcall* SpawnPointFn)(char* squad, void* edx, float* out);
const uintptr_t kSpawnPoint = 0x530060;
const uint8_t   kSpawnPointPrologue[7] = { 0x6A, 0xFF, 0x68, 0xA8, 0xFE, 0x61, 0x00 };
SpawnPointFn g_origSpawnPoint = nullptr;
bool g_squadForNonLeader = false;   // inside EnemyTickHook's non-leader pass
volatile long g_spawnsMoved = 0, g_spawnsHidden = 0;

char __fastcall SpawnPointHook(char* squad, void* edx, float* out) {
	CKYellowPages* yp = YellowPages();
	char* svc = yp ? *(char**)((char*)yp + 0x68) : nullptr;
	if (!CoopP2Joined() || CoopSuspended() || !svc) return g_origSpawnPoint(squad, edx, out);
	float saved[9];
	float m[16];
	bool swapped = false;
	if (g_squadForNonLeader && OtherViewMatrix(m)) {
		float* pos = (float*)(svc + 0x5B0);
		float* back = (float*)(svc + 0x15C);
		float* side = (float*)(svc + 0x144);
		memcpy(saved, pos, 12); memcpy(saved + 3, back, 12); memcpy(saved + 6, side, 12);
		const float* look = (const float*)(svc + 0x5BC);
		float fwd[3] = { look[0] - pos[0], look[1] - pos[1], look[2] - pos[2] };
		float sgn = back[0] * fwd[0] + back[1] * fwd[1] + back[2] * fwd[2] >= 0 ? 1.0f : -1.0f;
		float lb = sqrtf(back[0] * back[0] + back[1] * back[1] + back[2] * back[2]);
		float ls = sqrtf(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
		for (int k = 0; k < 3; k++) {
			pos[k] = m[12 + k];
			back[k] = sgn * lb * m[8 + k];
			side[k] = ls * m[k];
		}
		swapped = true;
	}
	char ok = g_origSpawnPoint(squad, edx, out);
	if (swapped) {
		memcpy(svc + 0x5B0, saved, 12); memcpy(svc + 0x15C, saved + 3, 12); memcpy(svc + 0x144, saved + 6, 12);
		if (ok && InterlockedIncrement(&g_spawnsMoved) % 20 == 1)
			Log("proximity: Roman reinforcement for %s's fight behind that player's camera (%.1f %.1f %.1f)",
			    HeroName(GetTrio() ? GetTrio().Leader() : -1), out[0], out[1], out[2]);   // posing as the leader
	}
	if (ok && KlusterSphereInAnyView(out, 1.5f, 40.0f)) {
		if (InterlockedIncrement(&g_spawnsHidden) % 60 == 1)
			Log("proximity: Roman reinforcement point (%.1f %.1f %.1f) in a player's view -- not there", out[0], out[1], out[2]);
		return 0;
	}
	return ok;
}

// Squads with spawn markers (squad +0x5C, 8 bytes each: +0 -> position;
// count +0x64) bring Romans in with vf38 (thiscall(count, member group),
// ret 8) instead: a marker picked from the game camera's position and look
// (camera service +0x5B0 / +0x5BC). Player 2 watched them pop up on a marker
// behind player 1's camera (level 3 sector 6). For a squad run for the
// non-leading player that player's camera is used, and for every squad the
// markers a player can see within 40 units are left out of the list for the call
// -- in a view with no collision wall in between: the levels hide their markers
// behind rocks and walls (LV5-6's marker 28 behind the plateau, in front of both
// cameras: nobody came while it counted as seen; single player brings Romans in
// there, the game's vf38 prefers a marker in front of its camera facing it).
// With all of them left out the call is skipped: its Romans stay asked for (the
// member group's +0x14, which only a spawn takes down; the game's own call skips
// too when no marker suits its camera) and come at the next call that finds a
// marker out of view -- or, with romans_spawn_in_view, at once from the farthest.
typedef void(__fastcall* MarkerSpawnFn)(char* squad, void* edx, int count, unsigned group);
const uintptr_t kMarkerSpawn = 0x52FB80;
MarkerSpawnFn g_origMarkerSpawn = nullptr;
volatile long g_markersHidden = 0;
char* g_markersWaitSquad = nullptr;   // the squad whose Romans last waited for a marker out of view
volatile long g_markersWaited = 0, g_markersInView = 0;
struct WaitedSquad { char* sq; bool hadMembers; };
WaitedSquad g_waitedSquads[8] = {};   // squads whose Romans waited, until their end (EndSpentSquad)

void NoteWaitedSquad(char* sq) {
	for (WaitedSquad& w : g_waitedSquads) if (w.sq == sq) return;
	WaitedSquad* slot = &g_waitedSquads[0];
	for (WaitedSquad& w : g_waitedSquads) if (!w.sq) { slot = &w; break; }
	slot->sq = sq;
	slot->hadMembers = true;
}

// While its Romans wait the rest of a squad's Romans can be beaten without
// them: the Twister counts Romans still to come as beaten (CKGrpSquadEnemy
// vf41 0x4C8CB0: +0x16 up to +0x18, event 0x4D09 and a helmet each), and the
// asked ones are dropped then (vf37: nothing left to send). The squad's end
// (FUN_00530570: event 0xD16, +0x154 = -2, the fight's gate opens) is looked at
// only as one of its Romans goes -- with none left, LV5-6's fight (squad 4)
// never ended. Ended here as the game would, once nothing is left to send or
// to beat and no Roman of it is there; one that ran out of Romans with nothing
// left (in its tick, just before this) has had the game's own end.
void EndSpentSquad(char* sq) {
	WaitedSquad* slot = nullptr;
	for (WaitedSquad& w : g_waitedSquads) if (w.sq == sq) slot = &w;
	if (!slot) return;
	void** vt = *(void***)sq;
	char* recs = *(char**)(sq + 0x144);
	unsigned n = *(unsigned*)(sq + 0x14C);
	if (!recs || n == 0 || n > 16) return;
	bool spent = true;
	for (unsigned r = 0; r < n && spent; r++) {
		const char* rec = recs + r * 0x1C;
		int16_t toDefeat = *(const int16_t*)(rec + 0x18);
		spent = toDefeat < 0 ? !rec[0x14] && !rec[0x15] : *(const uint16_t*)(rec + 0x16) >= (uint16_t)toDefeat;
	}
	typedef int* (__fastcall* ListFn)(char* self, void* edx);
	int* members = ((ListFn)vt[12])(sq, nullptr);
	bool had = slot->hadMembers;
	slot->hadMembers = members && *members;
	if (!spent) return;
	if (slot->hadMembers || had) {   // the game's own end: as its last Roman goes
		if (!slot->hadMembers) slot->sq = nullptr;
		return;
	}
	slot->sq = nullptr;
	Log("proximity: squad %p: its last Romans were beaten while they waited for a marker out of view -- the squad's "
	    "end (0D16)", sq);
	typedef void(__fastcall* SendFn)(char* self, void* edx, int ev, int arg);
	((SendFn)vt[5])(sq, nullptr, 0xD16, 0);
	*(uint8_t*)(sq + 0x154) = 0xFE;
}

// A collision wall between two points (the levels' walls, the invisible ones
// too): the game's own wall sweep, which keeps the camera out of walls --
// FUN_00449C90 on CKSrvCollision (yellow pages +0x50), thiscall(segment {start,
// delta}, height, result (14 dwords, +0x24 = how far along the hit is, 2 = none),
// 0, gather 1), ret 0x14. It gathers at most 40 walls a call, so the line goes
// in pieces of 8 units.
typedef uint32_t(__fastcall* WallSweepFn)(void* coll, void* edx, float* seg, float height, float* result, int unused,
                                          int gather);
const uintptr_t kWallSweep = 0x449C90;

bool WallBetween(const float* a, const float* b) {
	CKYellowPages* yp = YellowPages();
	void* coll = yp ? *(void**)((char*)yp + 0x50) : nullptr;
	if (!coll) return false;
	float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
	int pieces = (int)(sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) / 8.0f) + 1;
	if (pieces > 8) pieces = 8;
	for (int i = 0; i < pieces; i++) {
		float seg[6], res[16] = {};
		for (int k = 0; k < 3; k++) {
			seg[k] = a[k] + d[k] * i / pieces;
			seg[3 + k] = d[k] / pieces;
		}
		res[9] = 2.0f;
		if ((((WallSweepFn)kWallSweep)(coll, nullptr, seg, 0.0f, res, 0, 1) & 0xFF) && res[9] <= 1.0f) return true;
	}
	return false;
}

// Can a camera at eye see a Roman standing on the marker at p (its feet or its
// head, no wall in between)? Asked every frame while Romans are asked for:
// answers kept per marker and eye.
bool SeesMarker(const float* eye, const float* p) {
	struct Seen { float p[3], eye[3]; bool seen; };
	static Seen s_seen[16] = {};
	static int s_next = 0;
	for (const Seen& s : s_seen)
		if (!memcmp(s.p, p, 12) && !memcmp(s.eye, eye, 12)) return s.seen;
	float feet[3] = { p[0], p[1] + 0.5f, p[2] }, head[3] = { p[0], p[1] + 1.8f, p[2] };
	Seen& s = s_seen[s_next++ & 15];
	memcpy(s.p, p, 12);
	memcpy(s.eye, eye, 12);
	s.seen = !WallBetween(eye, head) || !WallBetween(eye, feet);
	return s.seen;
}

volatile long g_markersBehindWalls = 0;

// In a player's view within 40 units, with no wall in between.
bool SpawnMarkerSeen(const float* p) {
	const float* eyes[4];
	int n = KlusterViewsSeeing(p, 1.5f, 40.0f, eyes);
	for (int i = 0; i < n; i++)
		if (SeesMarker(eyes[i], p)) return true;
	if (n && InterlockedIncrement(&g_markersBehindWalls) % 300 == 1)
		Log("proximity: spawn marker (%.1f %.1f %.1f) in a player's view behind a wall -- Romans may come there", p[0],
		    p[1], p[2]);
	return false;
}

void __fastcall MarkerSpawnHook(char* squad, void* edx, int count, unsigned group) {
	CKYellowPages* yp = YellowPages();
	char* svc = yp ? *(char**)((char*)yp + 0x68) : nullptr;
	unsigned n = *(unsigned*)(squad + 0x64);
	char* list = *(char**)(squad + 0x5C);
	if (!CoopP2Joined() || CoopSuspended() || !svc || !list || n == 0 || n > 64) {
		g_origMarkerSpawn(squad, edx, count, group);
		return;
	}
	uint32_t keep[64 * 2];
	unsigned m = 0;
	// The game measures from its own camera with a cone of 1.5 x the camera's
	// field of view (camera +0x38 -> +0x44, degrees): first a marker inside it
	// that faces the camera, else one outside it. Here the other view gets the
	// same cone (within 40 units), and no marker within 8 units of a hero.
	char* camObj = *(char**)(svc + 0x38);
	float fov = camObj ? *(float*)(camObj + 0x44) : 60.0f;
	if (!(fov > 10.0f && fov < 170.0f)) fov = 60.0f;
	float cosLimit = cosf(fov * 1.5f * 0.5f * 0.01745329f);
	float om[16];
	bool other = OtherViewMatrix(om);
	Trio tt = GetTrio();
	Hero ha = tt ? tt.GetHero(HERO_ASTERIX) : Hero(), ho = tt ? tt.GetHero(HERO_OBELIX) : Hero();
	for (unsigned i = 0; i < n; i++) {
		const float* p = *(const float**)(list + i * 8);
		if (p) {
			Vector3 pv(p[0], p[1], p[2]);
			if (SpawnMarkerSeen(p)) continue;
			if ((ha && (ha.Pos() - pv).len2() < 64.0f) || (ho && (ho.Pos() - pv).len2() < 64.0f)) continue;
			if (other) {
				float dx = p[0] - om[12], dy = p[1] - om[13], dz = p[2] - om[14];
				float d = sqrtf(dx * dx + dy * dy + dz * dz);
				if (d < 40.0f && d > 0.01f && (dx * om[8] + dy * om[9] + dz * om[10]) / d >= cosLimit &&
				    SeesMarker(om + 12, p))
					continue;
			}
		}
		memcpy(keep + m * 2, list + i * 8, 8);
		m++;
	}
	if (m < n && InterlockedIncrement(&g_markersHidden) % 120 == 1)
		Log("proximity: squad %p: %u of %u spawn markers in a player's view, left out", squad, n - m, n);
	static const bool logMarkers = getenv("XXL_LOGMARKERS") != nullptr;
	if (logMarkers) {
		float m2[16];
		bool have = OtherViewMatrix(m2);
		for (unsigned i = 0; i < n; i++) {
			const float* p = *(const float**)(list + i * 8);
			if (!p) continue;
			float dx = p[0] - m2[12], dy = p[1] - m2[13], dz = p[2] - m2[14];
			float d = sqrtf(dx * dx + dy * dy + dz * dz);
			float dot = d > 0 ? (dx * m2[8] + dy * m2[9] + dz * m2[10]) / d : 0;
			Log("proximity: marker %u (%.1f %.1f %.1f): in a view %d, seen %d; other view %d at %.1f, cos to its axis %.2f",
			    i, p[0], p[1], p[2], KlusterSphereInAnyView(p, 1.5f, 40.0f), SpawnMarkerSeen(p), have, d, dot);
		}
	}
	// All in sight: the Romans wait -- the member group's +0x14 keeps them asked
	// for, and the next call that finds a marker out of view brings them. LV3-6's
	// fight has its three markers in the two views for 20 s and more at a time;
	// with the farthest taken anyway, a batch popped up there in plain view. With
	// romans_spawn_in_view they still come at once, from the marker farthest from
	// both heroes (round 82: LV5-6 squad 4 with both heroes standing in the arena).
	// (A point behind the camera, as squads without markers take, was no way out:
	// in a fight it lies by the other hero, in that player's view.)
	if (m == 0 && !g_config.romansInView) {
		const char* rec = group < *(unsigned*)(squad + 0x14C) ? *(char**)(squad + 0x144) + group * 0x1C : nullptr;
		if (squad != g_markersWaitSquad && InterlockedIncrement(&g_markersWaited) <= 40) {
			const float* p = *(const float**)list;
			Log("proximity: squad %p: all %u spawn markers in a player's view (the first at %.1f %.1f %.1f) -- %d Romans of "
			    "group %u wait for one out of view (asked %u, alive %u, sent %d of %d)", squad, n, p ? p[0] : 0, p ? p[1] : 0,
			    p ? p[2] : 0, count, group, rec ? rec[0x14] : 0, rec ? rec[0x11] : 0, rec ? *(short*)(rec + 0x16) : 0,
			    rec ? *(short*)(rec + 0x18) : 0);
		}
		g_markersWaitSquad = squad;
		NoteWaitedSquad(squad);
		return;
	}
	if (m > 0 && squad == g_markersWaitSquad) {
		g_markersWaitSquad = nullptr;
		if (InterlockedIncrement(&g_markersWaited) <= 40)
			Log("proximity: squad %p: %u of %u spawn markers out of view again -- %d Romans of group %u come", squad, m, n,
			    count, group);
	}
	if (m == 0) {
		float best = -1.0f;
		for (unsigned i = 0; i < n; i++) {
			const float* p = *(const float**)(list + i * 8);
			if (!p) continue;
			Vector3 pv(p[0], p[1], p[2]);
			float da = ha ? (ha.Pos() - pv).len2() : 1e12f, dd = ho ? (ho.Pos() - pv).len2() : 1e12f;
			float d = da < dd ? da : dd;
			if (d > best) { best = d; memcpy(keep, list + i * 8, 8); m = 1; }
		}
		if (m == 0) { g_origMarkerSpawn(squad, edx, count, group); return; }
		const float* p = *(const float**)keep;
		if (InterlockedIncrement(&g_markersInView) % 60 == 1)
			Log("proximity: squad %p: all %u spawn markers in a player's view -- %d Romans of group %u at the farthest "
			    "(%.1f %.1f %.1f) anyway (romans_spawn_in_view)", squad, n, count, group, p[0], p[1], p[2]);
	}
	float saved[6], mtx[16];
	bool swapped = g_squadForNonLeader && OtherViewMatrix(mtx);
	if (swapped) {
		float* pos = (float*)(svc + 0x5B0);
		float* look = (float*)(svc + 0x5BC);
		memcpy(saved, pos, 12); memcpy(saved + 3, look, 12);
		for (int k = 0; k < 3; k++) { pos[k] = mtx[12 + k]; look[k] = mtx[12 + k] + mtx[8 + k] * 5.0f; }
	}
	*(char**)(squad + 0x5C) = (char*)keep;
	*(unsigned*)(squad + 0x64) = m;
	g_origMarkerSpawn(squad, edx, count, group);
	*(char**)(squad + 0x5C) = list;
	*(unsigned*)(squad + 0x64) = n;
	if (swapped) { memcpy(svc + 0x5B0, saved, 12); memcpy(svc + 0x5BC, saved + 3, 12); }
}

bool InSquadBox(const char* sq, const Vector3& p) {
	const float* f = (const float*)sq;
	return f[0x4A] < p.x && p.x <= f[0x47] && f[0x4C] < p.z && p.z <= f[0x49] && f[0x4B] < p.y && p.y <= f[0x48];
}

// The squad's fight zone (vf46's test): max +0x104..+0x10C, min +0x110..+0x118.
int InFightZone(const char* sq, const Vector3& p) {
	const float* f = (const float*)sq;
	return f[0x44] <= p.x && p.x <= f[0x41] && f[0x46] <= p.z && p.z <= f[0x43] && f[0x45] <= p.y && p.y <= f[0x42];
}

// Romans of a squad shared between both heroes. The squad runs for one hero:
// its formation centre (node squad +0x30, position +0x30) goes after that hero
// (vf31 -> FUN_0052BAB0) and each Roman is given a slot around the centre
// (FUN_0052B670: the Roman's component 0x46, +0 slot, +0xC facing, +0x18 the
// step towards the slot: (slot - Roman) in x/z over max(distance, squad
// +0xC0)). The Romans' own ticks attack the nearest hero, but they only go
// where their slots are: the other hero was never chased. With both heroes in
// the fight zone, half the Romans have their slots moved around the other hero
// (same place relative to him as to the centre). Who goes for whom is kept
// from frame to frame; a newcomer joins the smaller side (the nearer hero on a
// tie) and one Roman a frame changes side while they are uneven.
std::unordered_map<char*, uint8_t> g_romanSide;   // Roman -> the hero (index) its slot is around
volatile long g_splitLogs = 0;

void SplitSquad(char* sq, int target, Hero targetHero, int other, Hero otherHero) {
	char* centre = *(char**)(sq + 0x30);
	char* rec0 = *(char**)(sq + 0x144);
	unsigned nRec = *(unsigned*)(sq + 0x14C);
	if (!centre || !rec0 || nRec > 8) return;
	Vector3 c(*(float*)(centre + 0x30), *(float*)(centre + 0x34), *(float*)(centre + 0x38));
	Vector3 heroPos[2] = { targetHero.Pos(), otherHero.Pos() };
	struct Member { char* m; Vector3 p; int side; };
	Member mem[64];
	int n = 0, count[2] = {};
	for (unsigned r = 0; r < nRec; r++)
		for (char* m = *(char**)(rec0 + r * 0x1C + 8); m && n < 64; m = *(char**)(m + 0x50)) {
			Vector3 p;
			if (!NodePos(*(void**)(m + 0x10), &p)) continue;
			auto it = g_romanSide.find(m);
			int side = it == g_romanSide.end() ? -1 : it->second == (uint8_t)target ? 0 : it->second == (uint8_t)other ? 1 : -1;
			mem[n++] = { m, p, side };
			if (side >= 0) count[side]++;
		}
	int half = (n + 1) / 2;
	for (int i = 0; i < n; i++) {
		if (mem[i].side >= 0) continue;
		int s = (mem[i].p - heroPos[1]).len2() < (mem[i].p - heroPos[0]).len2() ? 1 : 0;
		if (count[s] >= half) s ^= 1;
		mem[i].side = s;
		count[s]++;
	}
	if (count[0] > count[1] + 1 || count[1] > count[0] + 1) {
		int from = count[0] > count[1] ? 0 : 1, best = -1;
		float bestD = 1e30f;
		for (int i = 0; i < n; i++)
			if (mem[i].side == from && (mem[i].p - heroPos[from ^ 1]).len2() < bestD) {
				bestD = (mem[i].p - heroPos[from ^ 1]).len2();
				best = i;
			}
		if (best >= 0) { mem[best].side = from ^ 1; count[from]--; count[from ^ 1]++; }
	}
	typedef char*(__fastcall* CpntFn)(char* self, void* edx, int id);
	float minStep = *(float*)(sq + 0xC0);
	for (int i = 0; i < n; i++) {
		g_romanSide[mem[i].m] = (uint8_t)(mem[i].side ? other : target);
		if (!mem[i].side) continue;
		float* slot = (float*)((CpntFn)(*(void***)mem[i].m)[12])(mem[i].m, nullptr, 0x46);
		if (!slot) continue;
		Vector3 s(slot[0] - c.x + heroPos[1].x, slot[1] - c.y + heroPos[1].y, slot[2] - c.z + heroPos[1].z);
		slot[0] = s.x; slot[1] = s.y; slot[2] = s.z;
		float dx = s.x - mem[i].p.x, dz = s.z - mem[i].p.z, len = sqrtf(dx * dx + dz * dz);
		float k = len <= 0.01f ? 0.0f : 1.0f / (len > minStep ? len : minStep);
		slot[6] = dx * k; slot[7] = 0.0f; slot[8] = dz * k;
	}
	if (InterlockedIncrement(&g_splitLogs) % 600 == 1)
		Log("proximity: squad %p shared: %d Romans go for %s, %d for %s", sq, count[0], HeroName(target), count[1],
		    HeroName(other));
}

// Out of Romans. A squad coming alive takes its Romans from the shared pool
// (vf33); when the pool runs dry it calls vf36 (0x52F850), which sends back to
// the pool (vf34) every active squad of its enemy group whose box does not hold
// the game camera's look-at point (camera service +0x5BC) -- in single player,
// every squad away from the leader. With player 1 fighting elsewhere, the
// squads around player 2's Obelix freed each other every frame: their Romans
// were spawned over and over at their starting spots, hanging in the air above
// the lifters (level 5 sector 8) until Asterix came along. Squads around either
// player's hero are kept as well -- but for a squad coming alive round the
// leader (vf33 asks; in single player the leader's squads come first), one with
// only the other hero in its box, not fighting (ProximityHeroFighting), is not:
// the user's LV3-6, Obelix left at the level's start (squad #19's box) held the
// Romans of the pool squad #4 shares with it -- none of the fight's came. Never
// the other way round (two squads freeing each other as each comes alive). Such
// a squad is not woken for the other hero (EnemyTickHook: a hero in an asleep
// squad's box wakes it -- it took them straight back before the fight's next
// Romans were sent in) while the one it was sent back for is awake with Romans
// left to send in (a pool endless, or sent +0x16 short of its count +0x18, or
// some asked +0x14) and that hero is not fighting.
typedef void(__fastcall* SquadFreeFn)(char* squad, void* edx);
const uintptr_t kSquadFree = 0x52F850;
SquadFreeFn g_origSquadFree = nullptr;
volatile long g_squadsSpared = 0, g_squadsTaken = 0;
struct HeldBack { char* sq; char* taker; };
HeldBack g_heldBack[8];

bool MoreToSend(const char* sq) {
	const char* recs = *(char* const*)(sq + 0x144);
	unsigned nr = *(const unsigned*)(sq + 0x14C);
	for (unsigned r = 0; recs && r < nr && r < 16; r++) {
		const char* rec = recs + r * 0x1C;
		int16_t count = *(const int16_t*)(rec + 0x18);
		if (count < 0 || *(const uint16_t*)(rec + 0x16) < (uint16_t)count || rec[0x14]) return true;
	}
	return false;
}

void HoldBack(char* sq, char* taker) {
	HeldBack* slot = &g_heldBack[0];
	for (HeldBack& h : g_heldBack) {
		if (h.sq == sq) { slot = &h; break; }
		if (!h.sq) slot = &h;
	}
	*slot = { sq, taker };
}

// Is asleep `sq` (in `grp`) kept from waking for the other hero `hero`?
bool HeldBackFor(char* grp, char* sq, int hero) {
	for (HeldBack& h : g_heldBack) {
		if (h.sq != sq) continue;
		bool taker = false;
		for (char* s = *(char**)(grp + 0x0C); s && !taker; s = *(char**)(s + 4)) taker = s == h.taker;
		if (taker && (*(uint8_t*)(h.taker + 0x152) & 2) && MoreToSend(h.taker) && !ProximityHeroFighting(hero))
			return true;
		static int s_told = 0;
		if (s_told++ < 20)
			Log("proximity: squad %p may come alive for %s again (squad %p %s)", sq, HeroName(hero), h.taker,
			    !taker ? "gone" : !(*(uint8_t*)(h.taker + 0x152) & 2) ? "asleep" : MoreToSend(h.taker) ? "still sending, the hero fights"
			                                                                                : "has sent all its Romans");
		h = HeldBack();
		return false;
	}
	return false;
}

void __fastcall SquadFreeHook(char* squad, void* edx) {
	CKYellowPages* yp = YellowPages();
	char* svc = yp ? *(char**)((char*)yp + 0x68) : nullptr;
	Trio t = GetTrio();
	Hero ha = t ? t.GetHero(HERO_ASTERIX) : Hero(), ho = t ? t.GetHero(HERO_OBELIX) : Hero();
	char* grp = *(char**)(squad + 0x14);
	if (!CoopP2Joined() || CoopSuspended() || !svc || !ha || !ho || !grp) {
		g_origSquadFree(squad, edx);
		return;
	}
	typedef char(__fastcall* IsSubFn)(void* self, void* edx, int id);
	typedef void(__fastcall* VoidFn)(void* self, void* edx);
	const float* look = (const float*)(svc + 0x5BC);
	Vector3 lp(look[0], look[1], look[2]), pa = ha.Pos(), po = ho.Pos();
	int lead = t.Leader(), other = 1 - lead;   // (the real leader: a squad run for the other has them swapped meanwhile)
	Vector3 pl = lead == HERO_ASTERIX ? pa : po, pn = lead == HERO_ASTERIX ? po : pa;
	bool forLeader = !g_squadForNonLeader && lead <= HERO_OBELIX && InSquadBox(squad, pl) && !InSquadBox(squad, pn);
	for (char* sq = *(char**)(grp + 0x0C); sq; sq = *(char**)(sq + 4)) {
		void** vt = *(void***)sq;
		if (!((IsSubFn)vt[1])(sq, nullptr, 0x604) || !(*(uint8_t*)(sq + 0x152) & 2)) continue;
		if (InSquadBox(sq, lp) || NoLockKeepsSquad(sq) || GuardKeepsSquad(sq)) continue;   // (Romans by a waiting / downed hero stay)
		bool inL = InSquadBox(sq, pl), inN = InSquadBox(sq, pn);
		if (inL || (inN && (!forLeader || ProximityHeroFighting(other)))) {
			if (InterlockedIncrement(&g_squadsSpared) % 300 == 1)
				Log("proximity: out of Romans; squad %p kept for the player in it", sq);
			continue;
		}
		if (inN) {
			HoldBack(sq, squad);
			if (InterlockedIncrement(&g_squadsTaken) <= 20)
				Log("proximity: out of Romans; squad %p, with only %s in its box (not fighting), sends its Romans back for squad "
				    "%p coming alive round %s", sq, HeroName(other), squad, HeroName(lead));
		}
		((VoidFn)vt[34])(sq, nullptr);
	}
}

void __fastcall EnemyTickHook(char* life, void* edx, char* dt) {
	Trio t = GetTrio();
	int nl = CoopNonLeaderPlayer();
	char* grp = *(char**)(life + 0x0C);
	Hero lead = t ? t.GetHero(t.Leader()) : Hero(), other = t && nl >= 0 ? t.GetHero(nl) : Hero();
	if (!grp || !lead || !other || !CoopP2Joined() || CoopSuspended() || HeroOut(nl)) {
		g_origEnemyTick(life, edx, dt);
		for (char* sq = grp ? *(char**)(grp + 0x0C) : nullptr; sq; sq = *(char**)(sq + 4)) {
			EndSpentSquad(sq);
			NoLockSteer(sq);
			GuardSteer(sq);   // (a hero down: the non-leader is out)
		}
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
		// both heroes in the box and the fight zone of a squad after the trio's leader (target 5)
		bool shared = inL && inO && ((*(uint8_t*)(sq + 0x152) >> 2) & 7) == 5 && InFightZone(sq, pl) &&
		              InFightZone(sq, po) && !HeroOut(t.Leader());
		if (inL && inO) {
			// Big boxes hold both. The squad's script starts the fight on its target
			// being in the fight zone (+0x104..+0x10C max, +0x110..+0x118 min; vf46,
			// events 0x4D10/0x4D11): run for a hero inside it. LV3-6's Romans stood
			// in formation by Asterix while the squad ran for Obelix behind the
			// bars, nearer the box's middle. Else the hero nearer the middle.
			int zl = InFightZone(sq, pl), zo = InFightZone(sq, po);
			if (zl != zo) {
				inL = zl > zo;
				static char* s_lastSq = nullptr;
				static int s_logged = 0;
				if (sq != s_lastSq && s_logged < 40) {
					s_lastSq = sq;
					s_logged++;
					Log("proximity: squad %p: both heroes in its box, only %s in its fight zone: run for that hero", sq,
					    HeroName(inL ? t.Leader() : nl));
				}
			} else {
				const float* f = (const float*)sq;
				Vector3 c((f[0x47] + f[0x4A]) * 0.5f, (f[0x48] + f[0x4B]) * 0.5f, (f[0x49] + f[0x4C]) * 0.5f);
				if ((po - c).len2() < (pl - c).len2()) inL = false;
			}
		}
		if (inL) {
			((DtFn)vt[31])(sq, nullptr, dt);
			if (shared) SplitSquad(sq, t.Leader(), lead, nl, other);
		} else if (inO && !(*(uint8_t*)(sq + 0x152) & 2) && HeldBackFor(grp, sq, nl)) {
			*(float*)(sq + 0x138) += *(float*)(dt + 0xC);   // asleep, as below
		} else if (inO) {
			uint8_t leader = tr[0x2C], companion = tr[0x2D];
			tr[0x2C] = (uint8_t)nl;
			tr[0x2D] = leader;
			*(uint32_t*)(tr + 0x3F4) &= ~1u;   // the trio's enemy cache is per leader
			g_squadForNonLeader = true;
			((DtFn)vt[31])(sq, nullptr, dt);
			g_squadForNonLeader = false;
			if (tr[0x2C] == (uint8_t)nl) { tr[0x2C] = leader; tr[0x2D] = companion; }
			*(uint32_t*)(tr + 0x3F4) &= ~1u;
			if (shared) SplitSquad(sq, nl, other, t.Leader(), lead);
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
		EndSpentSquad(sq);
		NoLockSteer(sq);
		GuardSteer(sq);
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
	if (!t || nl < 0 || !CoopP2Joined() || CoopSuspended() || HeroOut(nl)) return;
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
// the fight alive; then the game's own pass for the real leader. Romans that
// no-lock mode sent for a waiting hero start no fight (nolock.cpp).
typedef void(__fastcall* FightFn)(char* trio, void* edx, void* time);
const uintptr_t kFightMode = 0x481950;
const uint8_t   kFightModePrologue[7] = { 0x51, 0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x06 };
FightFn g_origFight = nullptr;
volatile long g_p2Fights = 0;
// Per hero: game time an enemy was last within the fight radius (a pass that
// refreshed +0x408), for the fight camera of each player's own camera.
float g_fightSeen[2] = { -1000.0f, -1000.0f };
float g_fightNow = 0.0f, g_fightDelay = 0.0f;

void __fastcall FightHook(char* trio, void* edx, void* time) {
	Trio t = GetTrio();
	int nl = CoopNonLeaderPlayer();
	float now = time ? *(float*)((char*)time + 4) : 0.0f;
	float& last = *(float*)(trio + 0x408);
	bool nlFound = false;
	NoLockHideFromFight(true);
	if (t && trio == t.raw() && nl >= 0 && CoopP2Joined() && !CoopSuspended() && !HeroOut(nl)) {
		uint8_t* tr = (uint8_t*)trio;
		uint8_t leader = tr[0x2C], companion = tr[0x2D];
		bool wasOn = (*(uint32_t*)(tr + 0x3F4) & 0x40) != 0;
		float before = last;
		tr[0x2C] = (uint8_t)nl;
		tr[0x2D] = leader;
		*(uint32_t*)(tr + 0x3F4) &= ~1u;   // the enemy cache is the leader's
		g_origFight(trio, edx, time);
		if (tr[0x2C] == (uint8_t)nl) { tr[0x2C] = leader; tr[0x2D] = companion; }
		*(uint32_t*)(tr + 0x3F4) &= ~1u;
		nlFound = last != before;
		if (nlFound && nl < 2) g_fightSeen[nl] = now;
		if (!wasOn && (*(uint32_t*)(tr + 0x3F4) & 0x40)) {
			InterlockedIncrement(&g_p2Fights);
			Log("proximity: fight mode (music) started by %s (not leading)", HeroName(nl));
		}
	}
	// The game's own pass, for the leader. After a refresh by the other hero,
	// +0x408 is set just short of now so that the leader's own refresh shows
	// (and the fight cannot end in between).
	float mark = nlFound ? now - 0.01f : last;
	last = mark;
	int lead = t && trio == t.raw() ? t.Leader() : -1;
	g_origFight(trio, edx, time);
	if (last != mark && lead >= 0 && lead < 2) g_fightSeen[lead] = now;
	if (nlFound && last == mark) last = now;
	NoLockHideFromFight(false);
	if (t && trio == t.raw()) {
		g_fightNow = now;
		g_fightDelay = *(float*)(trio + 0x404);
	}
}

} // namespace

bool MarkerSeen(const float* p) { return SpawnMarkerSeen(p); }

bool ProximityHeroFighting(int hero) {
	if (hero < 0 || hero > 1) return false;
	return g_fightNow - g_fightSeen[hero] <= g_fightDelay;
}

namespace {

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

// Where a hero collects bonuses: its model (the node), as the game's check uses
// for the leader (trio vf27) -- a hero riding a cable car keeps its own position
// (+0x6C) at the boarding spot while the car carries the model on.
Vector3 CollectPos(Hero h) {
	Vector3 at;
	if (h.hook->node && NodePos(h.hook->node, &at)) return at;
	return h.Pos();
}

void AttractToPlayer2(char* pool) {
	Trio t = GetTrio();
	int nl = CoopNonLeaderPlayer();
	Hero h2 = t && nl >= 0 ? t.GetHero(nl) : Hero();
	if (!h2 || !CoopP2Joined() || CoopSuspended() || HeroOut(nl)) return;
	typedef char(__fastcall* BoolFn)(void* self, void* edx);
	typedef void(__fastcall* AttractFn)(void* self, void* edx, void* hero, int zero);
	if (!P2MayCollect(pool, t, h2)) return;
	char* radiusObj = *(char**)(pool + 0x3C);
	if (!radiusObj) return;
	float r2 = *(float*)(radiusObj + 4);
	Vector3 p2 = CollectPos(h2);
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
	if (!h2 || HeroOut(nl) || !P2MayCollect(pool, t, h2)) return;
	Vector3 p = CollectPos(h2);
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

// Placed bonuses (helmets on beacons) per frame: pool +0x2C, 20-25 out of 50
// bonus objects (the rest serve dropped ones). Two views see about twice as many
// beacons: three quarters more (35 of 50) still left LV1-2's far helmets on
// player 1's half to come and go as either camera turned (46 in the two views).
// With the split screen up the pool places as many as it has free, and a drop
// that finds none takes back the beacon's bonus placed last (PoolSpawnHook).
bool PoolSplit() { return CoopP2Joined() && !CoopSuspended() && KlusterSplitUp(); }
struct PoolCap {
	char* pool; unsigned saved;
	explicit PoolCap(void* p) : pool((char*)p), saved(*(unsigned*)((char*)p + 0x2C)) {
		if (PoolSplit()) *(unsigned*)(pool + 0x2C) = 0xFFFF;
	}
	~PoolCap() { *(unsigned*)(pool + 0x2C) = saved; }
};

// Shared vf29 of both pools (0x54C790, thiscall(pool, arg), ret 4): a dropped
// bonus (a beaten Roman's helmets) moves from the free list +0x38 to the active
// list +0x30; with none free, nothing drops. Under PoolCap the beacons may have
// taken them all: the head of +0x34 (placed last, so ranked last) that is not
// flying to a hero (vf19) is freed the way vf27 frees them each frame (vf22).
const size_t    kPoolSpawnSlot = 29;
const uintptr_t kPoolSpawn     = 0x54C790;
typedef void*(__fastcall* SpawnFn)(void* self, void* edx, void* arg);
SpawnFn g_origPoolSpawn = nullptr;

void* __fastcall PoolSpawnHook(void* self, void* edx, void* arg) {
	char* pool = (char*)self;
	if (!*(char**)(pool + 0x38) && PoolSplit()) {
		typedef char(__fastcall* BoolFn)(void* self, void* edx);
		typedef void(__fastcall* VoidFn)(void* self, void* edx);
		char** link = (char**)(pool + 0x34);
		for (char* b = *link; b; link = (char**)(b + 0x14), b = *link) {
			if (((BoolFn)(*(void***)b)[19])(b, nullptr)) continue;
			*link = *(char**)(b + 0x14);
			((VoidFn)(*(void***)b)[22])(b, nullptr);
			*(char**)(b + 0x14) = nullptr;
			*(char**)(pool + 0x38) = b;
			static long n = 0;
			if (++n <= 8 || n % 100 == 0)
				Log("proximity: bonus pool %p (handler %u): no free bonus for a drop, the beacon's %p taken back (%ld)", pool,
				    *(uint8_t*)(pool + 0x28), b, n);
			break;
		}
	}
	return g_origPoolSpawn(self, edx, arg);
}
void LogPoolOnce(void* self) {
	static void* logged[32];
	static int nLogged = 0;
	bool seen = false;
	for (int i = 0; i < nLogged; i++) seen = seen || logged[i] == self;
	if (!seen && nLogged < 32) {
		logged[nLogged++] = self;
		char* p = (char*)self;
		int freeN = 0;
		for (char* b = *(char**)(p + 0x38); b && freeN < 999; b = *(char**)(b + 0x14)) freeN++;
		Log("proximity: bonus pool %p: handler %u, beacons per frame %u, free %d", self, *(uint8_t*)(p + 0x28),
		    *(unsigned*)(p + 0x2C), freeN);
	}
}

void __fastcall PoolUpdateHook(void* self, void* edx, void* dt) {
	LogPoolOnce(self);
	PoolCap cap(self);
	g_origPoolUpdate(self, edx, dt);
	AttractToPlayer2((char*)self);
}

void LogPoolOnce(void* self);
void __fastcall AsterixPoolUpdateHook(void* self, void* edx, void* dt) {
	LogPoolOnce(self);
	PoolCap cap(self);
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

// Does event `ev`'s group switch the trio's leader, order the companion to
// wait (0x4800/0x4801/0x48B7/0x48B8, 0x4802, 0x4849) or send it somewhere?
bool GroupCommandsTrio(unsigned ev) {
	unsigned group = (ev & 0xFFFF) >> 3;
	CKYellowPages* yp = YellowPages();
	char* es = yp ? *(char**)((char*)yp + 0x6C) : nullptr;
	if (!es || group == 0x1FFF) return false;
	unsigned n = (unsigned)*(uint16_t*)(es + 0x1C) + *(uint16_t*)(es + 0x1E) + *(uint16_t*)(es + 0x20);
	if (group >= n) return false;
	uint8_t* tab = *(uint8_t**)(es + 0xC);
	char** objs = *(char***)(es + 0x10);
	uint16_t* msgs = *(uint16_t**)(es + 0x14);
	unsigned first = 0;
	for (unsigned g = 0; g < group; g++) first += tab[g * 3];
	for (unsigned k = 0; k < tab[group * 3]; k++) {
		if (!objs[first + k] || *(uintptr_t*)objs[first + k] != 0x6256A0) continue;
		unsigned m = msgs[first + k];
		if (m == 0x4800 || m == 0x4801 || m == 0x48B7 || m == 0x48B8 || m == 0x4802 || m == 0x4849) return true;
		// Sidekick walk / run to marker n (0x4813.., 0x4845..), teleport the sidekick
		// to marker n (0x48BB..): the boats' boxes (level 4 sector 9, level 5 sector
		// 4) send the companion behind the boat when the leader steps aboard; set off
		// by player 2's Obelix, they marched Obelix himself there.
		if ((m >= 0x4813 && m <= 0x4876) || (m >= 0x48BB && m <= 0x48EC)) return true;
	}
	return false;
}

// Event `ev`'s group: up to `max` targets and the message each gets.
int GroupTargets(unsigned ev, char** objsOut, uint16_t* msgsOut, int max) {
	unsigned group = (ev & 0xFFFF) >> 3;
	CKYellowPages* yp = YellowPages();
	char* es = yp ? *(char**)((char*)yp + 0x6C) : nullptr;
	if (!es || group == 0x1FFF) return 0;
	unsigned n = (unsigned)*(uint16_t*)(es + 0x1C) + *(uint16_t*)(es + 0x1E) + *(uint16_t*)(es + 0x20);
	if (group >= n) return 0;
	uint8_t* tab = *(uint8_t**)(es + 0xC);
	char** objs = *(char***)(es + 0x10);
	uint16_t* msgs = *(uint16_t**)(es + 0x14);
	unsigned first = 0;
	for (unsigned g = 0; g < group; g++) first += tab[g * 3];
	int k = 0;
	for (unsigned i = 0; i < tab[group * 3] && k < max; i++, k++) {
		objsOut[k] = objs[first + i];
		msgsOut[k] = msgs[first + i];
	}
	return k;
}

// Diagnostics: an event group's targets (object, vtable, message), once per event.
void DumpGroup(unsigned ev) {
	static unsigned done[64];
	static int nDone = 0;
	for (int i = 0; i < nDone; i++) if (done[i] == (ev & 0xFFFF)) return;
	if (nDone < 64) done[nDone++] = ev & 0xFFFF;
	unsigned group = (ev & 0xFFFF) >> 3;
	CKYellowPages* yp = YellowPages();
	char* es = yp ? *(char**)((char*)yp + 0x6C) : nullptr;
	if (!es || group == 0x1FFF) return;
	unsigned n = (unsigned)*(uint16_t*)(es + 0x1C) + *(uint16_t*)(es + 0x1E) + *(uint16_t*)(es + 0x20);
	if (group >= n) return;
	uint8_t* tab = *(uint8_t**)(es + 0xC);
	char** objs = *(char***)(es + 0x10);
	uint16_t* msgs = *(uint16_t**)(es + 0x14);
	unsigned first = 0;
	for (unsigned g = 0; g < group; g++) first += tab[g * 3];
	char buf[1024];
	LineBuf line(buf);
	line.Add("proximity: event %04X's group %03X:", ev & 0xFFFF, group);
	for (unsigned k = 0; k < tab[group * 3]; k++) {
		char* o = objs[first + k];
		line.Add(" %p(vt %06X)<-%04X", (void*)o, o ? (unsigned)*(uintptr_t*)o : 0, msgs[first + k]);
	}
	Log("%s", line.c_str());
}

bool GroupPlaysCutscene(unsigned ev);

// Are event `ev`'s orders to the trio only "lead" switches to hero `up` (0x4800 /
// 0x48B7 Asterix, 0x4801 / 0x48B8 Obelix) and "the sidekick may move again"
// (0x4803), with no cutscene? LV5-10's cross rectangle by the keg (525.7 -451.4
// -197.2, #97: 4800 + 4803) -- with Obelix down it put Asterix back for nothing:
// he leads anyway, Obelix waits for nothing. A switch to the fallen hero still
// needs both.
bool GroupOnlyLeadsTo(unsigned ev, int up) {
	char* objs[32];
	uint16_t msgs[32];
	int n = GroupTargets(ev, objs, msgs, 32), k;
	if (n == 32 || GroupPlaysCutscene(ev)) return false;
	for (k = 0; k < n; k++) {
		if (!objs[k] || *(uintptr_t*)objs[k] != 0x6256A0) continue;
		unsigned m = msgs[k];
		if (m == 0x4803) continue;
		int to = m == 0x4800 || m == 0x48B7 ? HERO_ASTERIX : m == 0x4801 || m == 0x48B8 ? HERO_OBELIX : -1;
		if (to < 0 || to != up) return false;
	}
	return true;
}

// Does event `ev`'s group only switch the lead (sp_hero_switch_lock, coop.cpp):
// to the trio nothing but switches (0x4800 / 0x48B7 Asterix, 0x4801 / 0x48B8
// Obelix), "the sidekick may move" (0x4803), the fight mode's timer (0x2000),
// Idefix's orders, Asterix's torch used, a catch-up hop? Its switches into `out` (their count; 0 for any other group).
// Whatever else it does outside the trio does not count -- a cinematic bloc
// included (LV3-5's 0850: 0501 to a CKManageEventCinematicBloc, a music stream;
// kept, the lead came back 0.3 s later): a later order for "the sidekick" the
// level meant for the other hero gets its switch then (coop.cpp SoloOrderNeedsLead).
int GroupOnlySwitches(unsigned ev, uint16_t* out, int max) {
	char* objs[64];
	uint16_t msgs[64];
	int n = GroupTargets(ev, objs, msgs, 64), k = 0;
	if (n == 64) return 0;
	for (int i = 0; i < n; i++) {
		if (!objs[i] || *(uintptr_t*)objs[i] != 0x6256A0) continue;
		unsigned m = msgs[i];
		if (m == 0x4803 || m == 0x2000) continue;
		// Idefix's wait / follow and his teleports to markers (LV2-1's keg: 4801 +
		// 4803 + 4878 -- the switch was kept, given back 0.3 s later: user)
		if (m == 0x4877 || m == 0x4878 || (m >= 0x4885 && m <= 0x48B6)) continue;
		// Asterix's torch used up (0x4811: +0x3EC 0x400000, whoever leads) and the
		// sidekick's / Idefix's catch-up to the leader (0x487D / 0x487E: +0x884
		// 0x400, a hop to a spot by the leader): nobody held (LV4-3's light pillar,
		// group 0E7: 4800 + 4803 + 487D + 4811 -- kept, Asterix for 0.3 s: user)
		if (m == 0x4811 || m == 0x487D || m == 0x487E) continue;
		if (m != 0x4800 && m != 0x4801 && m != 0x48B7 && m != 0x48B8) return 0;
		if (k < max) out[k++] = (uint16_t)m;
	}
	return k;
}

// The event service's dispatch (CKSrvEvent::vf17, 0x46B180, thiscall(this,
// arg), ret 4): each flagged group's messages in turn -- of the first set of
// groups (+0x1C of them) any with a flag up (table +2), of the next two (+0x1E,
// +0x20) those with all their flags up (+2 == the mask +1). Groups are flagged
// by inlined code in many places (an activator's lever, ...), not only through
// FUN_00460C20 (FlagEventHook): the groups about to run are noted here, for
// sp_hero_switch_lock (a level's switch from a group that only switches is
// dropped, coop.cpp).
typedef void(__fastcall* EventDispatchFn)(char* es, void* edx, void* arg);
const uintptr_t kSrvEventVtbl = 0x624FF0, kEventDispatch = 0x46B180;
EventDispatchFn g_origEventDispatch = nullptr;
uint16_t g_dispatching[32];   // the groups being dispatched, as their first event ids
int      g_nDispatching = 0;

bool g_heldAtDispatch[2];   // each hero held by the level (AI order not follow) as the dispatch began

void __fastcall EventDispatchHook(char* es, void* edx, void* arg) {
	g_nDispatching = 0;
	if (*(uint8_t*)(es + 0x22) && !CoopP2Joined() && g_config.spSwitchLock) {
		// (A group may free the hero before it hands it the lead: LV2-6's 0D29,
		// 4803 then 48B7 -- the switch then found Asterix following.)
		Trio t = GetTrio();
		for (int h = 0; h < 2; h++) {
			Hero hh = t ? t.GetHero(h) : Hero();
			g_heldAtDispatch[h] = hh && hh.Order() != AI_FOLLOW_LEADER;
		}
		uint8_t* tab = *(uint8_t**)(es + 0xC);
		unsigned n1 = *(uint16_t*)(es + 0x1C), n = n1 + *(uint16_t*)(es + 0x1E) + *(uint16_t*)(es + 0x20);
		for (unsigned g = 0; tab && g < n && g < 0x1FFF && g_nDispatching < 32; g++) {
			uint8_t f = tab[g * 3 + 2];
			if (f && (g < n1 || f == tab[g * 3 + 1])) g_dispatching[g_nDispatching++] = (uint16_t)(g << 3);
		}
	}
	g_origEventDispatch(es, edx, arg);
	g_nDispatching = 0;
}

// The groups under way that send the trio `msg`: all of them only switch (and
// free the sidekick)? The first such group's event id in `ev`.
bool DispatchOnlySwitches(unsigned msg, unsigned* ev) {
	bool found = false;
	for (int i = 0; i < g_nDispatching; i++) {
		char* objs[64];
		uint16_t msgs[64];
		int n = GroupTargets(g_dispatching[i], objs, msgs, 64);
		bool sends = false;
		for (int k = 0; k < n && !sends; k++) sends = objs[k] && *(uintptr_t*)objs[k] == 0x6256A0 && msgs[k] == msg;
		if (!sends) continue;
		uint16_t sw[4];
		if (GroupOnlySwitches(g_dispatching[i], sw, 4) <= 0) return false;   // (a group holding a hero sends it: kept)
		if (!found) *ev = g_dispatching[i];
		found = true;
	}
	return found;
}

// Does a group under way that sends the trio `msg` (a lead switch) also hold the
// hero losing the lead -- "the sidekick" waits (0x4802), walks or runs to a marker
// (0x4813..0x4876), is put on one (0x48BB..0x48EC)? The level's hand-over: the
// hero at the lever is parked, the other plays on (LV4-7's lever #138: 4800 +
// 4802; LV2-6's #406: 48B8 + 481B).
bool DispatchHoldsLoser(unsigned msg) {
	for (int i = 0; i < g_nDispatching; i++) {
		char* objs[64];
		uint16_t msgs[64];
		int n = GroupTargets(g_dispatching[i], objs, msgs, 64);
		bool sends = false, holds = false;
		for (int k = 0; k < n; k++) {
			if (!objs[k] || *(uintptr_t*)objs[k] != 0x6256A0) continue;
			unsigned m = msgs[k];
			sends |= m == msg;
			holds |= m == 0x4802 || (m >= 0x4813 && m <= 0x4876) || (m >= 0x48BB && m <= 0x48EC);
		}
		if (sends && holds) return true;
	}
	return false;
}

// Does event `ev`'s group need both heroes: command the trio (above; not only
// a switch of the lead to `up`, the standing hero), or start a ride / scripted
// mode for both (trio 0x4806, 0x4807, 0x480B..0x480F: the belly slide's cross
// detector, LV2-2 z -903.8, sends 0x480B), or a Roman agent's talk (`talks`)?
bool GroupNeedsBoth(unsigned ev, int up, bool talks = true) {
	if (GroupCommandsTrio(ev) && !GroupOnlyLeadsTo(ev, up)) return true;
	unsigned group = (ev & 0xFFFF) >> 3;
	CKYellowPages* yp = YellowPages();
	char* es = yp ? *(char**)((char*)yp + 0x6C) : nullptr;
	if (!es || group == 0x1FFF) return false;
	unsigned n = (unsigned)*(uint16_t*)(es + 0x1C) + *(uint16_t*)(es + 0x1E) + *(uint16_t*)(es + 0x20);
	if (group >= n) return false;
	uint8_t* tab = *(uint8_t**)(es + 0xC);
	char** objs = *(char***)(es + 0x10);
	uint16_t* msgs = *(uint16_t**)(es + 0x14);
	unsigned first = 0;
	for (unsigned g = 0; g < group; g++) first += tab[g * 3];
	for (unsigned k = 0; k < tab[group * 3]; k++) {
		uintptr_t vt = objs[first + k] ? *(uintptr_t*)objs[first + k] : 0;
		if (vt == 0x6259E4) return true;   // CKAsterixSlideFP: the belly slide started (0x480B there)
		unsigned m = msgs[first + k];
		// CKHkClueMan 0x5198: a Roman agent's talk set off by its zone. Let through
		// with a hero down, the spy took it as its one start (its request to the
		// trio dropped) and the zone never fired again.
		if (vt == 0x6261F4 && m == 0x5198 && talks) return true;
		if (vt != 0x6256A0) continue;
		if (m == 0x4806 || m == 0x4807 || (m >= 0x480B && m <= 0x480F)) return true;
	}
	return false;
}

// Does event `ev`'s group run a cutscene (a cinematic bloc, 8,9..8,28 /
// CKSkyCinematicBloc, or the cinematic service among its targets)?
bool GroupPlaysCutscene(unsigned ev) {
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
	for (unsigned k = 0; k < tab[group * 3]; k++) {
		uintptr_t vt = objs[first + k] ? *(uintptr_t*)objs[first + k] : 0;
		if ((vt >= 0x624A7C && vt <= 0x624F48) || vt == 0x629B78 || vt == 0x624F90) return true;
	}
	return false;
}

// Does a hero zone whose group commands the trio mean the hero it is bound to?
// Orders for the companion -- wait here (0x4802, 0x4849: level 4 sector 5's
// blue circle), walk / run / teleport to a marker (the boats) -- do: set off by
// the other hero they order that hero itself. A plain leader switch does not,
// nor does a zone that starts a cutscene (level 2 sector 10's boss: #34 "switch
// to Asterix", then #4 "Obelix walks to marker 22" and the boss's cutscene,
// both bound to Asterix -- in single player the AI Asterix crosses them with
// Obelix; in co-op player 2's Obelix could never start the boss). Nor does a zone
// that lights a light pillar (0x900 to CKHkLightPillar) -- a wait's sequence
// started or started over, the waiting hero's: LV2-8's 03B8 (Asterix off the
// platforms: Asterix leads, the blocks back, the pillar lit) was set off by
// Obelix jumping on the turnstile, and took the lead from him while Asterix
// waited at the rotary beam.
bool ZoneMeansItsHero(unsigned ev) {
	if (!GroupCommandsTrio(ev) || GroupPlaysCutscene(ev)) return false;
	unsigned group = (ev & 0xFFFF) >> 3;
	CKYellowPages* yp = YellowPages();
	char* es = *(char**)((char*)yp + 0x6C);
	uint8_t* tab = *(uint8_t**)(es + 0xC);
	char** objs = *(char***)(es + 0x10);
	uint16_t* msgs = *(uint16_t**)(es + 0x14);
	unsigned first = 0;
	for (unsigned g = 0; g < group; g++) first += tab[g * 3];
	for (unsigned k = 0; k < tab[group * 3]; k++)
		if (objs[first + k] && *(uintptr_t*)objs[first + k] == 0x629030 && msgs[first + k] == 0x900) return true;
	for (unsigned k = 0; k < tab[group * 3]; k++) {
		if (!objs[first + k] || *(uintptr_t*)objs[first + k] != 0x6256A0) continue;
		unsigned m = msgs[first + k];
		if (m == 0x4800 || m == 0x4801 || m == 0x48B7 || m == 0x48B8) continue;
		if (m == 0x4802 || m == 0x4849 || (m >= 0x4813 && m <= 0x4876) || (m >= 0x48BB && m <= 0x48EC)) return true;
	}
	return false;
}

// Boss runs. Level 2 sector 10's rectangle #4 (group #20) and level 3 sector
// 6's #46 (group #187) wake the boss, disable the Tornado and Mole combos
// (trio 0x48EE, 0x48F1) and walk the sidekick to a marker; on level 3 the
// sidekick's own rectangles along the way (#52, #53, enabled by the group) walk
// it on to the next marker. In single player the AI Obelix runs ahead of the
// player; in co-op that took player 2's Obelix away until the end of the path.
// The run is the starting group and, following the zones it enables, every
// group that walks the sidekick on; their marker walks are dropped in co-op.
struct BossRun {
	int      level = -1;
	unsigned groups[16] = {};
	int      ng = 0;
	uint16_t walks[16] = {};
	int      nw = 0;
	uint16_t tests[32] = {};  // the detector tests its groups enable: list (0..4: box-enter .. sphere-level) << 8 | index
	int      nt = 0;
	DWORD    flaggedAt = 0;   // a run group's zone fired (its events come later in the frame)
} g_bossRun;

static bool IsMarkerWalk(unsigned m) { return m >= 0x4813 && m <= 0x4876; }

struct EventTable {
	char* es = nullptr;
	unsigned n = 0;
	uint8_t* tab = nullptr;
	char** objs = nullptr;
	uint16_t* msgs = nullptr;
	bool Load() {
		CKYellowPages* yp = YellowPages();
		es = yp ? *(char**)((char*)yp + 0x6C) : nullptr;
		if (!es) return false;
		n = (unsigned)*(uint16_t*)(es + 0x1C) + *(uint16_t*)(es + 0x1E) + *(uint16_t*)(es + 0x20);
		tab = *(uint8_t**)(es + 0xC);
		objs = *(char***)(es + 0x10);
		msgs = *(uint16_t**)(es + 0x14);
		return tab && objs && msgs;
	}
	unsigned First(unsigned g) const {
		unsigned first = 0;
		for (unsigned k = 0; k < g; k++) first += tab[k * 3];
		return first;
	}
	bool IsTrio(unsigned k) const { return objs[k] && *(uintptr_t*)objs[k] == 0x6256A0; }
};

static bool GroupStartsBossRun(const EventTable& et, unsigned g) {
	if (g >= et.n) return false;
	bool combos = false, walk = false;
	for (unsigned k = et.First(g), e = k + et.tab[g * 3]; k < e; k++) {
		if (!et.IsTrio(k)) continue;
		if (et.msgs[k] == 0x48EE || et.msgs[k] == 0x48F1) combos = true;
		if (IsMarkerWalk(et.msgs[k])) walk = true;
	}
	return combos && walk;
}

static void AddBossRunGroup(const EventTable& et, char* det, unsigned g, int depth) {
	BossRun& r = g_bossRun;
	for (int i = 0; i < r.ng; i++) if (r.groups[i] == g) return;
	if (r.ng >= 16 || g >= et.n) return;
	bool walk = false;
	for (unsigned k = et.First(g), e = k + et.tab[g * 3]; k < e; k++)
		if (et.IsTrio(k) && IsMarkerWalk(et.msgs[k])) walk = true;
	if (!walk) return;
	r.groups[r.ng++] = g;
	for (unsigned k = et.First(g), e = k + et.tab[g * 3]; k < e; k++) {
		if (et.IsTrio(k) && IsMarkerWalk(et.msgs[k]) && r.nw < 16) r.walks[r.nw++] = et.msgs[k];
		// Detection service 0x14..0x18 + index: enable a test (box-enter, sphere-enter,
		// plane-cross, box-level, sphere-level).
		if (depth >= 8 || et.objs[k] != det) continue;
		unsigned op = et.msgs[k] >> 8, idx = et.msgs[k] & 0xFF;
		if (op < 0x14 || op > 0x18) continue;
		uint16_t test = (uint16_t)((op - 0x14) << 8 | idx);
		bool knownTest = false;
		for (int i = 0; i < r.nt; i++) if (r.tests[i] == test) knownTest = true;
		if (!knownTest && r.nt < 32) r.tests[r.nt++] = test;
		static const int bases[5] = { 0x2C, 0x34, 0x3C, 0x44, 0x4C };
		char* tests = *(char**)(det + bases[op - 0x14]);
		unsigned cnt = *(uint16_t*)(det + bases[op - 0x14] + 4);
		if (tests && idx < cnt) AddBossRunGroup(et, det, *(uint16_t*)(tests + idx * 8 + 6) >> 3, depth + 1);
	}
}

static int CurrentLevel() {
	CKYellowPages* yp = YellowPages();
	return (yp && yp->gameManager) ? *(int*)((char*)yp->gameManager + ax::kGM_currentLevel) : -1;
}

// A zone event was flagged by the detection service (`det`) for hero `zh`.
static void NoteBossRunZone(char* det, unsigned ev, int zh) {
	if (!CoopP2Joined() && CoopSoloLevelLeaderAway() < 0) return;   // (single player: DispatchInBossRun)
	EventTable et;
	if (!et.Load()) return;
	unsigned g = (ev & 0xFFFF) >> 3;
	BossRun& r = g_bossRun;
	if (r.level != CurrentLevel()) { r = BossRun(); r.level = CurrentLevel(); }
	if (GroupStartsBossRun(et, g)) {
		bool known = false;
		for (int i = 0; i < r.ng; i++) if (r.groups[i] == g) known = true;
		if (!known) {
			r.ng = r.nw = r.nt = 0;
			AddBossRunGroup(et, det, g, 0);
			char lb[512];
			LineBuf l(lb);
			l.Add("proximity: boss run from group %03X: groups", g);
			for (int i = 0; i < r.ng; i++) l.Add(" %03X", r.groups[i]);
			l.Add(", sidekick walks");
			for (int i = 0; i < r.nw; i++) l.Add(" %04X", r.walks[i]);
			Log("%s", lb);
		}
		if (zh >= 0) CoopNoteBossRun(zh);
	}
	for (int i = 0; i < r.ng; i++)
		if (r.groups[i] == g) r.flaggedAt = GetTickCount();
}

// Is the group being dispatched that sends `msg` to the trio a boss run's (its
// start, or one it enables that walks the sidekick on)?
bool DispatchInBossRun(unsigned msg) {
	EventTable et;
	if (!et.Load()) return false;
	const BossRun& r = g_bossRun;
	for (int i = 0; i < g_nDispatching; i++) {
		unsigned g = g_dispatching[i] >> 3;
		if (g >= et.n) continue;
		bool sends = false;
		for (unsigned k = et.First(g), e = k + et.tab[g * 3]; k < e && !sends; k++) sends = et.IsTrio(k) && et.msgs[k] == msg;
		if (!sends) continue;
		if (GroupStartsBossRun(et, g)) return true;
		for (int j = 0; r.level == CurrentLevel() && j < r.ng; j++)
			if (r.groups[j] == g) return true;
	}
	return false;
}

// Single player, sp_hero_switch_lock: the hero bound to the zones whose group,
// being dispatched, sends the trio `msg` (a level's order for the sidekick: a
// zone bound to one hero orders the other -- SoloOrderNeedsLead). LV4-7's plane
// #17, Obelix's (event 0448: 4818 "the sidekick walks to marker 5", Asterix to
// the automatic car's foot). The hero, or -1 (no zone's group, or zones of both).
// `withSwitch`: such a group also switches the leader (4800 / 4801 / 48B7 / 48B8).
int NodeIndex(char* det, Hero h);

int DispatchZoneHero(unsigned msg, bool* withSwitch) {
	if (withSwitch) *withSwitch = false;
	EventTable et;
	Trio t = GetTrio();
	CKYellowPages* yp = YellowPages();
	char* det = yp ? *(char**)((char*)yp + 0x54) : nullptr;
	if (!t || !det || *(uintptr_t*)det != 0x62504C || !et.Load()) return -1;
	int ia = NodeIndex(det, t.GetHero(HERO_ASTERIX)), io = NodeIndex(det, t.GetHero(HERO_OBELIX));
	static const int bases[5] = { 0x2C, 0x34, 0x3C, 0x44, 0x4C };
	int means = -1;
	for (int i = 0; i < g_nDispatching; i++) {
		unsigned g = g_dispatching[i] >> 3;
		if (g >= et.n) continue;
		bool sends = false;
		for (unsigned k = et.First(g), e = k + et.tab[g * 3]; k < e && !sends; k++) sends = et.IsTrio(k) && et.msgs[k] == msg;
		if (!sends) continue;
		for (unsigned k = et.First(g), e = k + et.tab[g * 3]; withSwitch && k < e; k++)
			if (et.IsTrio(k) && (et.msgs[k] == 0x4800 || et.msgs[k] == 0x4801 || et.msgs[k] == 0x48B7 || et.msgs[k] == 0x48B8))
				*withSwitch = true;
		for (int li = 0; li < 5; li++) {
			char* tl = *(char**)(det + bases[li]);
			for (unsigned j = 0, c = *(uint16_t*)(det + bases[li] + 4); tl && j < c; j++) {
				if ((unsigned)(*(uint16_t*)(tl + j * 8 + 6) >> 3) != g) continue;
				int obj = *(uint16_t*)(tl + j * 8 + 2);
				int h = obj == ia ? HERO_ASTERIX : obj == io ? HERO_OBELIX : -1;
				if (h < 0) continue;
				if (means >= 0 && means != h) return -1;
				means = h;
			}
		}
	}
	return means;
}

bool BossRunDropsWalk(int ev) {
	const BossRun& r = g_bossRun;
	if (!CoopP2Joined() || !r.flaggedAt || GetTickCount() - r.flaggedAt > 500 || r.level != CurrentLevel()) return false;
	for (int i = 0; i < r.nw; i++)
		if (r.walks[i] == (uint16_t)ev) return true;
	return false;
}

void ForgetMove(void* heroNode, const float at[3]) {
	CKYellowPages* yp = YellowPages();
	char* det = yp ? *(char**)((char*)yp + 0x54) : nullptr;
	if (!det || *(uintptr_t*)det != 0x62504C || !heroNode) return;
	char** nodes = *(char***)(det + 0x18);
	float* prev = *(float**)(det + 0x1C);
	unsigned n = *(uint16_t*)(det + 0x20);
	for (unsigned i = 0; nodes && prev && i < n; i++)
		if (nodes[i] == (char*)heroNode) memcpy(prev + 3 * i, at, 12);
}

bool LeverHeldBack(char* receiver, Trio t) {
	if (!g_config.safeSequence || *(uintptr_t*)receiver != 0x628DA0) return false;
	int lead = t.Leader();
	unsigned evs[2] = { *(uint16_t*)(receiver + 0x28), *(uint16_t*)(receiver + 0x2A) };
	for (unsigned ev : evs)
		if (GroupSwitchTarget(ev) == lead) return true;
	return false;
}

// Single player with the lead changing hands: the safe sequence for the leader
// -- a lever whose event gives the lead to the hero leading (the level has the
// other hero pull it and hand over) takes no hits while the other hero could
// lead instead (following, standing: switch and pull it with that one). With
// the other held by the level (a wait) or down, the lever is the game's.
// And a lever whose event gives the lead to the companion while that one is free
// (following you, not held by the level): the level has it pulled only while that
// hero waits where the level put it -- the lever frees it (LV2-5: lever #419
// behind the iron crates, 48B7 + 4803, for Asterix held by the hearth's 0D60
// walk). Pulled early as Obelix (your switch), the push-once lever was spent and
// the hearth's wait held Asterix for good. Only such a freeing lever (its group's
// trio orders: switches and 4803, nobody held): one that hands the lead over and
// parks the hero at it (LV2-6's #406: 48B8 + 481B; LV4-7's #138: 4800 + 4802) is
// the game's whoever follows.
static bool GroupSendsTrio(unsigned ev, unsigned msg) {
	char* objs[64];
	uint16_t msgs[64];
	int n = GroupTargets(ev, objs, msgs, 64);
	for (int k = 0; k < n; k++)
		if (objs[k] && *(uintptr_t*)objs[k] == 0x6256A0 && msgs[k] == msg) return true;
	return false;
}

static bool SoloLeverEarly(char* receiver, Trio t) {
	if (!g_config.safeSequence || *(uintptr_t*)receiver != 0x628DA0) return false;
	int l = t.Leader();
	unsigned evs[2] = { *(uint16_t*)(receiver + 0x28), *(uint16_t*)(receiver + 0x2A) };
	for (unsigned ev : evs) {
		int to = GroupSwitchTarget(ev);
		if (to < 0 || to == l || (to != HERO_ASTERIX && to != HERO_OBELIX)) continue;
		uint16_t sw[4];
		if (GroupOnlySwitches(ev, sw, 4) <= 0 || !GroupSendsTrio(ev, 0x4803)) continue;
		Hero h = t.GetHero(to);
		if (!h || IndivFallen(to) || h.Order() != AI_FOLLOW_LEADER) continue;   // (held by the level: the lever's)
		if (g_heldBackLogged != receiver) {
			g_heldBackLogged = receiver;
			Log("proximity: safe sequence: lever %p ignores %s -- it gives the lead to %s, who is not held by the level (its lever "
			    "for later)", receiver, HeroName(l), HeroName(to));
		}
		return true;
	}
	return false;
}

static bool SoloLeverHeldBack(char* receiver, Trio t) {
	if (!SoloLeadFree()) return false;
	if (SoloLeverEarly(receiver, t)) return true;
	if (!LeverHeldBack(receiver, t)) return false;
	int l = t.Leader();
	int c = l == HERO_ASTERIX ? HERO_OBELIX : l == HERO_OBELIX ? HERO_ASTERIX : -1;
	Hero h = c >= 0 ? t.GetHero(c) : Hero();
	if (!h || IndivFallen(c) || h.Order() != AI_FOLLOW_LEADER || ((h.Flags18() >> 2) & 7) != 0) return false;   // (mode 0)
	if (g_heldBackLogged != receiver) {
		g_heldBackLogged = receiver;
		Log("proximity: safe sequence: lever %p ignores %s -- it switches to %s, who leads already (%s's to pull)", receiver,
		    HeroName(l), HeroName(l), HeroName(c));
	}
	return true;
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
	if (!t || !CoopP2Joined() || CoopSuspended()) {
		if (t && SoloLeverHeldBack(receiver, t)) return 0;
		return send(receiver, nullptr, event, list);
	}
	int p1 = CoopP1Hero(), p2 = CoopP2Hero();
	Hero h1 = t.GetHero(p1), h2 = t.GetHero(p2);
	if (!h1 || !h2 || HeroOut(p1) || HeroOut(p2)) return send(receiver, nullptr, event, list);
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

// Single player, sp_hero_switch_lock: you play the hero the level means as the
// sidekick (its switch to the other dropped; CoopSoloLevelLeaderAway: the AI
// companion `away` is the level's leader). A zone bound to the level's leader is
// the player's in the game -- unless it orders that hero itself
// (ZoneMeansItsHero) or means the pair away ("while outside"): it answers the
// hero you play, tested with its node and last place, and no longer the AI
// companion (LV2-10's boss gate: #34 the intro, #4 the boss, both bound to
// Asterix -- Obelix walked through both, the AI Asterix behind him set them off).
bool g_inSoloPass = false;

// ZoneMeansItsHero for the solo pass: an order for "the sidekick" from the level
// leader's zone means that hero itself only with the group's switch away from it
// (a circle: "Obelix leads" + "the sidekick waits") or a light pillar lit. Without
// one the order is for the other hero, as when the leader crosses it (LV2-6's
// #144, 0CA8: 48CF "teleport the sidekick" + 4803 on the jump down into the fight
// -- Asterix's; crossed by the Obelix you play it was skipped: Asterix never put
// at the marker).
static bool SoloZoneMeansItsHero(unsigned ev, int bound) {
	if (!ZoneMeansItsHero(ev)) return false;
	if (GroupSwitchTarget(ev) == (bound ^ 1)) return true;
	char* objs[64];
	uint16_t msgs[64];
	int n = GroupTargets(ev, objs, msgs, 64);
	for (int k = 0; k < n; k++)
		if (objs[k] && *(uintptr_t*)objs[k] == 0x629030 && msgs[k] == 0x900) return true;
	return false;
}

static bool SoloZonesTick(char* det, void* edx, int arg, Trio t, int away) {
	int iw = NodeIndex(det, t.GetHero(away)), iy = NodeIndex(det, t.GetHero(away ^ 1));
	if (iw < 0 || iy < 0) return false;
	struct List { int base, count; bool level; } lists[5] = {
		{ 0x2C, 0x30, false }, { 0x34, 0x38, false }, { 0x3C, 0x40, false },
		{ 0x44, 0x48, true }, { 0x4C, 0x50, true } };
	size_t total = 0;
	for (const List& l : lists) total += *(char**)(det + l.base) ? *(uint16_t*)(det + l.count) : 0;
	static uint8_t saved[4096], moved[4096];
	if (total > sizeof(saved)) return false;
	// A boss run's own zones (the tests its groups enable) bound to the hero you
	// play -- the level's sidekick -- answer the companion too: it is the one
	// walked along the run (LV3-6: #143, the boss rolls once the walked sidekick
	// passes it -- Obelix's far jump off the platform crossed it nowhere).
	const BossRun& r = g_bossRun;
	bool run = r.nt > 0 && r.level == CurrentLevel();
	size_t k = 0;
	bool any = false;
	int li = 0;
	for (const List& l : lists) {   // the first pass without the moved tests
		char* e = *(char**)(det + l.base);
		unsigned n = *(uint16_t*)(det + l.count);
		for (unsigned i = 0; e && i < n; i++, e += 8, k++) {
			saved[k] = (uint8_t)e[4];
			unsigned obj = *(uint16_t*)(e + 2);
			bool on = (e[4] & 1) && !(l.level && !(e[5] & 1));
			moved[k] = on && obj == (unsigned)iw && !SoloZoneMeansItsHero(*(uint16_t*)(e + 6), away) ? 1 : 0;
			if (moved[k]) { any = true; e[4] &= ~1; }
			for (int j = 0; run && on && obj == (unsigned)iy && j < r.nt; j++)
				if (r.tests[j] == (uint16_t)(li << 8 | i)) { moved[k] = 2; any = true; }
		}
		li++;
	}
	float* prev = *(float**)(det + 0x1C);
	float prevW[3], prevY[3];
	memcpy(prevW, prev + 3 * iw, 12);
	memcpy(prevY, prev + 3 * iy, 12);
	g_origDetectTick(det, edx, arg);
	if (any) {   // the second: only those, with the hero you play in the companion's place
		k = 0;
		for (const List& l : lists) {
			char* e = *(char**)(det + l.base);
			unsigned n = *(uint16_t*)(det + l.count);
			for (unsigned i = 0; e && i < n; i++, e += 8, k++) e[4] = moved[k] ? (char)saved[k] : (char)(saved[k] & ~1);
		}
		char** nodes = *(char***)(det + 0x18);
		float curW[3], curY[3];
		memcpy(curW, prev + 3 * iw, 12);
		memcpy(curY, prev + 3 * iy, 12);
		std::swap(nodes[iw], nodes[iy]);
		memcpy(prev + 3 * iw, prevY, 12);
		memcpy(prev + 3 * iy, prevW, 12);
		g_inSoloPass = true;
		g_origDetectTick(det, edx, arg);
		g_inSoloPass = false;
		std::swap(nodes[iw], nodes[iy]);
		memcpy(prev + 3 * iw, curW, 12);
		memcpy(prev + 3 * iy, curY, 12);
	}
	k = 0;
	for (const List& l : lists) {
		char* e = *(char**)(det + l.base);
		unsigned n = *(uint16_t*)(det + l.count);
		for (unsigned i = 0; e && i < n; i++, e += 8, k++) e[4] = (char)saved[k];
	}
	static int s_told = 0, s_last = -1;
	if (away != s_last && s_told < 20) {
		s_told++;
		Log("proximity: sp_hero_switch_lock: zones for %s (the level's leader) answer %s, the hero you play", HeroName(away),
		    HeroName(away ^ 1));
	}
	s_last = away;
	return true;
}

void __fastcall DetectTickHook(char* det, void* edx, int arg) {
	Trio t = GetTrio();
	// (A fallen hero -- individual heroes -- counts here: zones bound to it are
	// still tried with the standing hero, so what needs both heroes is caught.)
	int fallen = IndivFallenHero();
	bool coop = t && (CoopP2Joined() || fallen >= 0) && !CoopSuspended() && *(uint8_t*)(det + 0x2A) &&
	            (!HeroOut(HERO_ASTERIX) || fallen == HERO_ASTERIX) && (!HeroOut(HERO_OBELIX) || fallen == HERO_OBELIX);
	int ia = coop ? NodeIndex(det, t.GetHero(HERO_ASTERIX)) : -1;
	int io = coop ? NodeIndex(det, t.GetHero(HERO_OBELIX)) : -1;
	g_tickingDet = det;
	int away = !coop && t && *(uint8_t*)(det + 0x2A) ? CoopSoloLevelLeaderAway() : -1;
	if (away >= 0 && SoloZonesTick(det, edx, arg, t, away)) { g_tickingDet = nullptr; return; }
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
			// Zones that order the companion ("Obelix waits there" -- level 4
			// sector 5's blue circle, bound to Asterix) mean the hero they are
			// bound to: set off by the other one they softlocked (ZoneMeansItsHero).
			static const bool zoneAll = getenv("XXL_ZONEALL") != nullptr;   // diagnostics: the unfiltered swapped pass
			if (keep && !zoneAll && ZoneMeansItsHero(*(uint16_t*)(e + 6))) keep = false;
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
		// Individual heroes: a zone that orders the companion (a boss gate, the
		// slide, a boat...) stays shut while a hero is down -- not set off, the
		// hero who walked in put back. A Roman agent's talk the same, without the
		// put-back: the hero walks on, the talk not started.
		int up = IndivFallenHero() == HERO_ASTERIX ? HERO_OBELIX : HERO_ASTERIX;
		if (IndivFallenHero() >= 0 && GroupNeedsBoth(eventId, up)) {
			int zh = ZoneEventHero(eventId & 0xFFFF);
			if ((CoopP2Joined() || IndivActive()) && !CoopSuspended()) {
				// (zh: the hero who set it off -- in the swapped pass too)
				if (zh != IndivFallenHero() && !GroupNeedsBoth(eventId, up, false))
					IndivBlockTalk("a Roman agent's talk");
				else if (zh != IndivFallenHero()) {
					// back to the side it came from: its place the frame before (the service's)
					Trio tz = GetTrio();
					Hero hz = tz && zh >= 0 ? tz.GetHero(zh) : Hero();
					int ni = hz && g_tickingDet ? NodeIndex(g_tickingDet, hz) : -1;
					float* prevs = g_tickingDet ? *(float**)(g_tickingDet + 0x1C) : nullptr;
					if (ni >= 0 && prevs && std::isfinite(prevs[3 * ni]))
						IndivBlockPartnerSpotFrom("a zone that needs both heroes", prevs + 3 * ni);
					else {
						Log("proximity: no previous place for %s in this zone's pass (node %d, det %p) -- the trail", zh >= 0 ? HeroName(zh) : "?", ni, (void*)g_tickingDet);
						IndivBlockPartnerSpot("a zone that needs both heroes");
					}
				}
				static unsigned s_last = 0;
				if (s_last != (eventId & 0xFFFF)) {
					s_last = eventId & 0xFFFF;
					Log("proximity: zone event %04X held back -- %s is down", eventId & 0xFFFF, HeroName(IndivFallenHero()));
				}
				return;
			}
		}
		// Only "lead" switches to the standing hero (and "the sidekick may move"):
		// let through, the level's choice of leader kept for after the revive.
		if (IndivFallenHero() >= 0 && !CoopSuspended() && GroupCommandsTrio(eventId)) {
			char* objs[32];
			uint16_t msgs[32];
			for (int k = 0, n = GroupTargets(eventId, objs, msgs, 32); k < n; k++)
				if (objs[k] && *(uintptr_t*)objs[k] == 0x6256A0 && msgs[k] != 0x4803) { IndivNoteLevelLead(up); break; }
			static int s_told = 0;
			if (s_told++ < 20)
				Log("proximity: zone event %04X let through with %s down -- only gives %s the lead / frees the sidekick",
				    eventId & 0xFFFF, HeroName(IndivFallenHero()), HeroName(up));
		}
		if (g_inSwappedPass) InterlockedIncrement(&g_swappedZones);
		int zh = ZoneEventHero(eventId & 0xFFFF);
		if (zh >= 0) CoopNoteZoneHero(zh);
		if (zh >= 0 && g_inSwappedPass && GroupCommandsTrio(eventId) && GroupPlaysCutscene(eventId)) CoopNoteStoryZone(zh);
		NoteBossRunZone(g_tickingDet, eventId, zh);
		if (g_inSoloPass)
			Log("proximity: zone event %04X (single player, the heroes' roles swapped: set off by %s)", eventId & 0xFFFF,
			    zh >= 0 ? HeroName(zh) : "?");
		else
			Log("proximity: zone event %04X%s", eventId & 0xFFFF, g_inSwappedPass ? " (the other hero's zone, for co-op)" : "");
	}
	// (a blue circle's, a spinning platform's...: the level as it was, should
	// the lock it starts need undoing)
	static const bool dumpAll = getenv("XXL_DUMPGROUPS") != nullptr;   // diagnostics
	if (dumpAll) DumpGroup(eventId);
	if (GroupCommandsTrio(eventId)) { CoopNoteTrioEventFlagged(eventId); DumpGroup(eventId); }
	g_origFlagEvent(self, edx, eventId);
}

} // namespace

long ProximityP2Contacts() { return g_p2Contacts; }
int ProximityGroupTargets(unsigned ev, char** objs, uint16_t* msgs, int max) { return GroupTargets(ev, objs, msgs, max); }
bool ProximityGroupCommandsTrio(unsigned ev) { return GroupCommandsTrio(ev); }
bool ProximityDispatchOnlySwitches(unsigned msg, unsigned* ev) { return DispatchOnlySwitches(msg, ev); }
bool ProximityDispatchHoldsLoser(unsigned msg) { return DispatchHoldsLoser(msg); }
// Does event `ev`'s group order "the sidekick" (wait, walk, teleport) before it
// switches the lead away from `bound`? Set off by `bound` as the game's sidekick,
// that order is the catch-up of that hero itself (LV2-6's 0C50, Obelix's: 48CE
// "teleport the sidekick", then 4800, 4803). A circle switches first, then parks
// the hero losing the lead (LV2-1's 0050: 4801, 4802) -- a hand-over.
bool ProximityGroupOrdersBeforeSwitchAway(unsigned ev, int bound) {
	char* objs[64];
	uint16_t msgs[64];
	int n = GroupTargets(ev, objs, msgs, 64);
	bool order = false;
	for (int k = 0; k < n; k++) {
		if (!objs[k] || *(uintptr_t*)objs[k] != 0x6256A0) continue;
		unsigned m = msgs[k];
		int to = m == 0x4800 || m == 0x48B7 ? HERO_ASTERIX : m == 0x4801 || m == 0x48B8 ? HERO_OBELIX : -1;
		if (to >= 0) return order && to == (bound ^ 1);
		order |= m == 0x4802 || (m >= 0x4813 && m <= 0x4876) || (m >= 0x48BB && m <= 0x48EC);
	}
	return false;
}
// The first group under way that sends the trio `msg` (its event id), or -1.
int ProximityDispatchGroupOf(unsigned msg) {
	for (int i = 0; i < g_nDispatching; i++) {
		char* objs[64];
		uint16_t msgs[64];
		int n = GroupTargets(g_dispatching[i], objs, msgs, 64);
		for (int k = 0; k < n; k++)
			if (objs[k] && *(uintptr_t*)objs[k] == 0x6256A0 && msgs[k] == msg) return g_dispatching[i];
	}
	return -1;
}
bool ProximityHeldAtDispatch(int hero) { return hero >= 0 && hero < 2 && g_heldAtDispatch[hero]; }
int ProximityLeverSwitchTarget(void* lever) {
	char* l = (char*)lever;
	int a = GroupSwitchTarget(*(uint16_t*)(l + 0x28));
	return a >= 0 ? a : GroupSwitchTarget(*(uint16_t*)(l + 0x2A));
}
long ProximitySwappedZones() { return g_swappedZones; }
long ProximityP2Squads() { return g_p2Squads; }
long ProximityP2Beacons() { return g_p2Beacons; }
long ProximitySlices() { return g_slices; }
void ProximityPassTo(char* roman, int receiver) {
	Pass* slot = &g_passes[0];
	for (Pass& p : g_passes) if (!p.roman || p.roman == roman) { slot = &p; break; }
	slot->roman = roman; slot->receiver = receiver; slot->thrower = -1; slot->since = GetTickCount();
}
bool ProximityBossRunDropsWalk(int ev) { return BossRunDropsWalk(ev); }
bool ProximityDispatchInBossRun(unsigned msg) { return DispatchInBossRun(msg); }
int ProximityDispatchZoneHero(unsigned msg, bool* withSwitch) { return DispatchZoneHero(msg, withSwitch); }
void ProximityForgetMove(void* heroNode, const float at[3]) { ForgetMove(heroNode, at); }
long ProximityP2Bonuses() { return g_p2Bonuses; }

namespace {
// A spy far from both heroes ignores its "start the dialogue" event (0x5198).
// Level 5 sector 9: the landing zone of the launcher (zone 0x0988) sends it to
// the agent of the slide; after both of its talks -- the first by the landing,
// the second at the slide's end -- the agent stays spawned at the slide's end
// (its sector stops streaming while it is still there), so the next landing
// started the slide's talk again 700 units away: the leader was sent walking
// across the level. An unspawned spy ignores its events in single player.
typedef int(__fastcall* ClueEventFn)(char* self, void* edx, unsigned ev, void* param);
ClueEventFn g_origClueEvent = nullptr;
const uintptr_t kClueManEvent = 0x4B82F0;
const float     kSpyFar = 250.0f;   // the slide's end is ~200 from its agent; the relaunch was ~700

int __fastcall ClueEventHook(char* self, void* edx, unsigned ev, void* param) {
	Trio t = GetTrio();
	if ((ev & 0xFFFF) == 0x5198 && t && CoopP2Joined()) {
		// The walk target 0x5198 sends the trio (0x5120 param +4): +0x108 for the
		// second talk of a two-part spy (+0x14 bit 1 set, bit 3 clear), else +0xFC.
		// Not the spy's node or spawn spot (+0x12C): the slide's end agents of
		// levels 4 and 5 stand ~800 units from those.
		uint8_t f = *(uint8_t*)(self + 0x14);
		const float* p = (const float*)(self + (((f & 2) && !(f & 8)) ? 0x108 : 0xFC));
		if (std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2])) {
			Vector3 target(p[0], p[1], p[2]);
			float best = 1e30f;
			for (int h = 0; h < 2; h++) {
				Hero hero = t.GetHero(h);
				if (!hero) continue;
				// Where the hero is drawn: during a ride (the belly slide) +kHero_pos
				// stays where the ride began while the node moves with it.
				Vector3 at = hero.Pos();
				NodePos(hero.hook->node, &at);
				float d = sqrtf((at - target).len2());
				if (d < best) best = d;
			}
			if (best > kSpyFar) {
				Log("proximity: spy %p told to start its talk at (%.1f %.1f %.1f), %.0f units from the nearest hero; ignored",
				    self, target.x, target.y, target.z, best);
				return 1;
			}
		}
	}
	return g_origClueEvent(self, edx, ev, param);
}
} // namespace

bool ProximityInstallHooks() {
	if (getenv("XXL_PASSLOG"))
		for (int i = 0; i < 5; i++)
			PatchVtableSlot(kRomanVtbls[i], 5, kRomanSends[i], (void*)RomanSendHook, (void**)&g_origRomanSend[i], "Roman::sendEvent (pass log)");
	bool ok = PatchVtableSlot(kBundleVtbl, kBundleTickSlot, kBundleTick,
	                          (void*)BundleTickHook, (void**)&g_origBundleTick, "CKBundle::tick");
	ok &= PatchVtableSlot(kSrvEventVtbl, 17, kEventDispatch, (void*)EventDispatchHook, (void**)&g_origEventDispatch,
	                      "CKSrvEvent::vf17 (the groups dispatched: sp_hero_switch_lock)");
	ok &= PatchVtableSlot(kBonusPoolVtbl, kPoolUpdateSlot, kBonusPoolUpdate,
	                      (void*)PoolUpdateHook, (void**)&g_origPoolUpdate, "CKGrpBonusPool::update");
	ok &= PatchVtableSlot(kAsterixBonusPoolVtbl, kPoolUpdateSlot, kAsterixBonusPoolUpdate,
	                      (void*)AsterixPoolUpdateHook, (void**)&g_origAsterixPoolUpdate,
	                      "CKGrpAsterixBonusPool::update");
	ok &= PatchVtableSlot(kBonusPoolVtbl, kPoolSpawnSlot, kPoolSpawn, (void*)PoolSpawnHook, (void**)&g_origPoolSpawn,
	                      "CKGrpBonusPool::spawn");
	ok &= PatchVtableSlot(kAsterixBonusPoolVtbl, kPoolSpawnSlot, kPoolSpawn, (void*)PoolSpawnHook, (void**)&g_origPoolSpawn,
	                      "CKGrpAsterixBonusPool::spawn");
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
	ok &= PatchVtableSlot(kClueManVtbl, 5, kClueManEvent, (void*)ClueEventHook, (void**)&g_origClueEvent,
	                      "CKHkClueMan::sendEvent (far spies)");
	ok &= PatchVtableSlot(kDetectVtbl, kDetectTickSlot, kDetectTick, (void*)DetectTickHook,
	                      (void**)&g_origDetectTick, "CKSrvDetection::tick");
	{
		const uintptr_t vts[3] = { 0x6266B0, 0x62682C, 0x626A6C };   // CKGrpSquadEnemy, CKGrpSquad, CKGrpSquadJetPack
		for (uintptr_t vt : vts)
			ok &= PatchVtableSlot(vt, 38, kMarkerSpawn, (void*)MarkerSpawnHook, (void**)&g_origMarkerSpawn,
			                      "squad marker spawn (both players' views)");
		for (uintptr_t vt : vts)
			ok &= PatchVtableSlot(vt, 36, kSquadFree, (void*)SquadFreeHook, (void**)&g_origSquadFree,
			                      "squads freed for Romans (both players' squads kept)");
	}
	ok &= InstallDetour(kSpawnPoint, (void*)SpawnPointHook, (void**)&g_origSpawnPoint, kSpawnPointPrologue,
	                    sizeof(kSpawnPointPrologue), "Roman reinforcement point (both players' cameras)");
	ok &= InstallDetour(kFightMode, (void*)FightHook, (void**)&g_origFight, kFightModePrologue,
	                    sizeof(kFightModePrologue), "trio fight mode (music) for the non-leader");
	ok &= InstallDetour(kFlagEvent, (void*)FlagEventHook, (void**)&g_origFlagEvent, kFlagEventPrologue,
	                    sizeof(kFlagEventPrologue), "event flag (zones)");
	return ok;
}
