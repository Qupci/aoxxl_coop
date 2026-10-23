// Drop-in co-op on top of the hero Life layer (docs/RESEARCH.md, "Hero control").
//
//  * Each hero's Life tick picks player or AI behaviour from the control bits in
//    hero+0x18: (f18 & 3) == 0 is the player variant of the current mode. Player
//    2's hero is held in the player variant of mode 0 (normal play), so its Life
//    runs the very handlers player 1's hero runs -- gravity, jumping, attacks and
//    all -- instead of the AI ones.
//  * Both heroes read input through one shared view onto CKInput's value array.
//    During player 2's hero tick that array pointer is swapped for one built from
//    player 2's pad, and swapped back straight after.
//  * Scripted modes (control modes 1..3: rides, ballista, belly slide, cinematics)
//    belong to the game. Co-op stands aside while either hero is in one, and around
//    every switch/mode event the trio is shown exactly the arrangement it expects
//    (only its leader in a player state), then player control is re-applied.

#include "coop.h"
#include "gamecam.h"
#include "crash.h"
#include "proximity.h"
#include "detour.h"
#include "experimental.h"
#include "klusters.h"
#include "sound.h"
#include "autotest.h"
#include "config.h"
#include "inputmgr.h"
#include "life.h"
#include "log.h"
#include "p2input.h"
#include "padident.h"
#include "p2cam.h"
#include "pads.h"
#include "hints.h"
#include "individual.h"
#include "freedom.h"

#include <intrin.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

void StartAiCycle(int idx, const char* why);   // below: two ticks through the AI variant
static void ServeMachineRequest(Trio t);       // below: Obelix's X at a machine
static bool MachineMatesNear(Trio t);
static void RefuseMachine(Trio t, int kind, int who);
static void ShowJoinHint(Trio t);
static void WatchLifterHint(Trio t);
static bool GunSolo(Trio t);
static void JoinGun(Trio t, const char* why);
static void GiveBackLead(Trio t);
void CloseShopFor(const char* why);

namespace {

// Player 2's action array. CKInput keeps a second bank of combo state right after
// the first numActions bytes, and a remap entry could in principle point into it,
// so this is sized well past both banks and everything beyond the first is zero.
const int kP2ValuesSize = 256;

struct CoopState {
	bool    p2Joined = false;
	int     p2Pad    = -1;
	int     p1Hero   = -1;
	int     p2Hero   = -1;
	DWORD   p2BackHeldAt = 0;
	bool    p2BackCombo = false;    // Back is held for a combo, not to leave
	bool    cineRegrouped = false;  // RegroupForCutscene done for the current cutscene
	int     storyZoneHero = -1;     // a cutscene zone bound to the other hero set off by this one
	DWORD   storyZoneAt = 0;
	int     bossRunHero = -1;       // woke a boss the heroes run from; the other player is brought along

	uint8_t p2Values[kP2ValuesSize] = {};
	bool    p2ValuesReady = false;
	uint8_t p2CamValues[kP2ValuesSize] = {};   // p2Values plus the right stick, for the game camera's ticks
	int     p2Combos = 0;             // combo bytes after the actions (P2RecognizeCombos)
	int     comboHitsHero = -1;       // the combo cheat: the hero landing two hits soon (CombosCheat)
	DWORD   comboHitsAt = 0;

	uint8_t* savedValues = nullptr;   // CKInput's own array while player 2's is in
	char*    savedBindings = nullptr; // CKInput+0x18 likewise (hold time in seconds at +0x18 each)
	float    p2HoldStart[256];
	float    p2HoldTime[256] = {};
	bool     swapped = false;

	// The trio's leader/companion bytes while player 2's hero is ticking (see
	// CoopBeforeLifeTick): saved values, and the values we put there.
	bool     sliced = false;
	uint8_t  sitBefore = 0xFF;        // the sliced hero's situation before its tick
	uint8_t  f18Before = 0;           // ... and its control bits
	bool     hidDash = false;         // game manager +0x5C bit 19 hidden for the tick
	int      dashHero = -1;           // the magic potion's dash is this hero's (-1: none up)
	int      aiCycle[HERO_COUNT] = {};   // ticks left in the AI variant (situation reset)
	DWORD    stuckSince[HERO_COUNT] = {};
	DWORD    outsideSince[HERO_COUNT] = {};   // WatchSectors: outside the streamed sector since
	Vector3  stuckPos[HERO_COUNT];
	uint8_t  savedLeader = 0, savedCompanion = 0;
	uint8_t  slicedHero = 0, slicedOther = 0;   // what the trio said during the sliced tick
	int      groupTookLead = -1;      // a hero that took the lead to start a group action
	int      groupPrevLeader = -1;    // ... and the leader before it, who gets it back after
	bool     groupLauncher = false;   // ... that group action was a launcher: the lead stays (single player's)
	DWORD    launcherAt = 0;          // the last launcher start (trio 0x4804 from 0x49C699)
	uint32_t savedOffer = 0;          // trio+0x4C hidden from player 2's tick
	uint32_t offerValue = 0;          // the last trio+0x4C a trio event put there...
	int      offerHero = -1;          // ... for this hero (the leader byte then: an object ticked as its)
	uint32_t leaderHidOffer = 0;      // trio+0x4C hidden from the leader's tick (the other hero's)

	bool    suspended = false;
	bool    shopping = false;            // a merchant's menu is open
	int     shopper = -1;                // ... by this hero (leading or not)
	int     shopHero = -1;               // the merchant's hero from its X until it is out of situation 0A
	bool    oneScreen = false;           // Twister Fusion: co-op on, split screen off
	bool    held[HERO_COUNT] = {};       // a player's hero the game has borrowed (scripted AI order)
	uint8_t lastOrder[HERO_COUNT] = {};
	int     cutsceneHold = 0;    // frames left before a cutscene counts as over
	int     lastLevel = -2;
	DWORD   lastFpsTick = 0;
	uint32_t frames = 0;
	uint32_t frameNo = 0;

	// A Start press is only accepted as a join a few frames later, once it is
	// clear the game's own input did not react to the very same press -- which
	// would mean the pad is player 1's.
	int      pendingPad = -1;
	uint32_t pendingFrame = 0;
	bool     pendingEcho = false;

	// Hints.
	bool     joinHintPending = false;   // a level started: say how player 2 joins
	int      spinHints = 0;             // "LB passes the Roman" shown this level
	bool     spinning[HERO_COUNT] = {};
	bool     passDone = false;          // a Roman was passed this session: no more pass hints

	// Obelix's X at a machine (machine gun, boat), served next frame.
	char*    machineReq = nullptr;
	int      machineKind = 0;           // 1 boat, 2 machine gun (trio +0x3F4 bits 10-11)
	DWORD    machineReqAt = 0;
	bool     machineSending = false;
	bool     gunSolo = false;           // Asterix on the machine gun, Obelix's player free (split screen on)
	bool     gunJoinReq = false;        // Obelix's X at the gun's handles, served next frame
	bool     gunJoining = false;        // ... being served: 0x4809's companion order is wanted

	// Death (shared health at 0): the hero that took the last point; it leads
	// for the death, the other hero is the AI companion until the respawn.
	int      dying = -1;
	bool     dyingLeads = false;        // ... and has been given the lead
	uint8_t  hpBeforeTick = 0;
	int      indivPrev = -1;            // individual heroes: whose stats were in place before the tick
	char*    closedShop = nullptr;
	int      shopCloseCheck = 0;        // frames until the forced close's result is logged
	char*    shopCamPending = nullptr;  // a forced close met another camera (a cutscene's): hand back once the shop's is up again
	DWORD    shopCamSince = 0;
	int      shopClosedHero = -1;       // the shopper of the last forced close, and when
	DWORD    shopClosedAt = 0;
	bool     shopLeadBack = false;      // the merchant's visit over: the lead goes back once the shopper is out
	bool     shopForcedClosed = false;  // the merchant closed for the other player's dialogue (until the shopper is out)
	int      lifterWaitsFor = -1;       // a lifter waits at the bottom for this hero (the other on it)
} g;

const int kJoinSettleFrames = 5;
const int kCutsceneHoldFrames = 15;   // ~0.25 s

// Hero situation-machine state (hero+0x3E9) of group actions: FUN_0049C030, which
// sends the companion and Idefix to markers and waits for them (lifters).
const uint8_t kSituationGroupAction = 0x0D;
// The walk to a Roman agent and the talk with him (X on the spy, or trio 0x5120).
const uint8_t kSituationTalk = 0x09;
// The walk into the merchant's shop (X at CKHkAsterixShop: trio 0x48EF sends
// "the companion" there too, AI order 14; the shop's camera follows the leader).
const uint8_t kSituationShop = 0x0A;
const int     kOrderDialoguePlace = 13;   // the companion's AI order from trio 0x48BA

const char* ModeName(uint8_t mode) {
	switch (mode) {
		case 0: return "normal";
		case 1: return "mode1";
		case 2: return "mode2";
		case 3: return "mode3(slide/cinematic)";
		case 4: return "swim";
		default: return "?";
	}
}

int OtherHero(int idx) { return idx == HERO_ASTERIX ? HERO_OBELIX : HERO_ASTERIX; }

uint8_t* F18(Hero h) { return (uint8_t*)(h.raw() + 0x18); }

// The heroes' shared health: game manager +0x40 -> +0x50 (points, 5 a shield),
// +0x51 (shields, 3 to 6).
uint8_t* HealthPtr() {
	CKYellowPages* yp = YellowPages();
	char* gm = yp ? (char*)yp->gameManager : nullptr;
	char* cb = gm ? *(char**)(gm + 0x40) : nullptr;
	return cb ? (uint8_t*)(cb + 0x50) : nullptr;
}
int Health() {
	uint8_t* hp = HealthPtr();
	return hp ? *hp : -1;
}

// Puts hero `idx` into the player variant of its current mode if that mode is a
// playable one (normal or swimming). Scripted modes are left to the game.
bool EnforcePlayer(Trio t, int idx) {
	Hero h = t.GetHero(idx);
	if (!h || (idx >= 0 && idx < HERO_COUNT && (g.held[idx] || g.aiCycle[idx] > 0))) return false;
	if (g.dying >= 0 && idx != g.dying) return false;   // the survivor watches the death as the AI companion
	uint8_t f = *F18(h);
	if (!HeroModePlayable(f) || HeroIsPlayerVariant(f)) return false;
	*F18(h) = HeroPlayerVariant(f);
	InterlockedIncrement(&g_hookCounters.enforced);
	LogV("coop: %s -> player (f18 %02X -> %02X)", HeroName(idx), f, *F18(h));
	return true;
}

bool IsScriptedOrder(int o) {
	return o != AI_FOLLOW_LEADER && o != AI_HIT_ENEMY && o != AI_THROW_ENEMY && o != AI_BITE_ENEMY &&
	       o != AI_TAKE_HIT;
}

// Locked sequences. A player's hero locked by the level -- frozen as the
// companion ("disable sidekick movement", trio 0x4802: a blue circle, the
// rotary beam), or held for a ride (the rope-pulled cable car) -- waits for the
// other hero to finish it. When the lock starts the detectors' enabled bits
// (detection service yp +0x54: five lists +0x2C/+0x34/+0x3C/+0x44/+0x4C of 8
// byte entries, counts at +4, bit 0 of byte 4) are kept: the lock disables its
// own box (a blue circle: "Disable Enter Bounding Box detector") and enables the
// ones that finish it.
// A locked sequence's "before" picture, so it can be undone (the waiting hero's
// partner fell): the detectors' enable bits, the state flags (+8) of blue
// circles (CKHkPressionStone 2,21) and spinning platforms (CKHkCorkscrew 2,44:
// bit 0 "set off", cleared only when its turn ends), and the event that set it
// off -- its group switched the circle's light (CKHkLightPillar 2,60: 0x900 on,
// 0x901 off), switched back on undo. (Models aren't put back wholesale: crates
// broken meanwhile came back.) Taken when an event that commands the trio is
// flagged (FUN_00460C20), before its group runs; a lock noted soon after keeps
// that picture.
struct LockPicture {
	size_t  n = 0;
	uint8_t bits[4096];
	unsigned event = 0xFFFF;
	struct Flags { char* hook; uint32_t f; } flags[256];
	size_t  nFlags = 0;
	DWORD   at = 0;
	int     leader = -1;   // the trio's leader then (the circle's 4801 hands the lead to the other)
	struct Cam { char* sec; uint8_t f; } cams[256];   // camera sectors (CKCameraSector, service +0x24): +0x29
	size_t  nCams = 0;
};
struct LockState {
	int         hero = -1;           // the locked hero
	char*       car = nullptr;       // the rope-pulled cable car it rides (CKHkTowedTelepher 2,159)
	DWORD       since = 0;
	bool        wait = false;        // a companion told to wait (0x4802: a circle), not a scripted walk or ride
	Vector3     at;                  // where the locked hero stood (the circle)
	LockPicture pic;
} g_lock;
LockPicture g_prePic;
// A lock that ended (0x4803) while its sequence goes on: the spinning platform
// frees Asterix once Obelix is at the turnstile, but it stays switched off
// (0x99A) and its pillar dark until Asterix reaches the box at its top. Kept
// until then. Obelix spins the platform at its turnstile (X, 3.8 units from the
// circle); gone off (the lifter) it never turns, and the pillar stays dark:
// WatchUnfinishedSequence sets it back.
LockPicture g_seqPic;
int g_seqHero = -1;   // the hero that lock held
bool g_seqWait = false;   // that lock was a "wait here" (0x4802)
Vector3 g_seqAt;      // where it stood
DWORD g_seqAwaySince = 0;
bool g_seqLive = false;
// A rope-pulled cable car boarded under a lock, and where it stood then (its
// start): the car rolls back there by itself once let go -- unless the pull
// was short and it stops on the way, and a level zone frees the rider (LV3-3:
// Obelix walks off to the lifter, zone 0830 sends 4800 / 4803). Left there it
// never offers itself again (WatchStrandedCar). Any level event that frees the
// rider mid-way does it; the cross detector switching the leader is the one met.
// Its rider, freed where the car stopped (or put back by an undo), stands in the
// car's zone: it is kept off cable cars until the car is home and free to board
// (+0x7C bit 0x80) for 1 s, or it has walked 4 units from where it settled -- its
// place over the first 0.5 s (in the frame of the put-back the car's offer pulled
// the hero back to it, which read as having walked away).
struct CarWatch {
	char*   car = nullptr;
	int     rider = -1;
	Vector3 start;
	DWORD   stillSince = 0;
	DWORD   drivenAt = 0;   // sent driving back then
	int     keptOff = -1;
	char*   keptOffCar = nullptr;
	DWORD   keptOffSince = 0, keptOffFreeSince = 0;
	Vector3 keptOffAt;
} g_carWatch;

static void KeepOffCars(int hero, char* car) {
	g_carWatch.keptOff = hero;
	g_carWatch.keptOffCar = car;
	g_carWatch.keptOffSince = GetTickCount();
	g_carWatch.keptOffFreeSince = 0;
}

static char* DetectionService() {
	CKYellowPages* yp = YellowPages();
	char* det = yp ? *(char**)((char*)yp + 0x54) : nullptr;
	return det && *(uintptr_t*)det == 0x62504C ? det : nullptr;
}

static void TakePicture(LockPicture& p) {
	p.n = p.nFlags = 0;
	p.event = 0xFFFF;
	p.at = GetTickCount();
	Trio t = GetTrio();
	p.leader = t ? t.Leader() : -1;
	if (char* det = DetectionService()) {
		const int bases[5] = { 0x2C, 0x34, 0x3C, 0x44, 0x4C };
		for (int b : bases) {
			char* e = *(char**)(det + b);
			unsigned n = *(uint16_t*)(det + b + 4);
			for (unsigned i = 0; e && i < n && p.n < sizeof(p.bits); i++) p.bits[p.n++] = (uint8_t)e[8 * i + 4];
		}
	}
	CKYellowPages* yp = YellowPages();
	char* cs = yp ? *(char**)((char*)yp + 0x68) : nullptr;
	p.nCams = 0;
	for (char* sec = cs ? *(char**)(cs + 0x24) : nullptr; sec && p.nCams < 256; sec = *(char**)(sec + 4))
		p.cams[p.nCams++] = { sec, *(uint8_t*)(sec + 0x29) };
	static const unsigned kCircleIds[] = { 21, 44 };
	for (unsigned id : kCircleIds)
		for (unsigned i = 0, n = GetNumLvlClassInstances(2, id); i < n; i++) {
			char* o = (char*)GetLvlObject(2, id, i);
			if (!o) continue;
			if (p.nFlags < 256) p.flags[p.nFlags++] = { o, *(uint32_t*)(o + 8) };
		}
}

static int CamerasBack(const LockPicture& p);

void NoteTrioEventFlagged(unsigned eventId) {
	if (!g.p2Joined) return;
	TakePicture(g_prePic);
	g_prePic.event = eventId & 0xFFFF;
	// The level starting a wait's sequence over (LV2-8's zone 03B8, Asterix off the
	// platforms: the light pillar on again, 0x900, and the companion freed, 4803):
	// the cameras it switched on meanwhile go back off too.
	char* objs[64];
	uint16_t msgs[64];
	int n = ProximityGroupTargets(eventId, objs, msgs, 64);
	bool pillar = false, freed = false;
	for (int i = 0; i < n; i++) {
		uintptr_t vt = objs[i] ? *(uintptr_t*)objs[i] : 0;
		if (vt == 0x629030 && msgs[i] == 0x900) pillar = true;
		if (vt == 0x6256A0 && msgs[i] == 0x4803) freed = true;
	}
	const LockPicture* pic = g_lock.hero >= 0 && g_lock.wait ? &g_lock.pic : g_seqLive ? &g_seqPic : nullptr;
	if (!pillar || !freed || !pic) return;
	int off = CamerasBack(*pic);
	if (off) Log("coop: event %04X starts the wait's sequence over -- %d camera sectors it switched on are off again", eventId & 0xFFFF, off);
}

// A cable car's boarding (the hero's own tick, 0x49ACB9) switches the lead to
// the puller (4801) just before it takes the rider (4812): the lock's picture,
// taken then, had the puller as "the leader before" -- R3 + B left Obelix leading.
struct CarSwitch { int leader = -1; DWORD at = 0; } g_carSwitch;

static void NoteLock(int hero, const char* why) {
	// (the same lock noted twice -- its 0x4802, then the held order -- keeps the
	// first picture)
	if (hero < 0 || (g_lock.hero == hero && GetTickCount() - g_lock.since < 60000)) return;
	bool pre = g_prePic.at && GetTickCount() - g_prePic.at < 3000;
	if (pre) g_lock.pic = g_prePic;
	else TakePicture(g_lock.pic);
	g_prePic.at = 0;
	if (g_carSwitch.at && GetTickCount() - g_carSwitch.at < 500) g_lock.pic.leader = g_carSwitch.leader;
	g_carSwitch.at = 0;
	g_lock.hero = hero;
	g_lock.wait = false;
	g_lock.since = GetTickCount();
	g_seqLive = false;
	// The car boarded (the rider's own position stays at the boarding spot
	// while the car moves on): the nearest one now.
	g_lock.car = nullptr;
	float best = 12.0f * 12.0f;
	Trio t = GetTrio();
	Hero h = t ? t.GetHero(hero) : Hero();
	if (h) g_lock.at = h.Pos();
	for (unsigned i = 0, n = h ? GetNumLvlClassInstances(2, 159) : 0; i < n; i++) {
		char* car = (char*)GetLvlObject(2, 159, i);
		char* node = car ? *(char**)(car + 0x10) : nullptr;
		char* fr = node ? *(char**)(node + 4) : nullptr;
		if (!fr) continue;
		const float* p = (const float*)(fr + 0x80);
		float d2 = (h.Pos() - Vector3(p[0], p[1], p[2])).len2();
		if (d2 < best) { best = d2; g_lock.car = car; g_carWatch.start = Vector3(p[0], p[1], p[2]); }
	}
	if (g_lock.car) {
		g_carWatch.car = g_lock.car;
		g_carWatch.rider = hero;
		g_carWatch.stillSince = g_carWatch.drivenAt = 0;
	}
	Log("coop: %s locked by the level (%s) -- %u detectors, %u circles kept (%s %04X)", HeroName(hero), why,
	    (unsigned)g_lock.pic.n, (unsigned)g_lock.pic.nFlags, pre ? "as before its event" : "as now", g_lock.pic.event);
}

static void EndLock(bool goesOn = false) {
	if (goesOn && g_lock.hero >= 0) {
		g_seqPic = g_lock.pic;
		g_seqHero = g_lock.hero;
		g_seqWait = g_lock.wait;
		g_seqAt = g_lock.at;
		g_seqAwaySince = 0;
		g_seqLive = true;
	}
	g_lock.hero = -1;
}

// The ended lock's sequence is still under way: a spinning platform it set off
// (bit 0) or switched off (bit 1: the zone that frees Asterix sends 0x99C and
// 0x99A; the turn's end switches it on again) is not as before.
static bool SequenceUnderWay() {
	// (only after a "wait here": LV2-1's platform top zone 0058 walks Obelix to a
	// marker -- that lock's picture, taken as the sequence's, was "set back" later:
	// 7 detectors from before 0058 and the lead to Obelix)
	if (!g_seqLive || !g_seqWait) return false;
	for (size_t i = 0; i < g_seqPic.nFlags; i++) {
		char* o = g_seqPic.flags[i].hook;
		if (*(uintptr_t*)o == 0x628E0C && ((g_seqPic.flags[i].f ^ *(uint32_t*)(o + 8)) & 3)) return true;
	}
	g_seqLive = false;   // finished (or never set one off)
	return false;
}

// Camera sectors switched on (+0x29 bit 1) since the picture, switched off again:
// a camera picks the first one on whose box holds its target and keeps it until
// it is in another's -- LV2-8's platforms' shot (on with the second block pushed
// into place) covers the ground below too, and after Asterix fell off (the
// level's start over, zone 03B8) player 1's camera kept it.
static int CamerasBack(const LockPicture& p) {
	int n = 0;
	for (size_t i = 0; i < p.nCams; i++) {
		uint8_t& f = *(uint8_t*)(p.cams[i].sec + 0x29);
		if ((f & 2) && !(p.cams[i].f & 2)) { f &= ~2; n++; }
	}
	return n;
}

static void RestoreLockPicture(const LockPicture& p) {
	int changed = 0, shown = 0, flags = 0, cams = CamerasBack(p);
	if (char* det = DetectionService()) {
		size_t k = 0;
		const int bases[5] = { 0x2C, 0x34, 0x3C, 0x44, 0x4C };
		for (int b : bases) {
			char* e = *(char**)(det + b);
			unsigned n = *(uint16_t*)(det + b + 4);
			for (unsigned i = 0; e && i < n && k < p.n; i++, k++) {
				uint8_t& f = *(uint8_t*)(e + 8 * i + 4);
				if ((f & 1) != (p.bits[k] & 1)) { f = (uint8_t)((f & ~1) | (p.bits[k] & 1)); changed++; }
			}
		}
	}
	if (p.event != 0xFFFF) {
		char* objs[64];
		uint16_t msgs[64];
		int n = ProximityGroupTargets(p.event, objs, msgs, 64);
		typedef int(__fastcall* SendEventFn)(void* self, void* edx, int event, void* param);
		for (int i = 0; i < n; i++)
			if (objs[i] && *(uintptr_t*)objs[i] == 0x629030 && (msgs[i] == 0x900 || msgs[i] == 0x901)) {
				((SendEventFn)(*(void***)objs[i])[5])(objs[i], nullptr, msgs[i] ^ 1, nullptr);
				shown++;
			}
	}
	for (size_t i = 0; i < p.nFlags; i++) {
		uint32_t& f = *(uint32_t*)(p.flags[i].hook + 8);
		if (f != p.flags[i].f) { f = p.flags[i].f; flags++; }
	}
	Log("coop: as before the lock: %d detectors, %d light pillars switched back, %d circles' states put back, %d camera sectors off",
	    changed, shown, flags, cams);
}

// Freedom mode ([coop] freedom): the level never takes a player's hero -- no
// "wait here" (light pillars, the rotary beam's circle), no walks to markers; the
// player does it by hand. Cable cars still do: the rope-pulled one (order 9) and
// the automatic CKHkTelepher (12, external control). Not the mod's own holds (the
// machine gun Obelix's player asked to join).
static bool FreedomLetsHold(int order) {
	return !g_config.freedom || order == AI_USE_CABLECAR || order == AI_EXTERNAL_CTRL;
}

// Waits freedom mode still lets lock the hero (CoopNoteTrioEvent, at the 0x4802):
// - the boss (LV2-10, LV3-6; its code at 0x4F3165/0x4F317E: "Obelix leads,
//   Asterix waits") once Asterix is on its button (the automatic cable car's end
//   jumps him there) -- left free he walked off and the boss fight was cheesed;
// - a corkscrew's circle (CKHkCorkscrew 2,44 -- LV3-6's light pillar, LV2-1's
//   spinning platform; [coop] freedom_corkscrews): it launches the waiting hero
//   (0x0C09, AI order 2), aimed where the level wants it -- by hand it was a blind
//   jump (LV3-6 boss: onto the cable car).
struct FreedomLock { int hero = -1; DWORD at = 0; const char* what = ""; } g_freedomLock;
static bool FreedomKeepsLock(int idx) {
	return g_freedomLock.hero == idx && GetTickCount() - g_freedomLock.at < 500;
}
// The hero a corkscrew's circle locked in freedom mode, until the ride gives it
// the lead (LV2-1: the top's box 0058) or lets it go. Its whole ride is the
// level's, as in single player: on its way to the turnstile Obelix crosses zone
// 03C8, whose 4803 freed Asterix -- the ride's walk onto the platform (0528:
// 482D) and the jump off at its top (0530: 484C) were then "the level taking a
// player's hero", not followed; he rode up and stayed there.
// Its launch (the corkscrew's 0x0C09 from 0x53C5F4 -> 4805, the companion's AI
// order 2, a jump to its marker) ends the lock's purpose: once that jump is over
// the hero is its player's again. LV5-3's corkscrew (-259.7 10.5 353.7) lands
// Asterix in a box (-263.9 16 352.5 / -261.9 20 354.5, event 0118: 4800 + 4803)
// that frees him -- in the user's run it never went off, and he stayed the game's.
struct CorkRide { int hero = -1; char* cork = nullptr; DWORD launched = 0; bool jumping = false; } g_corkRide;

void Hold(Trio t, int idx, const char* why, bool modOwn = false) {
	Hero h = t.GetHero(idx);
	if (!h || g.held[idx]) return;
	if (!modOwn && !FreedomLetsHold((int)h.Order()) && FreedomKeepsLock(idx))
		Log("coop: freedom: %s locked all the same (%s)", HeroName(idx), g_freedomLock.what);
	else if (!modOwn && !FreedomLetsHold((int)h.Order())) {
		static int s_told = 0;
		if (s_told++ < 40)
			Log("coop: freedom: %s stays its player's (%s, AI order %d not followed)", HeroName(idx), why, (int)h.Order());
		return;
	}
	g.held[idx] = true;
	g.aiCycle[idx] = 0;   // the game's now; no reset pass afterwards
	uint8_t f = *F18(h);
	if (HeroModePlayable(f)) *F18(h) = HeroAiVariant(f);
	Log("coop: %s handed to the game (%s, AI order %d)", HeroName(idx), why, (int)h.Order());
	NoteLock(idx, "held for a scripted order");
}

uint8_t Situation(Hero h) { return h ? *(uint8_t*)(h.raw() + 0x3E9) : 0xFF; }

// A cutscene is playing: CKSrvCinematic (yellowPages+0x80) keeps its scenes at
// +0x10 (count +0x14), and bit 0 of a scene's +0x20 flags means it is running
// (CKSrvCinematic::vf17 only updates those). Player 1 cannot move then, so
// player 2's hero goes back to the game until it ends.
// Cutscenes. CKSrvCinematic (yellow pages +0x80) recomputes its letterbox bars
// every tick: hidden first (FUN_0046AE20), shown again (FUN_0046ADE0, +0xC bit 0)
// if a playing scene wants them (scene +0x30 bit 0). The per-scene "playing" bit
// is no use on its own: ambient scenes play all along (level 4 scene 0). Seen on
// level 2's scene 1: bars on, game manager +0x5C bit 20 on (the HUD hides itself
// on it), the camera service's camera a plain CKCamera, and no hero Life ticks at
// all until it ends. Only the bars decide; the rest is logged when it changes.
bool CutsceneBars() {
	CKYellowPages* yp = YellowPages();
	char* srv = yp ? *(char**)((char*)yp + 0x80) : nullptr;
	if (srv && *(uintptr_t*)srv == 0x624F90 && (*(uint8_t*)(srv + 0xC) & 1) != 0) return true;
	// Dialogues (the Roman agent: trio events 0x5120..0x5121) show no bars but
	// take the camera, and set game manager +0x5C bit 20 for exactly that long
	// (the HUD hides on it; the pause menu sets it too, harmlessly).
	char* gm = yp ? (char*)yp->gameManager : nullptr;
	return gm && (*(uint32_t*)(gm + 0x5C) & 0x100000) != 0;
}

void LogCutsceneSignals() {
	CKYellowPages* yp = YellowPages();
	if (!yp) return;
	char* gm = (char*)yp->gameManager;
	char* cam = *(char**)((char*)yp + 0x68);
	char* track = cam ? *(char**)(cam + 0x38) : nullptr;
	uint32_t hud = gm ? (*(uint32_t*)(gm + 0x5C) & 0x100000) : 0;
	uintptr_t camVt = track ? *(uintptr_t*)track : 0;
	static uint32_t lastHud = 0;
	static uintptr_t lastCamVt = 0;
	if (hud != lastHud || camVt != lastCamVt) {
		Log("coop: cutscene signals: bars=%d gm5C.20=%d camera vt %08X", CutsceneBars(), hud != 0,
			(unsigned)camVt);
		lastHud = hud;
		lastCamVt = camVt;
	}
}

// A hero a player drives has no AI state: when the game moves a hero from its AI
// Life state to a player one, it runs the current AI state's exit handler and
// sets hero+0x561 to 0xFF (CKHkObelixLife::State1_Ai, mode 2). Group actions break
// that in co-op: at its 17 s timeout the lifter handler (FUN_0049C030) forces "the
// companion" into AI state 3 ("wait") -- which can be a player-driven hero, whose
// AI machine never ticks, so the 3 stays. The next lifter then sees a companion
// "already waiting", empties its marker slot, and dereferences it (the crash at
// axxl.exe+0x9C228). Clearing the leftover the same way the game does keeps the
// invariant the lifter code relies on.
void ClearStaleAiState(int idx, Hero h) {
	uint8_t st = *(uint8_t*)(h.raw() + 0x561);
	if (st == 0xFF) return;
	// Only once the hero's Life already runs a player state: on the tick that
	// switches it over, the Life's own AI-state exit does this job.
	uint8_t life = LifeLastState(idx);
	if (life == 0xFF || (life & 1)) return;
	typedef char(__fastcall* StateFn)(void* self, void* edx, int mode, int other);
	void*    owner = *(void**)(h.raw() + 0x558);
	StateFn* table = *(StateFn**)(h.raw() + 0x55C);
	if (owner && table) table[st](owner, nullptr, 2, 0xFF);
	*(uint8_t*)(h.raw() + 0x561) = 0xFF;
	Log("coop: cleared leftover AI state %u on player-driven %s", st, HeroName(idx));
}

bool CoopAllowedNow(Trio t, const char** why) {
	if (!t) { *why = "no trio"; return false; }
	if (GetNumLvlClassInstances(ax::kCKHkObelix) == 0) { *why = "Obelix not in level"; return false; }
	Hero a = t.GetHero(HERO_ASTERIX), o = t.GetHero(HERO_OBELIX);
	if (!a || !o) { *why = "hero missing"; return false; }
	// Level 1: Obelix is in the level before Asterix finds him, waiting at his
	// spot (hero +0x1A nonzero; trio 0x4883 "discover Obelix" clears it).
	if (*(uint8_t*)(o.raw() + 0x1A) != 0) { *why = "Obelix has not joined Asterix yet"; return false; }
	if (!HeroModePlayable(a.Flags18()) || !HeroModePlayable(o.Flags18())) { *why = "scripted mode"; return false; }
	if (a.Busy() || o.Busy()) { *why = "hero busy"; return false; }
	if (CutsceneBars() || g.cutsceneHold > 0) { *why = "cutscene"; return false; }
	// Joining at the merchant left the shop's camera on player 2's half for good:
	// not while one is open (CKHkAsterixShop 2,172: state +8 bits 0-2).
	for (unsigned i = 0, n = GetNumLvlClassInstances(2, 172); i < n && i < 16; i++) {
		char* s = (char*)GetLvlObject(2, 172, i);
		if (s && (*(uint8_t*)(s + 8) & 7) != 0) { *why = "at the merchant"; return false; }
	}
	return true;
}

void ForgetP2() {
	P2CamReset();
	g.p2Joined = false;
	g.p2Pad = -1;
	g.p1Hero = g.p2Hero = -1;
	g.p2BackHeldAt = 0;
	g.p2ValuesReady = false;
	g.suspended = false;
	g.shopping = false;
	g.shopHero = -1;
	g.dashHero = -1;
	g.shopLeadBack = false;
	g.dying = -1;
	g.dyingLeads = false;
	for (int i = 0; i < HERO_COUNT; i++) g.held[i] = false;
}

// Death. Every player-driven hero with the health at 0 (hero +0x390 bit 2, from
// Hero_PreLifeTick) goes into situation 0x0E, the death, whose cutscene follows
// the trio's leader. In single player only the leader is player-driven, and an
// AI companion that loses the last point (a death pit: FUN_0049D610) first takes
// the lead with trio 0x4800/0x4801 -- the death is shown on the hero that died.
// In co-op the hero that took the last point is noted (its own tick, or a hit
// through its sendEvent), the other hero becomes the AI companion before it can
// tick into its own death, and the dying hero gets the lead the game's way.
void NoteDeath(int idx, const char* how) {
	if (!g.p2Joined || g.suspended || g.dying >= 0 || (idx != HERO_ASTERIX && idx != HERO_OBELIX)) return;
	Trio t = GetTrio();
	if (!t) return;
	g.dying = idx;
	g.dyingLeads = false;
	int other = OtherHero(idx);
	Hero o = t.GetHero(other);
	if (o && HeroModePlayable(*F18(o))) *F18(o) = HeroAiVariant(*F18(o));
	Log("coop: %s lost the last point of health (%s) -- the death is %s's; %s is the AI companion meanwhile",
	    HeroName(idx), how, HeroName(idx), HeroName(other));
}

// Gives the dying hero the lead (outside any sliced tick: the trio's bytes are
// the real ones).
void LeadTheDeath() {
	if (g.dying < 0 || g.dyingLeads || g.sliced) return;
	Trio t = GetTrio();
	if (!t) return;
	g.dyingLeads = true;
	if (t.Leader() != g.dying) CoopGiveLead(g.dying, "the death is its");
	int other = OtherHero(g.dying);
	Hero o = t.GetHero(other);
	if (o && HeroModePlayable(*F18(o))) *F18(o) = HeroAiVariant(*F18(o));
}

} // namespace

void CoopGiveLead(int idx, const char* why) {
	Trio t = GetTrio();
	if (!t || g.sliced || t.Leader() == idx) return;
	Hero h = t.GetHero(idx), lead = t.GetHero(t.Leader());
	if (!h || !lead) return;
	// The switch handler refuses a dead leader (+0x390 bit 2) and a target that
	// is not the AI companion (+0x18 bit 0); it moves both heroes one Life state
	// along (leader -> AI, target -> player), so they go in as a player leader
	// and an AI target.
	uint8_t deadBit = *(uint8_t*)(lead.raw() + 0x390) & 4;
	*(uint8_t*)(lead.raw() + 0x390) &= ~4;
	if (HeroModePlayable(*F18(h))) *F18(h) = HeroAiVariant(*F18(h));
	if (HeroModePlayable(*F18(lead))) *F18(lead) = HeroPlayerVariant(*F18(lead));
	int was = t.Leader();
	t.SwitchTo(idx);
	*(uint8_t*)(lead.raw() + 0x390) |= deadBit;
	Log("coop: %s takes the lead from %s (%s) -- leader now %s", HeroName(idx), HeroName(was), why, HeroName(t.Leader()));
}

bool CoopP2ShowsHud() { return g.p2Joined && g.p2BackHeldAt != 0 && !g.p2BackCombo; }

float CoopHoldTime(int player, int action) {
	if (action < 0 || action >= 256) return 0.0f;
	if (player == 1) return g.p2Joined ? g.p2HoldTime[action] : 0.0f;
	InputManager im = GetInputManager();
	if (!im || action >= im.NumActions()) return 0.0f;
	return *(float*)(im.Binding(action) + 0x18);
}

namespace {

// Hand player 2's hero back to the game: make player 1's hero the trio leader
// again if a switch moved leadership, then put player 2's hero on AI, following.
void Release(const char* why) {
	Trio t = GetTrio();
	int p1 = g.p1Hero, p2 = g.p2Hero;
	// In the middle of a scripted sequence (a hero borrowed for a cable car ride,
	// a wait on a platform) the game's own arrangement stands: no switch back, no
	// "follow me" -- player 1 simply drives whoever leads, as in single player.
	bool scripted = (p1 >= 0 && g.held[p1]) || (p2 >= 0 && g.held[p2]);
	ForgetP2();   // first, so our trio-event wrapper stays out of the way
	if (t && p1 >= 0 && p2 >= 0 && scripted) {
		Log("coop: player 2 left (%s) during a scripted sequence; leader stays %s", why, HeroName(t.Leader()));
	} else if (t && p1 >= 0 && p2 >= 0) {
		// The leader stays the level's (single player from here: player 1 drives
		// whoever leads, as the game does after its own switches) -- a lead the mod
		// only lent for something under way goes back first. (It went back to player
		// 1's hero when that one led as player 2 came in: LV5-3, the level's switch
		// to Obelix undone by player 2 leaving.)
		GiveBackLead(t);
		// Single player from here: the leader is player 1's, the other one the AI
		// companion following it.
		int lead = t.Leader(), comp = lead == p1 ? p2 : p1;
		Hero hl = t.GetHero(lead), hc = t.GetHero(comp);
		if (hl && HeroModePlayable(*F18(hl))) *F18(hl) = HeroPlayerVariant(*F18(hl));
		if (hc && HeroModePlayable(*F18(hc))) *F18(hc) = HeroAiVariant(*F18(hc));
		t.CompanionFollow();
		Log("coop: player 2 left (%s); %s is AI again, leader=%s (player 1's now)", why, HeroName(comp),
			HeroName(lead));
	} else {
		Log("coop: player 2 left (%s)", why);
	}
}

// A companion left in a ride mode after the ride (Obelix kept the launcher's
// flight mode, 1, after landing -- and the save kept it): co-op could never
// start again. With the leader in ordinary play, no cutscene, and the companion
// not moving for 5 s, Start puts the companion back into ordinary play (AI).
bool RepairStuckCompanion(Trio t) {
	if (!t || CutsceneBars() || g.cutsceneHold > 0) return false;
	Hero lead = t.GetHero(t.Leader());
	int ci = t.Leader() == HERO_ASTERIX ? HERO_OBELIX : HERO_ASTERIX;
	Hero c = t.GetHero(ci);
	if (!lead || !c || !HeroModePlayable(lead.Flags18()) || lead.Busy() || HeroModePlayable(c.Flags18())) return false;
	if (!g.stuckSince[ci] || GetTickCount() - g.stuckSince[ci] < 5000) return false;
	uint8_t f = c.Flags18();
	*(uint8_t*)(c.raw() + 0x18) = (uint8_t)((f & 0xE0) | 0x03);
	Log("coop: %s was stuck in a ride mode (f18 %02X) with %s in ordinary play -- put back (f18 %02X)", HeroName(ci),
	    f, HeroName(t.Leader()), c.Flags18());
	g.stuckSince[ci] = 0;
	return true;
}

// Track how long each hero has sat still in a non-playable mode (see above).
void TrackStuckModes(Trio t) {
	for (int i = 0; i < 2; i++) {
		Hero h = t ? t.GetHero(i) : Hero();
		if (!h || HeroModePlayable(h.Flags18())) { g.stuckSince[i] = 0; continue; }
		CNode* node = h.hook->node;
		const float* np = node && node->matrix ? (const float*)((char*)node->matrix + 0x80) : nullptr;
		Vector3 p = np ? Vector3(np[0], np[1], np[2]) : h.Pos();
		if (!g.stuckSince[i] || (p - g.stuckPos[i]).len2() > 0.25f) {
			g.stuckSince[i] = GetTickCount();
			g.stuckPos[i] = p;
		}
	}
}

void TryJoin(Trio t) {
	if (g.pendingPad >= 0) {
		if (PadIdentGameEventRecently(0)) g.pendingEcho = true;
		if (g.frameNo - g.pendingFrame < (uint32_t)kJoinSettleFrames) return;
		int pad = g.pendingPad;
		g.pendingPad = -1;
		if (g.pendingEcho || PadIdentRole(pad) == PAD_ROLE_GAME) {
			Log("coop: Start on %s also reached the game -- that is player 1's controller, "
				"player 2 has to use another one", PadName(pad));
			return;
		}
		const char* why = "";
		if (!CoopAllowedNow(t, &why) && RepairStuckCompanion(t)) CoopAllowedNow(t, &why);
		if (!CoopAllowedNow(t, &why)) {
			Log("coop: %s pressed Start but co-op cannot start now (%s)", PadName(pad), why);
			return;
		}
		g.p2Joined = true;
		g.p2Pad    = pad;
		// Player 2 plays Obelix (ini p2_hero), whoever leads right now: after the
		// belly slides the game leads with Obelix, and "the hero player 1 is not
		// driving" handed player 2 Asterix. p2_hero = other keeps that rule.
		if (g_config.p2Hero == HERO_ASTERIX || g_config.p2Hero == HERO_OBELIX) g.p2Hero = g_config.p2Hero;
		else g.p2Hero = OtherHero(t.Leader());
		g.p1Hero = OtherHero(g.p2Hero);
		// Joining in the middle of a scripted sequence (Asterix already seated in
		// the cable car, Obelix leading to pull it): a hero the game has borrowed
		// stays the game's until the sequence is over.
		const int both[2] = { g.p1Hero, g.p2Hero };
		for (int idx : both) {
			Hero h = t.GetHero(idx);
			int order = h ? (int)h.Order() : 0;
			if (h && idx != t.Leader() && !h.IsPlayerControlled() && IsScriptedOrder(order)) {
				g.held[idx] = true;
				g.lastOrder[idx] = (uint8_t)order;
				Log("coop: joining mid-sequence: %s is the game's (AI order %d) until the sequence ends",
					HeroName(idx), order);
			}
		}
		g.p2BackHeldAt = 0;
		g.suspended = false;
		Log("coop: player 2 joined on %s as %s (player 1 keeps %s)", PadName(pad),
			HeroName(g.p2Hero), HeroName(g.p1Hero));
		HintHide(HINT_JOIN);
		g.joinHintPending = false;
		CoopLogState("join");
		return;
	}

	for (int pad = 0; pad < kMaxPads; pad++) {
		const PadState& p = PadGet(pad);
		if (!p.connected || !(p.pressed & PAD_START)) continue;
		if (PadIdentRole(pad) == PAD_ROLE_GAME) {
			Log("coop: Start on %s ignored -- the game reads it, it is player 1's", PadName(pad));
			continue;
		}
		g.pendingPad   = pad;
		g.pendingFrame = g.frameNo;
		g.pendingEcho  = PadIdentGameEventRecently(2);
		return;
	}
}

} // namespace

HookCounters g_hookCounters;

bool CoopP2Joined()  { return g.p2Joined; }
int  CoopP2Hero()    { return g.p2Hero; }
int  CoopNonLeaderPlayer() {
	Trio t = GetTrio();
	if (!t || !g.p2Joined) return -1;
	return t.Leader() == g.p2Hero ? g.p1Hero : g.p2Hero;
}
int  CoopP1Hero()    { return g.p1Hero; }
int  CoopP2Pad()     { return g.p2Pad; }
bool CoopSuspended() { return g.suspended; }
int  CoopShopper() { return g.p2Joined && g.shopping && !g.suspended ? g.shopper : -1; }
int  CoopShopHero() { return g.p2Joined && !g.suspended ? g.shopHero : -1; }
const uint8_t* CoopP2Values() { return g.p2Joined && g.p2ValuesReady ? g.p2Values : nullptr; }
bool CoopOneScreen() { return g.oneScreen; }
// Player 2's input for the game camera (CKSrvCamera +0x580: an input view whose
// actions 0-4 are the right stick up, down, right, left and its button -- the
// game's bindings 46-50 -- which player 2's hero values never carry).
uint8_t* CoopP2CameraValues() {
	if (!g.p2Joined || !g.p2ValuesReady || g.p2Pad < 0) return nullptr;
	CKYellowPages* yp = YellowPages();
	char* cs = yp ? *(char**)((char*)yp + 0x68) : nullptr;
	char* view = cs ? *(char**)(cs + 0x580) : nullptr;
	uint8_t* remap = view ? *(uint8_t**)(view + 8) : nullptr;
	memcpy(g.p2CamValues, g.p2Values, sizeof(g.p2CamValues));
	if (!remap) return g.p2CamValues;
	const PadState& p = PadGet(g.p2Pad);
	auto put = [&](int action, float v) {
		uint8_t b = remap[action];
		if (v > 1.0f) v = 1.0f;
		g.p2CamValues[b] = v > 0.0f ? (uint8_t)(v * 255.0f + 0.5f) : 0;
	};
	put(0, p.ry);  put(1, -p.ry);
	put(2, p.rx);  put(3, -p.rx);
	put(4, (p.pressed & PAD_RTHUMB) ? 1.0f : 0.0f);
	return g.p2CamValues;
}

// Diagnostics: the user saw jump move from A to B mid-session (not reproduced
// yet). Each binding's current source (binding +0xC -> +0x14) and the bindings
// array itself are watched; a change is logged with the whole table.
// The controller preset. CKInput (yellow pages +0x14) keeps the preset in use at
// +0x64; FUN_00438950 (thiscall, the preset) points every binding at it. The game
// picks it in CKAsterixGameManager::vf10 and FUN_00503AA0: 3 (the pad layout) if
// it sees a pad (FUN_00438AB0), else 1 (the keyboard's). The saved options
// (FUN_00503B00) set 1 and then the player's own buttons, or 0 (X/B/Y shuffled,
// menus on the left stick). The game only looks again when the title replays.
// Three sessions were played on a wrong preset until a return to the menu set 3:
// twice on 0, once on 1 chosen while the game saw no pad yet (a slow start: the
// first frame 5 s after the menu came up; force_level skips the title, where the
// game would have looked again). So a level running on 0 gets the game's choice,
// and a level running on 1 that the game chose without a pad gets 3 once the game
// sees one (what the title does next time). 1 chosen with a pad (the options'
// keyboard layout) is left alone. XXL_PRESETNOPAD=1 (autotest preset1): the
// game's next choice counts as made without a pad.
// A fourth session (xxl_coop.pad_preset_issue.log): another slow start, where the
// boot's saved options (vf10(1) -> FUN_00503B00, which sets 1 at 0x503B41 while it
// builds the tables) came right before the menu, and force_level's level started
// before the title's choice (vf10(6), normally a second later). 1 from 0x503B41 is
// that step, never a choice, so a level running on it gets 3 too (autotest presetopts).
typedef void(__fastcall* SetPresetFn)(char* in, void* edx, int preset);
typedef char(__fastcall* HasPadFn)(char* in, void* edx);
const uintptr_t kSetPreset = 0x438950;
const uint8_t   kSetPresetPrologue[6] = { 0x55, 0x8B, 0x6C, 0x24, 0x08, 0x56 };
SetPresetFn g_origSetPreset = nullptr;
bool g_presetWithoutPad = false;   // the game's last choice was 1, made while it saw no pad
bool g_presetOptionsStep = false;  // the last one was the saved options' own 1 (0x503B41)

static bool GameSeesPad(char* in) { return ((HasPadFn)0x438AB0)(in, nullptr) != 0; }

static void SetPreset(char* in, int preset) {
	(g_origSetPreset ? g_origSetPreset : (SetPresetFn)kSetPreset)(in, nullptr, preset);
}

// Any preset choice the hook saw (the boot choice can come before the hook is in).
// The merchant's menu, when it comes up (FUN_004BFAD0), turns the gameplay
// bindings off (FUN_00437F10(CKInput, 0, 0), which also zeroes their values) --
// player 1's controls. With player 2 at the merchant they stay on: that call
// site (0x4BFB1B) comes here.
const uintptr_t kShopMenuInputOff = 0x4BFB1B;
const uint8_t   kShopMenuInputOffBytes[5] = { 0xE8, 0xF0, 0x83, 0xF7, 0xFF };

static int __fastcall ShopMenuInputOff(char* in, void* edx, int group, int on) {
	if (group == 0 && !on && g.p2Joined && g.shopping && g.shopper == g.p2Hero) {
		Log("coop: the merchant's menu is player 2's -- player 1's controls stay on");
		return 1;
	}
	return ((int(__fastcall*)(char*, void*, int, int))0x437F10)(in, edx, group, on);
}

static bool g_presetSeen = false;

// The pad's layout (preset 3, built from the profile's options by the title
// screen: vf10(1) -> FUN_004FF1E0 + FUN_00503B00). A level started straight
// away (force_level) never gets it -- the same calls there give a default table
// with the face buttons shifted -- so the title's is kept in a file and put
// back. Each binding's source (+0xC) holds a pointer per preset (+4..+0x10)
// into the input's devices: A (+4, 0xCC each), B (+8, 0x68), C (+0xC, 0x3FC).
struct PadDev { char* base; int count, stride, release; };
static void PadDevices(char* in, PadDev d[3]) {
	d[0] = { *(char**)(in + 0x04), *(uint8_t*)(in + 0x10), 0xCC, 0x18 };
	d[1] = { *(char**)(in + 0x08), *(uint8_t*)(in + 0x11), 0x68, 0x0E };
	d[2] = { *(char**)(in + 0x0C), *(uint8_t*)(in + 0x12), 0x3FC, 0x90 };
}

static void SavePadLayout(char* in) {
	InputManager im = GetInputManager();
	if (!im || im.raw() != in) return;
	PadDev d[3];
	PadDevices(in, d);
	char path[MAX_PATH];
	GameDirPath(path, sizeof(path), "xxl_coop_padlayout.txt");
	FILE* f = fopen(path, "w");
	if (!f) return;
	int n = im.NumActions(), saved = 0;
	fprintf(f, "# player 1's pad layout as the title screen built it (preset 3), put back when a level starts without it\n");
	for (int i = 0; i < n; i++) {
		char* b = im.Binding(i);
		char* src = b ? *(char**)(b + 0xC) : nullptr;
		char* p = src ? *(char**)(src + 0x10) : nullptr;
		if (!p) continue;
		for (int k = 0; k < 3; k++)
			if (d[k].base && p >= d[k].base && p < d[k].base + d[k].count * d[k].stride) {
				int off = (int)(p - d[k].base);
				fprintf(f, "%d %d %d %d\n", i, k, off / d[k].stride, off % d[k].stride);
				saved++;
				break;
			}
	}
	fclose(f);
	Log("input: the pad's layout from the title screen kept (%d bindings, xxl_coop_padlayout.txt)", saved);
}

static bool RestorePadLayout(char* in) {
	InputManager im = GetInputManager();
	if (!im || im.raw() != in) return false;
	char path[MAX_PATH];
	GameDirPath(path, sizeof(path), "xxl_coop_padlayout.txt");
	FILE* f = fopen(path, "r");
	if (!f) return false;
	PadDev d[3];
	PadDevices(in, d);
	int n = im.NumActions(), put = 0;
	char line[128];
	while (fgets(line, sizeof(line), f)) {
		int i, k, idx, off;
		if (line[0] == '#' || sscanf(line, "%d %d %d %d", &i, &k, &idx, &off) != 4) continue;
		if (i < 0 || i >= n || k < 0 || k > 2 || !d[k].base || idx < 0 || idx >= d[k].count || off < 0 || off >= d[k].stride) continue;
		char* b = im.Binding(i);
		char* src = b ? *(char**)(b + 0xC) : nullptr;
		if (!src) continue;
		// The value's pointer for preset 3, and its "previous value" pointer
		// (+0x18 + preset * 4: the same byte one block on, as the binder sets them,
		// CKServiceManager::vf62) -- left empty, the first frame waiting for a
		// release reads through null (xxl_coop.crash_question_mark.log).
		char* p = d[k].base + idx * d[k].stride + off;
		*(char**)(src + 0x10) = p;
		*(char**)(src + 0x24) = p + d[k].release;
		put++;
	}
	fclose(f);
	Log("input: the pad's layout from the title screen put back (%d bindings)", put);
	return put > 0;
}

static void __fastcall SetPresetHook(char* in, void* edx, int preset) {
	static bool fakeNoPad = getenv("XXL_PRESETNOPAD") && atoi(getenv("XXL_PRESETNOPAD"));
	bool pad = GameSeesPad(in) && !fakeNoPad;
	g_presetWithoutPad = preset == 1 && !pad;
	g_presetSeen = true;
	void* from = _ReturnAddress();
	g_presetOptionsStep = preset == 1 && (uintptr_t)from == 0x503B41;
	Log("input: the game sets controller preset %d (from %p; it sees %s)", preset, from, pad ? "a pad" : "no pad");
	g_origSetPreset(in, edx, preset);
	// The title's own choice with a pad (FUN_00503AA0): its layout is the right one.
	if (preset == 3 && pad && (uintptr_t)from == 0x503AED) SavePadLayout(in);
}

static void WatchControllerPreset() {
	static int lastLevel = -99;
	static DWORD lastCheck = 0;
	DWORD now = GetTickCount();
	CKYellowPages* yp = YellowPages();
	char* in = yp ? *(char**)((char*)yp + 0x14) : nullptr;
	if (!in) return;
	int preset = *(int*)(in + 0x64);
	if (g.lastLevel != lastLevel) {
		lastLevel = g.lastLevel;
		Log("input: level %d, controller preset %d", g.lastLevel, preset);
	}
	if (g.lastLevel <= 0 || now - lastCheck < 1000) return;
	lastCheck = now;
	if (preset == 1 && (g_presetWithoutPad || g_presetOptionsStep || !g_presetSeen) && GameSeesPad(in)) {
		// The pad's own layout goes into the presets at boot, when the game sees a
		// pad there (CKAsterixGameManager::vf10(1) -> FUN_00503B00, thiscall(game
		// manager), done: +0x5BC = 1); the title then picks 3. With no pad seen at
		// boot it was never built, and preset 3 alone was a default layout with the
		// face buttons shifted (xxl_coop.its_back.log): build it first.
		char* gm = yp ? (char*)yp->gameManager : nullptr;
		if (gm && *(int*)(gm + 0x5BC) == 0) {
			((void(__fastcall*)(char*, void*))0x503B00)(gm, nullptr);
			Log("input: the pad's layout was never built (no pad at boot) -- built now (FUN_00503B00)");
		}
		// The saved options outside the title build preset 3 as the default table
		// (face buttons shifted, autotest presetopts), so the title's always goes back.
		RestorePadLayout(in);
		SetPreset(in, 3);
		Log("input: level %d is running on the keyboard preset 1, which the game chose %s; it sees a pad now -- "
		    "preset 3 set (the game's choice with a pad)", g.lastLevel,
		    g_presetWithoutPad ? "while it saw no pad" :
		    g_presetOptionsStep ? "as a step of the saved options (FUN_00503B00), with no choice after it" :
		    "before the mod could see it (at boot)");
		g_presetWithoutPad = false;
		g_presetOptionsStep = false;
		g_presetSeen = true;
		return;
	}
	if (preset != 0) return;
	int want = GameSeesPad(in) ? 3 : 1;
	SetPreset(in, want);
	Log("input: level %d is running on the unset controller preset 0; preset %d set (the game's own choice)", g.lastLevel, want);
}

void WatchBindings() {
	static char* lastArray = nullptr;
	static char* lastSrc[256];
	static int lastN = -1;
	static DWORD lastCheck = 0;
	DWORD now = GetTickCount();
	if (now - lastCheck < 500) return;
	lastCheck = now;
	InputManager im = GetInputManager();
	if (!im) return;
	char* arr = *(char**)(im.raw() + 0x18);
	int n = im.NumActions();
	if (!arr || n <= 0 || n > 256) return;
	bool changed = arr != lastArray || n != lastN;
	int first = -1;
	for (int i = 0; i < n; i++) {
		char* src = *(char**)(arr + 0x20 * i + 0xC);
		char* cur = src ? *(char**)(src + 0x14) : nullptr;
		if (lastN == n && cur != lastSrc[i] && first < 0) first = i;
		lastSrc[i] = cur;
	}
	if (lastN >= 0 && (changed || first >= 0)) {
		Log("input: the game's bindings changed (array %p -> %p, %d actions%s, first changed source: action %d)",
		    lastArray, arr, n, g.swapped ? ", player 2's copy swapped in!" : "", first);
		TraceDumpBindings();
	}
	lastArray = arr;
	lastN = n;
}

bool CoopHeroHeld(int idx) { return g.p2Joined && idx >= 0 && idx < HERO_COUNT && g.held[idx]; }

void CoopRequestLeave(const char* why) {
	if (g.p2Joined) Release(why);
}

bool CoopGameplayLive() {
	if (!KalLevelIsLoaded()) return false;
	Trio t = GetTrio();
	if (!t) return false;
	Hero leader = t.GetHero(t.Leader());
	return leader.Exists() && leader.IsPlayerControlled();
}

void CoopLogState(const char* tag) {
	Trio t = GetTrio();
	if (!t) { Log("state[%s]: no trio", tag); return; }
	for (int i = 0; i < HERO_COUNT; i++) {
		Hero h = t.GetHero(i);
		if (!h) continue;
		uint8_t f = h.Flags18();
		Log("state[%s]: %-7s f18=%02X mode=%s %s life=%02X sit=%02X body=%02X ai=%02X order=%d "
			"f1A=%02X busy=%d pos=(%.2f %.2f %.2f)%s",
			tag, HeroName(i), f, ModeName(HeroMode(f)), HeroIsPlayerVariant(f) ? "PLAYER" : "ai",
			LifeLastState(i), *(uint8_t*)(h.raw() + 0x3E9), *(uint8_t*)(h.raw() + 0xE5),
			*(uint8_t*)(h.raw() + 0x561), (int)h.Order(), *(uint8_t*)(h.raw() + 0x1A), h.Busy(),
			h.Pos().x, h.Pos().y, h.Pos().z, t.Leader() == i ? " LEADER" : "");
	}
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

// Marker for capturing what the game does in situations that cannot be staged by
// the autotest (cutscenes...): LB+RB together on any pad logs a burst of engine
// state for four seconds.
DWORD g_probeUntil = 0;
int   g_markNo = 0;

void ProbeLine() {
	char buf[1024];
	LineBuf line(buf);
	__try {
		CKYellowPages* yp = YellowPages();
		Trio t = GetTrio();
		for (int i = HERO_ASTERIX; i <= HERO_OBELIX; i++) {
			Hero h = t ? t.GetHero(i) : Hero();
			if (!h) continue;
			line.Add("%c f18=%02X L=%02X sit=%02X body=%02X 1A=%02X 390=%08X 3EC=%08X 3F0=%08X 38C=%08X "
				"464=%p 884=%08X ord=%d pos=(%.1f %.1f %.1f) ",
				i == HERO_ASTERIX ? 'A' : 'O', h.Flags18(), LifeLastState(i), Situation(h),
				*(uint8_t*)(h.raw() + 0xE5), *(uint8_t*)(h.raw() + 0x1A), h.Flags390(),
				*(uint32_t*)(h.raw() + 0x3EC), *(uint32_t*)(h.raw() + 0x3F0), *(uint32_t*)(h.raw() + 0x38C),
				*(void**)(h.raw() + 0x464), h.Flags884(), (int)h.Order(), h.Pos().x, h.Pos().y, h.Pos().z);
		}
		char* cin = *(char**)((char*)yp + 0x80);
		char** scenes = cin ? *(char***)(cin + 0x10) : nullptr;
		unsigned n = cin ? *(unsigned*)(cin + 0x14) : 0;
		line.Add("| bars=%d cin:", cin ? (*(uint8_t*)(cin + 0xC) & 1) : -1);
		for (unsigned i = 0, shown = 0; scenes && i < n && i < 64 && shown < 8; i++) {
			unsigned f20 = *(unsigned short*)(scenes[i] + 0x20), f30 = *(uint8_t*)(scenes[i] + 0x30);
			if ((f20 & 0x8001) || f30) { line.Add(" %u:%04X/%02X", i, f20, f30); shown++; }
		}
		char* cam = *(char**)((char*)yp + 0x68);
		char* track = cam ? *(char**)(cam + 0x38) : nullptr;
		char* gm = (char*)yp->gameManager;
		char* inp = *(char**)((char*)yp + 0x14);
		line.Add(" | cam trk=%08X fl=%08X | gm5C=%08X", track ? *(unsigned*)track : 0,
			cam ? *(unsigned*)(cam + 0xC) : 0, gm ? *(unsigned*)(gm + 0x5C) : 0);
		if (inp) {
			unsigned mask = 0;
			char* binds = *(char**)(inp + 0x18);
			for (int i = 0; i < 16 && binds; i++) if (binds[i * 0x20 + 4] == 1) mask |= 1u << i;
			uint8_t* v = *(uint8_t**)(inp + 0x1C);
			line.Add(" | bind=%04X v0-7=%02X%02X%02X%02X%02X%02X%02X%02X", mask,
				v ? v[0] : 0, v ? v[1] : 0, v ? v[2] : 0, v ? v[3] : 0, v ? v[4] : 0, v ? v[5] : 0,
				v ? v[6] : 0, v ? v[7] : 0);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		line.Add(" (probe fault)");
	}
	Log("probe: %s", line.c_str());
}

void CheckMarker(DWORD now) {
	for (int i = 0; i < kMaxPads; i++) {
		const PadState& p = PadGet(i);
		if (p.connected && (p.pressed & (PAD_LB | PAD_RB)) && (p.buttons & PAD_LB) && (p.buttons & PAD_RB)) {
			Log("probe: ===== MARK %d (LB+RB on pad %d) =====", ++g_markNo, i);
			g_probeUntil = now + 4000;
			break;
		}
	}
	static DWORD lastProbe = 0;
	if ((int)(g_probeUntil - now) > 0 && (int)(now - lastProbe) >= 250) {
		lastProbe = now;
		ProbeLine();
	}
}

static void SetHeroPosition(Hero h, const Vector3& to);
static void TwisterNearness(Trio t, Hero h1, Hero h2);
static bool FusionOn(Hero h1, Hero h2);

// The level streams one sector at a time besides sector 0 (CKLevel, yellow
// pages +0x90: sectors at +4, current +0x24, loading +0x2C, requested +0x34).
// Each sector lists Sas (sector +0x10, count +0x14; CKSas: two boxes at the
// border, +0xC and +0x24 -- in the first, sector +4 is wanted, in the second
// sector +8), checked against the trio leader's position only (CKLevel::vf17
// -> FUN_00410DA0, starting from the current sector's list). Zone events switch
// too (CKLevel 0x1222: level 2's rectangles #75/#76), for either hero. Sector
// boxes overlap too much to tell where a hero is, so each player's hero gets
// the leader's treatment: its own sector, moved by the Sas boxes it walks
// through. A switch goes to the hero that made it -- the leader by a Sas, or
// the hero whose zone fired -- and a player's hero whose sector is not the
// streamed one (outside any Sas box) for a second (ini sector_catch_up_ms) is
// brought to the other.
// Only a hero the streaming *left behind* (the sector switched away from it) is
// brought over at once. A hero that walks through a Sas box towards a sector
// that is not streamed (player 2's Obelix heading for the rectangle that
// switches the streaming over to him) is left alone -- the level between
// sectors is always there -- unless it falls (the floor it walked onto is not
// loaded).
struct SectorTrack {
	int   level = -99;
	int   lastCur = -2;
	int   sector[HERO_COUNT] = {};
	bool  leftBehind[HERO_COUNT] = {};
	float outsideY[HERO_COUNT] = {};
	int   zoneHero = -1;
	DWORD zoneAt = 0;
} g_sec;
const float kFallBelow = 8.0f;   // units below where it left the streamed sector

void CoopNoteStoryZone(int heroIndex) {
	g.storyZoneHero = heroIndex;
	g.storyZoneAt = GetTickCount();
}

void CoopNoteBossRun(int heroIndex) {
	if (g.bossRunHero < 0) g.bossRunHero = heroIndex;
}

void CoopNoteZoneHero(int heroIndex) {
	g_sec.zoneHero = heroIndex;
	g_sec.zoneAt = GetTickCount();
}

static void WatchSectors(Trio t, DWORD now) {
	CKYellowPages* yp = YellowPages();
	char* lvl = yp ? *(char**)((char*)yp + 0x90) : nullptr;
	int cur = lvl ? *(int*)(lvl + 0x24) : -2;
	char** secs = lvl ? *(char***)(lvl + 4) : nullptr;
	if (!t || !secs || cur <= 0 || !secs[cur]) return;
	int nsec = 0;
	while (nsec < 64 && secs[nsec] && *(void**)secs[nsec] == *(void**)secs[0]) nsec++;
	if (cur >= nsec) return;
	int idx[2] = { HERO_ASTERIX, HERO_OBELIX };   // the two player heroes, joined or not
	if (g.lastLevel != g_sec.level) {   // new level (or first look): everyone where the level is
		g_sec.level = g.lastLevel;
		g_sec.lastCur = cur;
		for (int i = 0; i < HERO_COUNT; i++) { g_sec.sector[i] = cur; g_sec.leftBehind[i] = false; }
		memset(g.outsideSince, 0, sizeof(g.outsideSince));
		return;
	}
	// Sas boxes, as CKLevel::vf17 checks them, for each player's own sector.
	bool inSas[HERO_COUNT] = {};
	for (int i : idx) {
		Hero h = t.GetHero(i);
		int s = g_sec.sector[i];
		if (!h || s <= 0 || s >= nsec) { g_sec.sector[i] = cur; continue; }
		Vector3 p = h.Pos();
		char** sas = *(char***)(secs[s] + 0x10);
		unsigned ns = *(unsigned*)(secs[s] + 0x14);
		for (unsigned k = 0; sas && k < ns && k < 64; k++) {
			const float* b = (const float*)(sas[k] + 0xC);
			for (int half = 0; half < 2; half++, b += 6) {
				if (p.x <= b[0] && p.x > b[3] && p.y <= b[1] && p.y > b[4] && p.z <= b[2] && p.z > b[5]) {
					int to = *(int*)(sas[k] + (half ? 8 : 4));
					inSas[i] = true;
					if (to > 0 && to < nsec) g_sec.sector[i] = to;
					k = ns;
					break;
				}
			}
		}
	}
	// A completed switch: whose was it?
	if (cur != g_sec.lastCur) {
		int L = t.Leader();
		if (L < 0 || L >= HERO_COUNT) L = idx[0];
		bool zoneRecent = g_sec.zoneHero >= 0 && (int)(now - g_sec.zoneAt) < 5000;
		const char* why;
		if (g_sec.sector[L] == cur) why = "the leader's Sas";
		else if (zoneRecent) { g_sec.sector[g_sec.zoneHero] = cur; why = "a zone"; }
		else { for (int i : idx) g_sec.sector[i] = cur; why = "unknown, everyone moved along"; }
		Log("coop: sector %d -> %d (%s%s%s); Asterix's sector %d, Obelix's %d", g_sec.lastCur, cur, why,
		    zoneRecent && g_sec.sector[L] != cur ? " of " : "", zoneRecent && g_sec.sector[L] != cur ? HeroName(g_sec.zoneHero) : "",
		    g_sec.sector[HERO_ASTERIX], g_sec.sector[HERO_OBELIX]);
		for (int i : idx) g_sec.leftBehind[i] = g_sec.sector[i] != cur;
		g_sec.lastCur = cur;
		// The streaming moved on from a player at the merchant (the other walked
		// through a Sas): the visit ends, and the catch-up brings the shopper over.
		if (g.p2Joined && g.shopping && !g.shopForcedClosed && g.shopper >= 0 && g.shopper < HERO_COUNT &&
		    g_sec.leftBehind[g.shopper])
			CloseShopFor("the other player's sector change");
		g_sec.zoneHero = -1;
	}
	// Heroes together are in the same place: whatever the game did to put them
	// there (warp, respawn, its own teleports).
	Hero h1 = t.GetHero(idx[0]), h2 = t.GetHero(idx[1]);
	if (h1 && h2 && (h1.Pos() - h2.Pos()).len2() < 8.0f * 8.0f &&
	    (g_sec.sector[idx[0]] == cur || g_sec.sector[idx[1]] == cur))
		g_sec.sector[idx[0]] = g_sec.sector[idx[1]] = cur;
	for (int i : idx) if (g_sec.sector[i] == cur) g_sec.leftBehind[i] = false;

	bool settled = *(int*)(lvl + 0x2C) == -2 && *(int*)(lvl + 0x34) == -2;
	if (!settled || !g.p2Joined || g.suspended || g.held[HERO_ASTERIX] || g.held[HERO_OBELIX]) {
		memset(g.outsideSince, 0, sizeof(g.outsideSince));
		return;
	}
	for (int k = 0; k < 2; k++) {
		int i = idx[k], o = idx[1 - k];
		Hero h = t.GetHero(i), other = t.GetHero(o);
		if (!h || !other || g_sec.sector[i] == cur || inSas[i]) { g.outsideSince[i] = 0; continue; }
		if (!g.outsideSince[i]) { g.outsideSince[i] = now; g_sec.outsideY[i] = h.Pos().y; continue; }
		bool falling = h.Pos().y < g_sec.outsideY[i] - kFallBelow;
		if ((!g_sec.leftBehind[i] && !falling) || (int)(now - g.outsideSince[i]) < g_config.sectorCatchUpMs ||
		    g_sec.sector[o] != cur || !HeroModePlayable(other.Flags18()))
			continue;
		g.outsideSince[i] = 0;
		Vector3 from = h.Pos(), to = other.Pos();
		to.y += 1.0f;
		SetHeroPosition(h, to);
		Log("coop: %s %s sector %d (at %.0f %.0f %.0f), sector %d is streamed: brought to %s at (%.1f %.1f %.1f)",
		    HeroName(i), falling && !g_sec.leftBehind[i] ? "fell out of the streamed level into" : "left in",
		    g_sec.sector[i], from.x, from.y, from.z, cur, HeroName(o), to.x, to.y, to.z);
		g_sec.sector[i] = cur;
		g_sec.leftBehind[i] = false;
	}
}

// Cutscenes (CKCinematicScene, 12,37; listed by the cinematic service, yellow
// pages +0x80, +0x10 / count +0x14). CKSrvCinematic::vf17 ends a playing scene
// (scene +0x20 bit 0) the game's own way (FUN_00461A80: every running bloc
// finished) when its input view's action 0 -- Enter -- is pressed, if the scene
// has flag 0x1000 ("end on pressing Enter") and not 0x180 == 0x80. Level 1's
// long intro (scene 1, flags 0x4F16) has no 0x1000. [debug] skip_cutscenes
// gives it to every scene.
static void MakeCutscenesSkippable() {
	if (!g_config.skipCutscenes) return;
	CKYellowPages* yp = YellowPages();
	char* cin = yp ? *(char**)((char*)yp + 0x80) : nullptr;
	char** scenes = cin ? *(char***)(cin + 0x10) : nullptr;
	unsigned n = cin ? *(unsigned*)(cin + 0x14) : 0;
	static char** doneFor = nullptr;
	static unsigned doneN = 0;
	if (!scenes || (scenes == doneFor && n == doneN)) return;
	doneFor = scenes;
	doneN = n;
	unsigned changed = 0;
	for (unsigned i = 0; i < n && i < 512; i++) {
		uint16_t& f = *(uint16_t*)(scenes[i] + 0x20);
		if ((f & 0x1000) == 0 && (f & 0x180) != 0x80) { f |= 0x1000; changed++; }
	}
	Log("coop: skip_cutscenes: %u of %u cutscenes now end on Enter", changed, n);
}

// A skipped cutscene (Enter: CKSrvCinematic::vf17 -> FUN_00461A80, which drops
// the scene's running nodes and fades it out) lost what its later nodes would
// have done: their level events (CKStartEventCinematicBloc, event id at +0x2C,
// flagged when it starts -- level 1's music after the intro, LV2-10's boss gate
// that brings the hero left behind). The scene's graph (CKCinematicNode: +4 next
// running, +8/+0xA/+0xC edges into the scene's +4 list: [+8, +0xA) started and
// [+0xA, ...) stopped when the node ends, +0x14 flags (1 started, 4 done, 0x40
// its edges count; a path or camera bloc runs off the list), +0x18 group,
// doors' inputs +0x20 of +0x24) is run on from its running nodes without time
// first: each StartEvent bloc reached has its event flagged, in order (a
// stopped one, as the game's stop, does not).
typedef void(__fastcall* SkipSceneFn)(char* scene, void* edx);
const uintptr_t kSkipScene = 0x461A80;
const uint8_t   kSkipScenePrologue[6] = { 0x57, 0x8B, 0xF9, 0x8A, 0x47, 0x30 };
const uintptr_t kMenuSkipReturn = 0x524359;   // CKHkInterfaceMain::vf18 (the menus' scene): left as it was
SkipSceneFn g_origSkipScene = nullptr;

static void RunOnSkippedScene(char* sc) {
	const uintptr_t kAnd = 0x62495C, kOr = 0x6249A4, kRand = 0x6249EC, kStartDoor = 0x624A34;
	const uintptr_t kGroup = 0x624B54, kStreamGroup = 0x624F48, kStartEvent = 0x624E70;
	char** edges = *(char***)(sc + 4);
	unsigned ne = *(unsigned*)(sc + 8);
	CKYellowPages* yp = YellowPages();
	char* evs = yp ? *(char**)((char*)yp + 0x6C) : nullptr;
	if (!edges || !evs) return;
	const unsigned kMax = 512;
	static char*   node[kMax];
	static uint8_t st[kMax];     // 1 started, 2 ended, 4 its event flagged
	static int     inputs[kMax];
	unsigned n = 0;
	auto vt = [](char* x) { return *(uintptr_t*)x; };
	auto isDoor = [&](char* x) { uintptr_t v = vt(x); return v == kAnd || v == kOr || v == kRand || v == kStartDoor; };
	auto isGroup = [&](char* x) { return vt(x) == kGroup || vt(x) == kStreamGroup; };
	auto slot = [&](char* x) -> int {
		for (unsigned i = 0; i < n; i++) if (node[i] == x) return (int)i;
		if (n >= kMax) return -1;
		node[n] = x;
		st[n] = 0;
		inputs[n] = isDoor(x) ? *(int*)(x + 0x20) : 0;
		return (int)n++;
	};
	struct Op { char* x; int op; };   // 0 reached, 1 ended, 2 stopped
	const unsigned kOps = 2048;
	static Op q[kOps];
	unsigned qh = 0, qt = 0;
	auto push = [&](char* x, int op) { if (x && qt < kOps) q[qt++] = { x, op }; };
	static const bool trace = getenv("XXL_SKIPLOG") != nullptr;   // diagnostics: every step of the run-on
	// Running: on the scene's list, or started and not done off it (a path or a
	// camera bloc waiting for its actor: LV1's walk to Obelix led to the music).
	static char* run[kMax];
	unsigned nRun = 0;
	for (char* x = *(char**)(sc + 0xC); x && nRun < kMax; x = *(char**)(x + 4)) run[nRun++] = x;
	static const unsigned kNodeIds[] = { 1, 2, 11, 17, 9, 22, 5, 7, 27, 23, 15, 16, 12, 4, 32, 3, 28, 20, 25, 13, 10, 14, 26 };
	for (unsigned id : kNodeIds) {
		for (unsigned k = 0, m = GetNumLvlClassInstances(8, id); k < m && k < 4096 && nRun < kMax; k++) {
			char* x = (char*)GetLvlObject(8, id, k);
			if (!x || *(char**)(x + 0x1C) != sc || (*(uint32_t*)(x + 0x14) & 5) != 1) continue;
			bool listed = false;
			for (unsigned r = 0; r < nRun && !listed; r++) listed = run[r] == x;
			if (!listed) run[nRun++] = x;
		}
	}
	for (unsigned r = 0; r < nRun; r++) {
		char* x = run[r];
		int i = slot(x);
		if (i < 0) continue;
		st[i] |= 1;
		if (trace) Log("coop:   skip: running %p vt %06X flags %08X", x, (unsigned)vt(x), *(uint32_t*)(x + 0x14));
		if (isDoor(x)) {
			uintptr_t v = vt(x);
			if (v != kAnd || inputs[i] >= *(int*)(x + 0x24)) push(x, 1);
		} else if (isGroup(x)) {
			char* second = *(char**)(x + 0x38);
			if (second && (*(uint32_t*)(second + 0x14) & 4)) push(x, 1);
		} else {
			if (vt(x) == kStartEvent) push(x, 0);   // waiting for its line, or not ticked yet: its event
			else push(x, 1);
		}
	}
	char fired[160] = "";
	unsigned nFired = 0, nOps = 0;
	while (qh < qt) {
		Op o = q[qh++];
		int i = slot(o.x);
		if (i < 0) continue;
		nOps++;
		uintptr_t v = vt(o.x);
		if (trace && nOps < 400) Log("coop:   skip: %s %p vt %06X state %d", o.op == 0 ? "reach" : o.op == 1 ? "end" : "stop", o.x, (unsigned)v, st[i]);
		if (o.op == 0) {   // reached (started by an edge)
			if (st[i] & 2) continue;   // (each node ends once here: no loops)
			if (isDoor(o.x)) {   // a door runs again whenever reached (its +0x14 bit 2 stays from the level's load)
				st[i] |= 1;
				inputs[i]++;
				if (v == kOr || v == kRand || inputs[i] >= *(int*)(o.x + 0x24)) push(o.x, 1);
				continue;
			}
			if ((st[i] & 1) && vt(o.x) != kStartEvent) continue;   // running already: it ends from the seed
			bool once = (*(uint32_t*)(o.x + 0x14) & 0x84) == 0x84;   // done before, and only once: ends at once
			st[i] |= 1;
			if (once) {
				push(o.x, 1);
			} else if (v == kStartEvent) {
				uint16_t ev = *(uint16_t*)(o.x + 0x2C);
				if ((ev >> 3) != 0x1FFF && !(st[i] & 4)) {
					st[i] |= 4;
					((void(__fastcall*)(char*, void*, unsigned))0x460C20)(evs, nullptr, ev);
					if (strlen(fired) + 6 < sizeof(fired)) sprintf(fired + strlen(fired), " %04X", ev);
					nFired++;
				}
				push(o.x, 1);
			} else if (isGroup(o.x)) {
				push(*(char**)(o.x + 0x34), 0);   // its first inner node; it ends with its second
			} else {
				push(o.x, 1);
			}
			continue;
		}
		if (st[i] & 2) continue;
		if (o.op == 2 && isDoor(o.x)) continue;   // (a door's stop does nothing)
		if (o.op == 2) {   // stopped: no effect of its own (a group's running inner nodes end quietly)
			st[i] |= 1;
			if (isGroup(o.x)) {
				for (unsigned k = 0; k < n; k++)
					if (*(char**)(node[k] + 0x18) == o.x && (st[k] & 1)) st[k] |= 2;
			}
		}
		st[i] |= 2;
		// Its edges (a random door's own end follows none, as the game's)
		uint32_t f = *(uint32_t*)(o.x + 0x14);
		char* grp = *(char**)(o.x + 0x18);
		int gi = grp ? slot(grp) : -1;
		bool counts = v != kRand && ((f & 0x40) || (gi >= 0 && (st[gi] & 1)));
		uint16_t s0 = *(uint16_t*)(o.x + 8), f0 = *(uint16_t*)(o.x + 0xA), cnt = *(uint16_t*)(o.x + 0xC);
		if (trace && nOps < 400) Log("coop:   skip:   edges %04X/%04X/%u flags %08X group %p: %s", s0, f0, cnt, f, grp, counts ? "followed" : "not followed");
		if (counts) {
			unsigned n1 = s0 == 0xFFFF ? 0 : f0 == 0xFFFF ? cnt : (unsigned)(f0 - s0);
			unsigned n2 = f0 == 0xFFFF ? 0 : s0 == 0xFFFF ? cnt : (unsigned)(cnt - (f0 - s0));
			for (unsigned e = 0; e < n1 && s0 + e < ne; e++) push(edges[s0 + e], 0);
			for (unsigned e = 0; e < n2 && f0 + e < ne; e++) push(edges[f0 + e], 2);
		}
		// The group it is the end of ends with it.
		if (grp && gi >= 0 && *(char**)(grp + 0x38) == o.x && !(st[gi] & 2)) push(grp, 1);
	}
	static int s_told = 0;
	if (s_told < 40) {
		s_told++;
		Log("coop: cutscene %p skipped: run on without time (%u nodes, %u steps)%s%s", sc, n, nOps,
		    nFired ? " -- its later events flagged:" : " -- no later events", fired);
	}
}

// Level 0 (the game's start) is the exception: its intro (scene 0) runs into the
// opening film (scene 1), which brings the title -- the logo, "Press Validate" --
// and later flies on over Gaul (the title music stopped, game manager 0x530A).
// Run on, a skip went past the title at once: no logo. There a skip ends the
// scene the game's way (scene 1's end starts scene 3, the title), and one skip
// of the intro (scene 0) skips the film too as it starts.
static DWORD g_level0SkipAt = 0;   // the intro skipped: the film that follows is skipped too
static char* g_level0Skipped = nullptr;

static int CurrentLevelNow() {
	CKYellowPages* yp = YellowPages();
	return (yp && yp->gameManager) ? *(int*)((char*)yp->gameManager + ax::kGM_currentLevel) : -1;
}

static void __fastcall SkipSceneHook(char* sc, void* edx) {
	uintptr_t ret = (uintptr_t)_ReturnAddress();
	if (CurrentLevelNow() == 0) {
		CKYellowPages* yp = YellowPages();
		char* cin = yp ? *(char**)((char*)yp + 0x80) : nullptr;
		char** scenes = cin && *(unsigned*)(cin + 0x14) ? *(char***)(cin + 0x10) : nullptr;
		bool intro = scenes && scenes[0] == sc;   // (scene 1, the film, brings the title: no chain from it)
		if (ret != kMenuSkipReturn && intro && (*(uint16_t*)(sc + 0x20) & 1) && *(int8_t*)(sc + 0x30) >= 0) {
			g_level0SkipAt = GetTickCount();
			g_level0Skipped = sc;
		}
		if (ret != kMenuSkipReturn)
			Log("coop: level 0's cutscene %p skipped the game's way%s", sc, g_level0Skipped == sc && g_level0SkipAt ? " (the film after it too)" : "");
		g_origSkipScene(sc, edx);
		return;
	}
	// (A scene of kind 0x80 -- level 1's scene 0, behind the others -- is never skipped by Enter.)
	uint16_t f = sc ? *(uint16_t*)(sc + 0x20) : 0;
	if (ret != kMenuSkipReturn && (f & 1) && (f & 0x180) != 0x80 && *(int8_t*)(sc + 0x30) >= 0) {
		__try {
			RunOnSkippedScene(sc);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			Log("coop: the skipped cutscene's run-on failed (exception)");
		}
	}
	g_origSkipScene(sc, edx);
}

// A Roman agent's dialogue (CKHkClueMan, trio event 0x5120, parameter: +4 the
// spot to walk to, then directions and a second point, 0x34 bytes) has the
// trio walk the *leader* there (situation 09); the dialogue starts when the
// walk ends there. Set off by player 2's Obelix with Asterix far behind, the
// walk jumped into the water on the way, Asterix came back playable, and the
// spy never talked -- walking him there by hand does not end the walk. When
// the walk stops getting closer, or drops out before arriving, the leader is
// put on the spot and the event sent again.
struct SpyWalk {
	bool    on = false;
	bool    resending = false;
	int     tries = 0;
	DWORD   since = 0, lastProgress = 0;
	float   best = 0;
	void*   trio = nullptr;
	uint8_t param[0x34] = {};
} g_spyWalk;

const uintptr_t kFixTrackVtbl = 0x6244EC, kPlainCameraVtbl = 0x6245BC;
void* g_trackBeforeSpy = nullptr;
static void WatchSpyCameras();
void WatchBindings();

static void WatchSpyWalk(Trio t, DWORD now) {
	WatchSpyCameras();
	SpyWalk& w = g_spyWalk;
	if (!w.on) return;
	if (!g.p2Joined || !t || (int)(now - w.since) > 30000) { w.on = false; return; }
	Hero lead = t.GetHero(t.Leader());
	if (!lead || g.held[t.Leader()]) return;
	const float* spot = (const float*)(w.param + 4);
	Vector3 to(spot[0], spot[1], spot[2]);
	float d = sqrtf((lead.Pos() - to).len2());
	if (d < 3.0f) { w.on = false; return; }
	if (d < w.best - 0.25f) { w.best = d; w.lastProgress = now; }
	uint8_t sit = Situation(lead), f = lead.Flags18();
	bool walking = sit == 0x09;
	bool dropped = !walking && HeroModePlayable(f) && (int)(now - w.since) > 1000;
	bool stuck = walking && (int)(now - w.lastProgress) > 2500;
	if (!dropped && !stuck) return;
	if (w.tries >= 2) { w.on = false; return; }
	w.tries++;
	Vector3 dst = to;
	dst.y += 0.5f;
	SetHeroPosition(lead, dst);
	Log("coop: %s's walk to the Roman agent %s %.0f units short; put on the spot (%.1f %.1f %.1f), dialogue resent",
	    HeroName(t.Leader()), stuck ? "stuck" : "dropped", d, dst.x, dst.y, dst.z);
	typedef int(__fastcall* SendFn)(void* self, void* edx, int event, void* param);
	uint8_t copy[sizeof(w.param)];
	memcpy(copy, w.param, sizeof(copy));
	w.resending = true;
	((SendFn)(*(void***)w.trio)[5])(w.trio, nullptr, 0x5120, copy);
	w.resending = false;

	w.since = w.lastProgress = now;
	w.best = 1e30f;
}

// The spy saves the camera service's track (+0x38) when its dialogue starts
// (CKHkClueMan::sendEvent 0x5139, into spy +0x94) and puts it back (+0x34, the
// track to switch to; +0xE0 blend 1.0, +0xE4 0) at the end, just before it
// sends 0x5121. A second start -- the walk resent, or the player pressing X on
// the spy again -- saves the dialogue's own CKCameraFixTrack, and the camera
// stayed on it after the dialogue. The track from before the first start is
// kept here and put back if the spy restored anything else.

static char* CameraService() {
	CKYellowPages* yp = YellowPages();
	return yp ? *(char**)((char*)yp + 0x68) : nullptr;
}


// Diagnostics: what the spies saved, logged when it changes during a dialogue.
static void WatchSpyCameras() {
	static void* last[8];
	if (!g_trackBeforeSpy) { memset(last, 0, sizeof(last)); return; }
	unsigned n = GetNumLvlClassInstances(2, 161);
	for (unsigned i = 0; i < n && i < 8; i++) {
		char* c = (char*)GetLvlObject(2, 161, i);
		void* saved = c ? *(void**)(c + 0x94) : nullptr;
		if (saved == last[i]) continue;
		uintptr_t vt = saved ? *(uintptr_t*)saved : 0;
		if (saved && saved != g_trackBeforeSpy && (vt == kFixTrackVtbl || vt == kPlainCameraVtbl)) {
			*(void**)(c + 0x94) = g_trackBeforeSpy;   // a second start saved the dialogue's own camera
			Log("coop: spy %u saved the dialogue's camera %p (vt %08X) as the one to go back to; set to %p", i,
			    saved, (unsigned)vt, g_trackBeforeSpy);
			saved = g_trackBeforeSpy;
		}
		last[i] = saved;
	}
}

// Leader switches (0x4800/0x4801/0x48B7/0x48B8) that arrive while a hero is
// flying from a trio catapult (situation 0x0D) wait until the flight is over.
// Level 4 sector 8's launcher started by Obelix: a zone on the landing ground
// bound to Asterix sends "switch to Asterix" as the trio comes down; taken from
// Obelix in mid-flight, the landing never ended -- both heroes hung in the air
// for good (also with Obelix leading in single player). With Asterix leading
// the same switch only comes after the landing and changes nothing.
const uint8_t kSituationCatapultFlight = 0x0D;
int g_deferredSwitch = -1;
DWORD g_deferredSince = 0;

static bool IsLeaderSwitch(int ev) {
	ev &= 0xFFFF;
	return ev == 0x4800 || ev == 0x4801 || ev == 0x48B7 || ev == 0x48B8;
}

// Someone at the merchant: its state (+8 bits 0-2) not 0, or a hero still in
// the shop's situation (0x0A: walking up to it, or the goodbye's last frames).
static bool ShopBusy(Trio t) {
	for (unsigned i = 0, n = GetNumLvlClassInstances(2, 172); i < n && i < 16; i++) {
		char* s = (char*)GetLvlObject(2, 172, i);
		if (s && (*(uint8_t*)(s + 8) & 7) != 0) return true;
	}
	for (int i = 0; t && i < 2; i++) {
		Hero h = t.GetHero(i);
		if (h && Situation(h) == kSituationShop) return true;
	}
	return false;
}

static bool HeroFlying(Trio t) {
	for (int i = 0; i < 2; i++) {
		Hero h = t.GetHero(i);
		if (h && Situation(h) == kSituationCatapultFlight) return true;
	}
	return false;
}

static void ResetCableCar(char* car, int hero);
static char* NearestTowedCar(Hero h);
static int KeptOffCars();

bool CoopDeferTrioEvent(void* trio, int ev) {
	// Individual heroes: a cable car takes the hero who steps on it from the
	// hero's own tick (0x49ACB9: "Obelix leads", 0x49ACC7: "Asterix rides it",
	// AI order 9) -- with the partner down the ride is not started: the hero
	// reset (two AI ticks, as R3 + B) and put back. The same while a lifter waits
	// for that hero with the partner on it (WatchLifterHint): the car would lock it
	// with the partner unable to pull.
	{
		extern void* g_trioEventCaller;
		uintptr_t from = (uintptr_t)g_trioEventCaller;
		int e = ev & 0xFFFF;
		bool lifter = KeptOffCars() >= 0 && IndivFallenHero() < 0;
		if ((e == 0x4801 || e == 0x4800 || e == 0x4812) && from >= 0x49AC80 && from < 0x49AD00 &&
		    (IndivFallenHero() >= 0 || lifter) && g.p2Joined && !g.suspended) {
			int up = lifter ? KeptOffCars() : IndivFallenHero() == HERO_ASTERIX ? HERO_OBELIX : HERO_ASTERIX;
			if (e == 0x4812) {
				StartAiCycle(up, lifter ? "a cable car, kept off" : "a cable car with the partner down");
				if (IndivFallenHero() >= 0) IndivBlockPartnerSpot("a cable car");
				else {
					// the car took the hero already (+0x7C bit 0x20 "rider on", 0x80 "free" gone):
					// it would never offer itself again
					Trio tc = GetTrio();
					char* car = tc ? NearestTowedCar(tc.GetHero(up)) : nullptr;
					if (car && (*(uint32_t*)(car + 0x7C) & 0xA0) == 0x20) ResetCableCar(car, up);
					IndivPutOnSafeGround(up);
				}
			}
			Log("coop: trio event %04X from %p held back -- %s", e, (void*)from,
			    lifter ? "kept off cable cars (a lifter waits for it, or its car was sent back)" : "the cable car waits for both heroes");
			return true;
		}
	}
	// Freedom mode: the level's 4803 (zone 03C8) does not free the hero a
	// corkscrew's circle holds (g_corkRide) -- it stays the level's until its ride
	// ends. The lock's own bookkeeping runs as for any 4803 (the sequence goes on:
	// Obelix off before turning it is still set back).
	if ((ev & 0xFFFF) == 0x4803 && g_config.freedom && g_config.freedomCorkscrews && g_corkRide.hero >= 0 && g.p2Joined &&
	    !g.suspended) {
		extern void* g_trioEventCaller;
		uintptr_t from = (uintptr_t)g_trioEventCaller;
		Trio tc = GetTrio();
		Hero h = tc ? tc.GetHero(g_corkRide.hero) : Hero();
		char* node = g_corkRide.cork ? *(char**)(g_corkRide.cork + 0x10) : nullptr;
		char* fr = node ? *(char**)(node + 4) : nullptr;
		if (from >= 0x400000 && from < 0x800000 && h && fr && g.held[g_corkRide.hero] && (int)h.Order() == 3) {
			const float* p = (const float*)(fr + 0x80);
			float dx = h.Pos().x - p[0], dz = h.Pos().z - p[2];
			if (dx * dx + dz * dz < 4.0f * 4.0f) {
				if (g_lock.hero == g_corkRide.hero) EndLock(true);
				static int s_told = 0;
				if (s_told++ < 20)
					Log("coop: freedom: trio event 4803 from %p held back -- %s stays on the corkscrew's circle for its ride",
					    (void*)from, HeroName(g_corkRide.hero));
				return true;
			}
		}
	}
	// Individual heroes: a Roman agent's talk needs both heroes -- with one down
	// its offer (0x511F) and start (0x5120) are held back (the hero not put back).
	if (((ev & 0xFFFF) == 0x511F || (ev & 0xFFFF) == 0x5120) && IndivFallenHero() >= 0 && g.p2Joined && !g.suspended) {
		IndivBlockTalk("a Roman agent's talk");
		return true;
	}
	Trio t = GetTrio();
	// Asterix's X at a machine (FUN_0049A080) with Obelix's player elsewhere.
	int lo = ev & 0xFFFF;
	// (The boat only: Asterix takes the machine gun alone, and Obelix joins him
	// at it if he likes -- JoinGun.)
	if (t && t.raw() == (char*)trio && lo == 0x4807 && g.p2Joined && !g.suspended &&
	    !g.machineSending && t.GetHero(HERO_ASTERIX) &&
	    (*(uint32_t*)(t.GetHero(HERO_ASTERIX).raw() + 0x3EC) & 0x10) && !MachineMatesNear(t)) {
		RefuseMachine(t, 1, HERO_ASTERIX);
		return true;
	}
	if (t && t.raw() == (char*)trio && ProximityBossRunDropsWalk(ev & 0xFFFF)) {
		Log("coop: trio event %04X (the sidekick runs ahead from the boss) dropped: each player runs their own hero", ev & 0xFFFF);
		return true;
	}
	// What takes "the companion" along -- a launcher's call to its marker
	// (0x4804; lifters do not send it, they wait for both players), a ride (the boat 0x4807, the belly slide and the other
	// modes 0x480A..0x480F) -- with the other player at the merchant: the visit
	// ends first (the shopper is in the shop's situation, out of the game's reach).
	// Cable cars (0x4812) and light pillars (0x4802 / 0x4803) wait for both
	// players instead, so the shop stays.
	if (t && t.raw() == (char*)trio && g.p2Joined && g.shopping && !g.shopForcedClosed &&
	    (lo == 0x4804 || lo == 0x4807 || (lo >= 0x480A && lo <= 0x480F)) && ShopBusy(t)) {
		char why[64];
		sprintf(why, "the other player's ride / launcher (trio event %04X)", lo);
		CloseShopFor(why);
	}
	if (!t || t.raw() != (char*)trio || !IsLeaderSwitch(ev)) return false;
	// The same while a player is at the merchant (the shop pauses the world in
	// single player; in co-op the other player plays on): LV2-10's zone by the
	// save point (event 05A0) sends "Asterix leads" when Asterix walks in -- taken
	// from Obelix at the merchant, the shop went on as Asterix's, its camera in
	// player 1's half, Obelix free to walk off with the menu up (bro_test log).
	bool shop = g.p2Joined && ShopBusy(t);
	if (!shop && !HeroFlying(t)) return false;
	g_deferredSwitch = ev & 0xFFFF;
	g_deferredSince = GetTickCount();
	Log("coop: trio event %04X %s", g_deferredSwitch,
	    shop ? "while a player is at the merchant -- after the visit" : "while flying from a catapult -- after the landing");
	return true;
}

static void SendDeferredSwitch() {
	if (g_deferredSwitch < 0) return;
	Trio t = GetTrio();
	if (!t) { g_deferredSwitch = -1; return; }
	if (HeroFlying(t) && GetTickCount() - g_deferredSince < 20000) return;
	if (g.p2Joined && ShopBusy(t)) return;
	int ev = g_deferredSwitch;
	g_deferredSwitch = -1;
	Log("coop: landed / out of the shop -- trio event %04X sent now", ev);
	typedef int(__fastcall* SendFn)(void* self, void* edx, int event, void* param);
	((SendFn)(*(void***)t.raw())[5])(t.raw(), nullptr, ev, nullptr);
}

// One player at the merchant, the other sets off a Roman agent's dialogue: the
// dialogue ran with the shop's menu still up -- its camera, its input, its
// "world pause" skipped -- and crashed on A (xxl_coop.shop+spy.log). The visit
// ends first, as the shop ends it itself after the goodbye (state 4): state 0
// (FUN_004C1040, thiscall(shop, state): the state's exit and entry), then trio
// 0x5124 ("done": the shopper's situation 0x0A ends).
// The camera back as the shop's leaving state's entry gives it (FUN_004C1B90(1)):
// the camera service's +0x34 / +0xE0 from the shop's +0x11C / +0x120, its target
// (CKSrvCamera::SetTarget 0x459050, thiscall(target, seconds, cut)) `target`, the
// new track's position the one kept at the visit's start (+0x1B0). Without it the
// camera stayed the shop's: through the other player's save menu and after it.
static void HandBackShopCamera(char* cs, char* sh, void* target) {
	*(uint32_t*)(cs + 0xC) |= 0x400;
	*(uint32_t*)(cs + 0xE0) = *(uint32_t*)(sh + 0x120);
	*(uint32_t*)(cs + 0x34) = *(uint32_t*)(sh + 0x11C);
	*(uint8_t*)(cs + 0xE4) = 0;
	((void(__fastcall*)(char*, void*, void*, float, char))0x459050)(cs, nullptr, target, 1.0f, 0);
	if (char* tr = *(char**)(cs + 0x38)) memcpy(tr + 0x48, sh + 0x1B0, 12);
	Log("coop: the merchant's camera handed back (track now %p)", *(void**)(cs + 0x38));
}

// A forced close met a cutscene's camera: once the game's camera is back on the
// shop's (the cutscene's end goes back to the camera it took over -- both
// players' views stayed on the shop), it is handed back, aimed at the hero whose
// camera it is; a player's copy on the shop's camera starts over.
static void WatchShopCamera(Trio t) {
	char* sh = g.shopCamPending;
	if (!sh || !t) return;
	if (!g.p2Joined || ShopBusy(t) || GetTickCount() - g.shopCamSince > 120000) { g.shopCamPending = nullptr; return; }
	void* shopTrack = *(void**)(sh + 0x118);
	for (int h = 0; h < 2; h++) {
		const char* c = GameCamCopyState(h);
		if (c && *(void* const*)(c + 0x38) == shopTrack) {
			GameCamDropCopy(h);
			Log("coop: %s's camera copy was on the merchant's camera -- it starts over", HeroName(h));
		}
	}
	char* cs = CameraService();
	if (!cs || *(void**)(cs + 0x38) != shopTrack) return;
	int who = GameCamLiveHero();
	if (who < 0) who = t.Leader();
	Hero h = who >= 0 && who < 2 ? t.GetHero(who) : Hero();
	void* target = h ? *(void**)(h.raw() + 0x10) : *(void**)(sh + 0x19C);
	g.shopCamPending = nullptr;
	Log("coop: the game's camera is back on the merchant's after the cutscene -- handed back to %s", HeroName(who));
	HandBackShopCamera(cs, sh, target);
}

void CloseShopFor(const char* why) {
	Trio t = GetTrio();
	for (unsigned i = 0, n = GetNumLvlClassInstances(2, 172); t && i < n && i < 16; i++) {
		char* sh = (char*)GetLvlObject(2, 172, i);
		if (!sh || (*(uint8_t*)(sh + 8) & 7) == 0) continue;
		Log("coop: %s closes the merchant's shop (%s was in state %d)", why, HeroName(g.shopper), *(uint8_t*)(sh + 8) & 7);
		// His words cut first, as his goodbye does (speech service yp +0x84, vf 0x6C),
		// and his "talking" bit (+0x1CA 0x20) off: while both hold, his pose stays the
		// talking one (open, leaning out) whatever pose state 0 asks for.
		{
			CKYellowPages* yp = YellowPages();
			char* speech = yp ? *(char**)((char*)yp + 0x84) : nullptr;
			if (speech) ((void(__fastcall*)(char*, void*))(*(void***)speech)[0x6C / 4])(speech, nullptr);
			*(uint8_t*)(sh + 0x1CA) &= ~0x20;
		}
		((void(__fastcall*)(char*, void*, int))0x4C1040)(sh, nullptr, 0);
		// its menu's pieces (item, price, legends: +0x28..+0x34) hidden, as its reset does
		((void(__fastcall*)(char*, void*, int))0x4BF670)(sh, nullptr, 0);
		// The camera back as the leaving state's entry gives it (FUN_004C1B90(1)): the
		// camera service's +0x34 / +0xE0 from the shop's +0x11C / +0x120, its target
		// (CKSrvCamera::SetTarget 0x459050, thiscall(target, seconds, cut)) the
		// shop's +0x19C, the new track's position the one kept at the visit's start
		// (+0x1B0). Without it the camera stayed the shop's: through the other
		// player's save menu and after it (player 2's half, LV2-10).
		// The game's camera is the shopper's (a leader switch waits for the visit's
		// end); if it is the other player's, the shop's camera is in the copy, which
		// starts over from the game's camera, aimed at its own hero. A cutscene that
		// has already taken the camera (the other player's boss zone, LV2-10) goes
		// back to the shop's when it ends: handed back then (WatchShopCamera) --
		// handing back now wrote the shop's place into the cutscene's own camera.
		int live = GameCamLiveHero();
		char* cs = CameraService();
		if (live >= 0 && live != g.shopper) {
			GameCamDropCopy(g.shopper);
			Log("coop: the merchant's camera was in %s's copy -- the copy starts over", HeroName(g.shopper));
		} else if (cs && *(void**)(sh + 0x118) && *(void**)(cs + 0x38) == *(void**)(sh + 0x118)) {
			HandBackShopCamera(cs, sh, *(void**)(sh + 0x19C));
		} else if (cs && *(void**)(sh + 0x118)) {
			g.shopCamPending = sh;
			g.shopCamSince = GetTickCount();
			Log("coop: the merchant's camera is not up (track %p) -- handed back once the game goes back to it",
			    *(void**)(cs + 0x38));
		}
		g.shopClosedHero = g.shopper;
		g.shopClosedAt = GetTickCount();
		// The game's camera follows the leader again (its hero's own camera handed
		// back above): the other player's launcher takes it over for its flight.
		g.shopHero = -1;
		// and the leaving state's exit (state 5, FUN_004C1B90(2, x), thiscall ret 8),
		// which the natural end goes through: the world unpaused and the game manager's
		// "at the merchant" bit (+0x5C 0x4000000: the shop's screen, bars) cleared
		((char(__fastcall*)(char*, void*, int, int))0x4C1B90)(*(char**)(sh + 0x1A4), nullptr, 2, 0);
		// his bars (+0x38, +0x3C: slid in by state 1, out by state 5) gone at once: vf19
		((void(__fastcall*)(char*, void*))(*(void***)sh)[19])(sh, nullptr);
		// back in his rest pose (FUN_004C0ED0(anim, dt), thiscall ret 8; anim 0)
		((void(__fastcall*)(char*, void*, int, float))0x4C0ED0)(sh, nullptr, 0, 0.0f);
		CKYellowPages* yp = YellowPages();
		char* gm = yp ? (char*)yp->gameManager : nullptr;
		if (gm) Log("coop: the merchant closed: game manager +0x5C now %08X", *(unsigned*)(gm + 0x5C));
		typedef int(__fastcall* SendEventFn)(void* self, void* edx, int event, void* param);
		((SendEventFn)(*(void***)t.raw())[5])(t.raw(), nullptr, 0x5124, nullptr);
		g.shopForcedClosed = true;
		g.shopCloseCheck = 3;
		g.closedShop = sh;
	}
}

void CoopNoteTrioEventFlagged(unsigned eventId) { NoteTrioEventFlagged(eventId); }

// Still locked by the level, waiting for the partner: held by the game, or told
// to wait (+0x564 = 3, 0x4802; 0x4803 sets 0) -- in an order that waits (3 wait,
// 8 the beam, 9 a cable car). Not a scripted walk (LV2-1: the platform's top
// zone 0058 gives Asterix the lead and walks Obelix to a marker, 4828 order 1):
// that is the level going on; undone, its "before" lead went back to Obelix and
// the torch's prompt became player 2's.
static bool LockedByLevel(int idx, Hero h) {
	if (!h) return false;
	uint8_t order = *(uint8_t*)(h.raw() + 0x564);
	bool waits = order == AI_WAIT || order == AI_HANG_ON_BEAM || order == AI_USE_CABLECAR;
	if (g_config.freedom) return waits && g.held[idx];   // only a cable car holds a hero
	return waits && (g.held[idx] || g_lock.hero == idx);
}

static void UndoLock(Trio t, int up, bool lead);
static void UndoSequence();

// A rope-pulled cable car (CKHkTowedTelepher 2,159) lets go of `hero` (0x512D)
// and goes back to its start (vf16).
static void ResetCableCar(char* car, int hero) {
	char* node = *(char**)(car + 0x10);
	char* fr = node ? *(char**)(node + 4) : nullptr;
	if (!fr) return;
	typedef int(__fastcall* SendEventFn)(void* self, void* edx, int event, void* param);
	uint32_t f0 = *(uint32_t*)(node + 0x10), h0 = *(uint32_t*)(car + 0x7C);
	((SendEventFn)(*(void***)car)[5])(car, nullptr, 0x512D, nullptr);
	((void(__fastcall*)(char*, void*))(*(void***)car)[16])(car, nullptr);
	node = *(char**)(car + 0x10);
	fr = node ? *(char**)(node + 4) : nullptr;
	// vf16 is its level-start init, which ends by hiding the car (node +0x10
	// bit 1) -- the model was gone, the rope still there: shown again.
	if (node && !(f0 & 2)) *(uint32_t*)(node + 0x10) &= ~2u;
	Log("coop: the cable car %p let go of %s and went back to its start (node %p flags %08X -> %08X, hook +7C %08X -> %08X, "
	    "now at %.1f %.1f %.1f)", car, HeroName(hero), node, f0, node ? *(uint32_t*)(node + 0x10) : 0, h0,
	    *(uint32_t*)(car + 0x7C), fr ? ((float*)(fr + 0x80))[0] : 0, fr ? ((float*)(fr + 0x80))[1] : 0, fr ? ((float*)(fr + 0x80))[2] : 0);
}

// The rope-pulled cable car nearest hero `h` (within 12 units), else null.
static char* NearestTowedCar(Hero h) {
	char* best = nullptr;
	float bestD = 12.0f * 12.0f;
	for (unsigned i = 0, n = h ? GetNumLvlClassInstances(2, 159) : 0; i < n; i++) {
		char* car = (char*)GetLvlObject(2, 159, i);
		char* node = car ? *(char**)(car + 0x10) : nullptr;
		char* fr = node ? *(char**)(node + 4) : nullptr;
		if (!fr) continue;
		const float* p = (const float*)(fr + 0x80);
		float d2 = (h.Pos() - Vector3(p[0], p[1], p[2])).len2();
		if (d2 < bestD) { bestD = d2; best = car; }
	}
	return best;
}

int CoopUndoLockFor(int fallen) {
	int up = fallen == HERO_ASTERIX ? HERO_OBELIX : fallen == HERO_OBELIX ? HERO_ASTERIX : -1;
	if (up < 0 || !g.p2Joined) return -1;
	Trio t = GetTrio();
	Hero h = t ? t.GetHero(up) : Hero();
	if (!h) return -1;
	if (!LockedByLevel(up, h)) {
		if (!SequenceUnderWay()) return -1;
		// Freed already, but the platform's turn not finished: back to before it set off.
		Log("coop: %s went down before the spinning platform's turn ended -- the sequence undone", HeroName(fallen));
		int before = g_seqPic.leader;
		UndoSequence();
		return before;
	}
	Log("coop: %s went down while %s waited on it -- the sequence undone", HeroName(fallen), HeroName(up));
	int before = g_lock.hero == up ? g_lock.pic.leader
	           : g_seqHero == up && SequenceUnderWay() ? g_seqPic.leader : -1;   // (a corkscrew's ride kept, freedom)
	UndoLock(t, up, true);
	return before;
}

// The trio's lead to `who` (0x4800 Asterix, 0x4801 Obelix).
static void GiveLead(Trio t, int who) {
	if ((who != HERO_ASTERIX && who != HERO_OBELIX) || t.Leader() == who) return;
	typedef int(__fastcall* SendEventFn)(void* self, void* edx, int event, void* param);
	((SendEventFn)(*(void***)t.raw())[5])(t.raw(), nullptr, who == HERO_ASTERIX ? 0x4800 : 0x4801, nullptr);
}

// The same a little later (GiveLeadLater -> WatchPendingLead): the hero just put
// back is still where it was for the spinning platform this frame -- handed the
// lead at once on its circle, it set it off again.
struct PendingLead { int who = -1; DWORD at = 0; } g_pendingLead;
static void GiveLeadLater(int who) { g_pendingLead.who = who; g_pendingLead.at = GetTickCount(); }
static void WatchPendingLead(Trio t) {
	if (g_pendingLead.who < 0 || GetTickCount() - g_pendingLead.at < 300) return;
	if (t && g.p2Joined) GiveLead(t, g_pendingLead.who);
	g_pendingLead.who = -1;
}

// The automatic cable car (CKHkTelepher 2,158) takes only the trio's leader
// (its +0x84 hero == trio vf 0x74): ticked with the nearest player's hero posing
// as the leader (proximity), it took the other one's -- and told the trio
// (0x0C0D) in that tick. All the rest of the ride is the real leader's: the
// companion's walk to its end (0x481C, AI order 1), the jump off at the end
// (0x0C09: the leader's own; the companion's AI order 2) and the helmets on the
// way. LV3-6: the light pillar's circle had handed Obelix the lead (4801); in
// freedom mode Asterix rode on to the end and stayed on the car, his jump off an
// AI order not followed, the helmets not his. The rider takes the lead the next
// frame -- before the walk order (0.3 s later), as in a ride boarded leading.
int g_telepherRider = -1;

// With cable_car_rider_leads off the lead stays where it was; the ride's own
// events then treat the rider as the leader they expect (CoopRideSliceBegin):
// the car's 0x0C09 at its end (from 0x549BA5: the leader on the car's ground --
// situation 2 -- jumps off, +0x3F0 |= 0x200 and its target; anyone else's turns
// into 0x4805, the companion's AI order 2: the rider stayed on the car) and the
// level's orders for the companion (0x481C at the boarding: the one not riding
// walks to the end). The car itself only checks that the leader stands on its
// ground (+0x84 vs trio vf29, the leader's +0x280), and proximity ticks it with
// the nearest player's hero posing as the leader.
struct AutoRide { int rider = -1; char* car = nullptr; } g_autoRide;
struct RideSlice { bool on = false; uint8_t leader = 0, companion = 0, rider = 0, other = 0; } g_rideSlice;

static char* AutoCarUnder(Hero h) {
	char* ground = h ? *(char**)(h.raw() + 0x280) : nullptr;
	for (unsigned i = 0, n = ground ? GetNumLvlClassInstances(2, 158) : 0; i < n; i++) {
		char* c = (char*)GetLvlObject(2, 158, i);
		if (c && *(char**)(c + 0x84) == ground) return c;
	}
	return nullptr;
}

static void WatchTelepherRider(Trio t) {
	int who = g_telepherRider;
	g_telepherRider = -1;
	if (g_autoRide.rider >= 0) {
		Hero r = t ? t.GetHero(g_autoRide.rider) : Hero();
		if (!r || !g.p2Joined || Situation(r) != 0x02 || t.Leader() == g_autoRide.rider) {
			Log("coop: %s's automatic cable car ride is over (situation %02X)", HeroName(g_autoRide.rider), r ? Situation(r) : 0xFF);
			g_autoRide = {};
		}
	}
	if (who < 0 || !t || !g.p2Joined || t.Leader() == who) return;
	if (!g_config.carRiderLeads) {
		if (g_autoRide.rider == who) return;   // (0x0C0D comes with each of its ticks while boarding)
		Hero r = t.GetHero(who);
		g_autoRide = { who, AutoCarUnder(r) };
		Log("coop: %s boarded an automatic cable car while %s led -- the ride's events treat the rider as the leader (car %p)",
		    HeroName(who), HeroName(t.Leader()), g_autoRide.car);
		return;
	}
	Log("coop: %s boarded an automatic cable car while %s led -- the lead to the rider", HeroName(who), HeroName(t.Leader()));
	GiveLead(t, who);
}

// Freedom mode, the rotary beam's catch: trio 0x0C02 from the beam (0x53F348;
// FUN_00481d50 case 2) hangs the companion on it (AI order 8) only while its order
// is 3, "wait here", and it is within the trio's +0x50 of the beam. The circle's
// wait gives Asterix that 3 (LV2-8: the platform's pillar), but a lead switch since
// sets the other hero's order to 0 (the user's R3 + X; a level's switch, a cable
// car...) and the beam took nobody. A companion player's hero that jumped in the
// last second counts as waiting for the catch; not caught, its order is put back.
// Asterix leading jumps for it too: the catch is run with him as the companion
// and the real leader back afterwards -- he rides it as the game's while leading
// (the held release waits for the ride's end). Its drop at the far end, trio
// 0x0C03 from the beam (0x53F4C3: the companion's order 8 -> 3; anyone else's
// "nobody on it", 0x0C05 to the beam), is run the same way. Only Asterix: Obelix
// has no ride.
static void WatchCorkLaunch(Trio t) {
	int idx = g_corkRide.hero;
	if (idx < 0 || !g_corkRide.launched || !t) return;
	Hero h = t.GetHero(idx);
	if (!h || !g.held[idx]) { g_corkRide = {}; return; }
	if (h.Order() == AI_JUMP_TO_POS) { g_corkRide.jumping = true; return; }
	if (!g_corkRide.jumping && GetTickCount() - g_corkRide.launched < 3000) return;   // (the jump not begun yet)
	g.held[idx] = false;
	g_corkRide = {};
	Log("coop: freedom: %s landed from the corkscrew's launch -- back to its player (AI order %d)", HeroName(idx), (int)h.Order());
}

DWORD g_beamJumpAt[2] = {};   // a player's hero's last jump (body 0x0E), set by WatchFreedomBeam
struct BeamCatch { int hero = -1; uint8_t order = 0; bool sliced = false; uint8_t leader = 0, companion = 0; bool drop = false; } g_beamCatch;

bool CoopBeamCatchBegin(int ev, void* caller) {
	Trio t = GetTrio();
	Hero h = t ? t.GetHero(HERO_ASTERIX) : Hero();
	const int a = HERO_ASTERIX;
	if ((ev & 0xFFFF) == 0x0C03 && (uintptr_t)caller == 0x53F4C3 && h && g.p2Joined) {
		uint8_t* tr = (uint8_t*)t.raw();
		if (tr[0x2C] != a || h.Order() != AI_HANG_ON_BEAM) return false;
		g_beamCatch = { a, (uint8_t)h.Order(), true, tr[0x2C], tr[0x2D], true };
		tr[0x2C] = tr[0x2D];
		tr[0x2D] = (uint8_t)a;
		Log("coop: the rotary beam drops Asterix (leading) -- the companion for it");
		return true;
	}
	if ((ev & 0xFFFF) != 0x0C02 || (uintptr_t)caller != 0x53F348 || !g_config.freedom || !g.p2Joined || g.suspended) return false;
	if (!h || (a != g.p1Hero && a != g.p2Hero) || g.held[a] || GetTickCount() - g_beamJumpAt[a] > 1000) return false;
	uint8_t* tr = (uint8_t*)t.raw();
	uint8_t* order = (uint8_t*)(h.raw() + 0x564);
	bool leads = tr[0x2C] == a;
	if (!leads && *order == AI_WAIT) return false;   // waiting already: the game's own catch
	g_beamCatch = { a, *order, leads, tr[0x2C], tr[0x2D], false };
	if (leads) {
		tr[0x2C] = tr[0x2D];
		tr[0x2D] = (uint8_t)a;
	}
	*order = AI_WAIT;
	Log("coop: freedom: Asterix jumped as the rotary beam came (AI order %d%s) -- taken as waiting for its catch",
	    g_beamCatch.order, leads ? ", leading: the companion for it" : "");
	return true;
}

void CoopBeamCatchEnd(bool began) {
	if (!began || g_beamCatch.hero < 0) return;
	Trio t = GetTrio();
	Hero h = t ? t.GetHero(g_beamCatch.hero) : Hero();
	if (h && !g_beamCatch.drop && h.Order() != AI_HANG_ON_BEAM) *(uint8_t*)(h.raw() + 0x564) = g_beamCatch.order;   // not caught (too far)
	uint8_t* tr = t ? (uint8_t*)t.raw() : nullptr;
	if (tr && g_beamCatch.sliced && tr[0x2C] == g_beamCatch.companion && tr[0x2D] == (uint8_t)g_beamCatch.hero) {
		tr[0x2C] = g_beamCatch.leader;
		tr[0x2D] = g_beamCatch.companion;
	}
	g_beamCatch = {};
}

bool CoopRideSliceBegin(int ev, void* caller) {
	ev &= 0xFFFF;
	if (g_rideSlice.on || g_autoRide.rider < 0 || !g.p2Joined) return false;
	uintptr_t from = (uintptr_t)caller;
	bool endJump = ev == 0x0C09 && from == 0x549BA5;
	bool companionOrder = ((ev >= 0x4813 && ev <= 0x4876) || (ev >= 0x48BB && ev <= 0x48EC)) && from >= 0x400000 && from < 0x800000;
	if (!endJump && !companionOrder) return false;
	Trio t = GetTrio();
	if (!t || t.Leader() == g_autoRide.rider) return false;
	uint8_t* trio = (uint8_t*)t.raw();
	g_rideSlice = { true, trio[0x2C], trio[0x2D], (uint8_t)g_autoRide.rider,
	                (uint8_t)(g_autoRide.rider == g.p1Hero ? g.p2Hero : g.p1Hero) };
	trio[0x2C] = g_rideSlice.rider;
	trio[0x2D] = g_rideSlice.other;
	static int s_told = 0;
	if (s_told++ < 20)
		Log("coop: trio event %04X from %p for %s's automatic cable car ride -- %s as the leader for it", ev, caller,
		    HeroName(g_autoRide.rider), HeroName(g_autoRide.rider));
	return true;
}

void CoopRideSliceEnd(bool began) {
	if (!began || !g_rideSlice.on) return;
	g_rideSlice.on = false;
	Trio t = GetTrio();
	if (!t) return;
	uint8_t* trio = (uint8_t*)t.raw();
	if (trio[0x2C] == g_rideSlice.rider && trio[0x2D] == g_rideSlice.other) {
		trio[0x2C] = g_rideSlice.leader;
		trio[0x2D] = g_rideSlice.companion;
	}
}

// Freedom mode and the rotary beam (CKHkRotaryBeam 2,57; LV4-2): the level asks
// the hero on its circle to hang on it (beam event 0x0C02 -> AI order 8) when the
// beam is over him (+8 bit 2). A player's jump never catches it -- only the AI's
// order 8 does. So the player's jump (body 0x0E) hands the hero to the game for
// that ride; it comes back when the beam lets go. The beam over the circle
// (state +8 & 7 == 4) first waits out a delay (+0x20), then until it has stopped
// swinging (+0x2C speed < 0.2, +0x28 angle within 0.02 of +0x3C), and only then
// sends 0x0C02 (state 2) -- the order came long after the beam was there. A jump
// under it cuts the delay; a jump up to 1 s before the order still counts.
static void WatchFreedomBeam(Trio t) {
	if (!g_config.freedom || !t || !g.p2Joined || g.suspended) return;
	DWORD* s_jumpAt = g_beamJumpAt;
	const int players[2] = { g.p1Hero, g.p2Hero };
	for (int idx : players) {
		Hero h = t.GetHero(idx);
		if (h && *(uint8_t*)(h.raw() + 0xE5) == 0x0E) s_jumpAt[idx] = GetTickCount();
	}
	for (unsigned i = 0, n = GetNumLvlClassInstances(2, 57); i < n; i++) {
		char* b = (char*)GetLvlObject(2, 57, i);
		if (!b || (*(uint32_t*)(b + 8) & 7) != 4 || *(float*)(b + 0x20) <= 0.0f) continue;
		for (int idx : players)
			if (GetTickCount() - s_jumpAt[idx] < 300 && !g.held[idx]) {
				Log("coop: freedom: %s jumped under the rotary beam -- its wait (%.2f s) cut", HeroName(idx), *(float*)(b + 0x20));
				*(float*)(b + 0x20) = 0.0f;
			}
	}
	for (int idx : players) {
		Hero h = t.GetHero(idx);
		if (!h || g.held[idx] || h.Order() != AI_HANG_ON_BEAM || GetTickCount() - s_jumpAt[idx] > 1000) continue;
		Log("coop: freedom: %s jumped for the rotary beam -- the game's for the ride", HeroName(idx));
		Hold(t, idx, "jumped for the rotary beam", true);
	}
}

// An ended lock's sequence still under way (SequenceUnderWay) back to before it
// set off: the spinning platform down, its pillar and detectors as then.
static void UndoSequence() {
	for (size_t i = 0; i < g_seqPic.nFlags; i++)
		if (*(uintptr_t*)g_seqPic.flags[i].hook == 0x628E0C) *(float*)(g_seqPic.flags[i].hook + 0x28) = 0.0f;   // down again
	RestoreLockPicture(g_seqPic);
	g_seqLive = false;
}

// The locked sequence of hero `up` back to before it started, the hero freed
// (it takes the lead if `lead`; else the lead goes back to who had it before the
// lock -- the spinning platform's circle answers only the leader: LV2-1, its
// event's 4801 had given it to Obelix and Asterix's circle stayed dead) and put
// on its last solid ground.
static void UndoLock(Trio t, int up, bool lead) {
	// A rope-pulled cable car (CKHkTowedTelepher; not the automatic ones) the
	// locked hero boarded lets go and goes back to its start.
	char* carBack = nullptr;
	for (unsigned i = 0, n = GetNumLvlClassInstances(2, 159); i < n; i++) {
		char* car = (char*)GetLvlObject(2, 159, i);
		if (car && g_lock.hero == up && car == g_lock.car) { ResetCableCar(car, up); carBack = car; }
	}
	int before = g_lock.hero == up ? g_lock.pic.leader : -1;
	if (g_lock.hero == up) RestoreLockPicture(g_lock.pic);
	else if (g_seqHero == up && SequenceUnderWay()) {
		// (a corkscrew's ride in freedom mode: its lock ended at 03C8, the hero kept)
		before = g_seqPic.leader;
		UndoSequence();
	}
	EndLock();
	if (g_corkRide.hero == up) g_corkRide = {};
	// The hero freed: the companion moves again, the lead, and a reset pass.
	typedef int(__fastcall* SendEventFn)(void* self, void* edx, int event, void* param);
	((SendEventFn)(*(void***)t.raw())[5])(t.raw(), nullptr, 0x4803, nullptr);
	if (lead) GiveLead(t, up);
	else GiveLeadLater(before);
	g.held[up] = false;
	StartAiCycle(up, "its sequence undone");
	IndivPutOnSafeGround(up);
	GameCamAimAtHero(up);   // a cable car had its camera follow the car
	// The car, set back, offers itself again at once to a hero still in its zone:
	// kept off cable cars for a while (WatchStrandedCar).
	if (carBack) KeepOffCars(up, carBack);
}

// R3 + B on a hero the level holds: its sequence undone as for a fallen partner
// (the light pillar back, a cable car back at its start, the spinning platform
// down, the lead back), the hero freed and on its last solid ground -- off the
// circle, or it would be locked again at once. A step towards never being locked.
static void UnlockByPlayer(Trio t, int me, const char* player) {
	Hero h = t.GetHero(me);
	if (!h) return;
	if (LockedByLevel(me, h) && (g_lock.hero == me || g_corkRide.hero == me)) {
		Log("coop: %s unlocked %s (R3 + B) -- its sequence undone", player, HeroName(me));
		UndoLock(t, me, false);
	} else if (g_seqHero == me && SequenceUnderWay()) {
		Log("coop: %s unlocked %s (R3 + B) -- its spinning platform's sequence set back", player, HeroName(me));
		int before = g_seqPic.leader;
		UndoSequence();
		IndivPutOnSafeGround(me);
		GiveLeadLater(before);
	} else if (g.held[me]) {
		g.held[me] = false;
		Log("coop: %s took %s back from the game (AI order %d left behind)", player, HeroName(me), (int)h.Order());
		EnforcePlayer(t, me);
	}
}

// Freedom mode: the level has given `hero` a scripted order the mode leaves
// unfollowed (a wait, a walk to a marker) -- it would be waiting now.
bool CoopFreedomWaiting(int hero) {
	Trio t = GetTrio();
	Hero h = t ? t.GetHero(hero) : Hero();
	if (!g_config.freedom || !h || g.held[hero] || t.Leader() == hero) return false;
	int o = (int)h.Order();
	return IsScriptedOrder(o) && !FreedomLetsHold(o);
}

void CoopUnlockHero(int hero) {
	Trio t = GetTrio();
	if (t && g.p2Joined) UnlockByPlayer(t, hero, hero == g.p1Hero ? "player 1" : "player 2");
}

// The spinning platform's sequence under way (Obelix crossed 03C8: Asterix
// freed, the platform off until its turn reaches the top) with the partner more
// than 25 units from the circle -- the turnstile is beside it -- for 3 s: set
// back (platform down, pillar on, the lead to who had it), so that Asterix on
// the circle is locked again at once.
static void WatchUnfinishedSequence(Trio t) {
	if (!g_seqLive || g_seqHero < 0 || !t || !g.p2Joined || g.suspended || g_lock.hero >= 0 || IndivFallenHero() >= 0) {
		g_seqAwaySince = 0;
		return;
	}
	Hero o = t.GetHero(OtherHero(g_seqHero));
	if (!o || (o.Pos() - g_seqAt).len2() <= 25.0f * 25.0f || !SequenceUnderWay()) { g_seqAwaySince = 0; return; }
	if (!g_seqAwaySince) g_seqAwaySince = GetTickCount();
	if (GetTickCount() - g_seqAwaySince < 3000) return;   // (walking through 03C8 to the turnstile: 1.5 s past 25)
	Log("coop: %s went off (%.0f units) before turning the spinning platform -- its sequence set back", HeroName(OtherHero(g_seqHero)),
	    sqrtf((o.Pos() - g_seqAt).len2()));
	int before = g_seqPic.leader;
	UndoSequence();
	GiveLeadLater(before);
	g_seqAwaySince = 0;
}

// The watched car stands still (+0x30 speed) away from its start, not free to
// board (+0x7C bit 0x80), its rope not held (bit 0x4000), and nobody locked in
// it: after 1 s it drives back to its start as it does after a full ride. With
// the rope let go it travels as an automatic cable car (CKHkTelepher::vf29,
// 0x548880): +0x7C bit 0x2 = on its motor (+0x94), bit 0x8 = backwards; a
// finished ride leaves 0x241B (both on), a short pull's stop 0x2431 (neither, and
// bit 0x20). Not home within 15 s (or stopped again on the way): put there.
static void WatchStrandedCar() {
	Trio t = GetTrio();
	if (g_carWatch.keptOff >= 0) {
		Hero k = t ? t.GetHero(g_carWatch.keptOff) : Hero();
		// (in its cable car situation the car is pulling it in: not walking)
		char* kc = g_carWatch.keptOffCar;
		bool free = !kc || (*(uint32_t*)(kc + 0x7C) & 0x80);
		if (!free) g_carWatch.keptOffFreeSince = 0;
		else if (!g_carWatch.keptOffFreeSince) g_carWatch.keptOffFreeSince = GetTickCount();
		bool settled = free && GetTickCount() - g_carWatch.keptOffFreeSince >= 1000 && GetTickCount() - g_carWatch.keptOffSince >= 1000;
		if (k && GetTickCount() - g_carWatch.keptOffSince < 500) g_carWatch.keptOffAt = k.Pos();
		else if (!k || (Situation(k) != 0x02 && (settled || (k.Pos() - g_carWatch.keptOffAt).len2() > 4.0f * 4.0f))) {
			if (k) Log("coop: %s may board cable cars again (%s)", HeroName(g_carWatch.keptOff),
			           settled ? "the car home and free" : "walked away");
			g_carWatch.keptOff = -1;
		}
	}
	char* car = g_carWatch.car;
	if (!car) return;
	bool listed = false;   // still one of the level's (a level change frees it)
	for (unsigned i = 0, n = GetNumLvlClassInstances(2, 159); i < n && !listed; i++) listed = (char*)GetLvlObject(2, 159, i) == car;
	char* node = listed ? *(char**)(car + 0x10) : nullptr;
	char* fr = node ? *(char**)(node + 4) : nullptr;
	if (!fr) { g_carWatch.car = nullptr; return; }
	const float* p = (const float*)(fr + 0x80);
	uint32_t f = *(uint32_t*)(car + 0x7C);
	float dist = sqrtf((Vector3(p[0], p[1], p[2]) - g_carWatch.start).len2());
	bool away = dist > 1.0f;
	if (!away && (f & 0x80)) {   // back and free: done
		if (g_carWatch.drivenAt) Log("coop: the cable car %p is back at its start", car);
		g_carWatch.car = nullptr;
		g_carWatch.drivenAt = 0;
		return;
	}
	bool still = fabsf(*(float*)(car + 0x30)) < 0.05f;
	if (g_carWatch.drivenAt) {
		// on its way back: home but not free, stopped on the way, or too long -- put there
		DWORD on = GetTickCount() - g_carWatch.drivenAt;
		if (on < 1500 || (!still && on < 15000)) { g_carWatch.stillSince = 0; return; }
		if (!g_carWatch.stillSince) g_carWatch.stillSince = GetTickCount();
		if (on < 15000 && GetTickCount() - g_carWatch.stillSince < 1000) return;
		Log("coop: the cable car %p did not get home by itself (%.1f units off, +7C %08X) -- put there", car, dist, f);
		ResetCableCar(car, g_carWatch.rider);
		g_carWatch.car = nullptr;
		g_carWatch.drivenAt = 0;
		return;
	}
	bool stranded = away && !(f & 0x80) && !(f & 0x4000) && still && g_lock.hero < 0 && !g.held[HERO_ASTERIX] &&
	                !g.held[HERO_OBELIX] && g.p2Joined && !g.suspended;
	if (!stranded) { g_carWatch.stillSince = 0; return; }
	if (!g_carWatch.stillSince) g_carWatch.stillSince = GetTickCount();
	if (GetTickCount() - g_carWatch.stillSince < 1000) return;
	Log("coop: the cable car %p stopped %.1f units from its start with nobody in it -- it drives back (+7C %08X)", car, dist, f);
	*(uint32_t*)(car + 0x7C) = (f | 0x2 | 0x8) & ~0x20u;
	g_carWatch.drivenAt = GetTickCount();
	g_carWatch.stillSince = 0;
	// The rider, freed in the car, still stands on it (its CDynamicGround): onto
	// its last solid ground, or the car carries it home and takes it again there.
	Hero r = t ? t.GetHero(g_carWatch.rider) : Hero();
	char* ground = r ? *(char**)(r.raw() + 0x280) : nullptr;
	if (ground && !IsBadReadPtr(ground, 4) && *(uintptr_t*)ground == 0x6241BC) {
		IndivPutOnSafeGround(g_carWatch.rider);
		GameCamAimAtHero(g_carWatch.rider);
	}
	KeepOffCars(g_carWatch.rider, car);
}

// The hero kept off cable cars now: the lifter waits for it, or its car was just sent back. Else -1.
static int KeptOffCars() { return g.lifterWaitsFor >= 0 ? g.lifterWaitsFor : g_carWatch.keptOff; }

// A 0x4802 freedom mode keeps (g_freedomLock): the boss's own, or the waiter on a
// corkscrew's circle -- its node within 4 units (LV3-6's pillar: 0.4).
static void NoteFreedomLock(Trio t, int waiter) {
	extern void* g_trioEventCaller;
	if (!g_config.freedom) return;
	Hero h = t.GetHero(waiter);
	if (!h) return;
	if ((uintptr_t)g_trioEventCaller == 0x4F317E) {
		g_freedomLock = { waiter, GetTickCount(), "on the boss's button" };
		return;
	}
	if (!g_config.freedomCorkscrews) return;
	for (unsigned i = 0, n = GetNumLvlClassInstances(2, 44); i < n; i++) {
		char* c = (char*)GetLvlObject(2, 44, i);
		char* node = c ? *(char**)(c + 0x10) : nullptr;
		char* fr = node ? *(char**)(node + 4) : nullptr;
		if (!fr) continue;
		const float* p = (const float*)(fr + 0x80);
		float dx = h.Pos().x - p[0], dz = h.Pos().z - p[2];
		if (dx * dx + dz * dz < 4.0f * 4.0f && fabsf(h.Pos().y - p[1]) < 4.0f) {
			g_freedomLock = { waiter, GetTickCount(), "a corkscrew's circle" };
			g_corkRide = { waiter, c };
			Log("coop: freedom: %s waits on corkscrew %p (%.1f units off)", HeroName(waiter), c, sqrtf(dx * dx + dz * dz));
			return;
		}
	}
}

void CoopNoteOffer(uint32_t offer, int hero) {
	if (offer != g.offerValue || hero != g.offerHero) {
		static int s_told = 0;
		if (g.p2Joined && s_told < 20 && (hero == g.p1Hero || hero == g.p2Hero) && offer != g.offerValue) {
			s_told++;
			Log("coop: %08X offered to %s (trio+0x4C)", offer, HeroName(hero));
		}
	}
	g.offerValue = offer;
	g.offerHero = hero;
}

void CoopNoteTrioEvent(void* trio, int event, void* param) {
	extern void* g_trioEventCaller;
	if ((event & 0xFFFF) == 0x4804 && (uintptr_t)g_trioEventCaller == 0x49C699) g.launcherAt = GetTickCount();
	if (g.p2Joined && ((event & 0xFFFF) == 0x4800 || (event & 0xFFFF) == 0x4801) &&
	    (uintptr_t)g_trioEventCaller >= 0x49AC80 && (uintptr_t)g_trioEventCaller < 0x49AD00) {
		Trio tl = GetTrio();
		// (before the switch; the real leader -- boarded by the hero not leading, its
		// tick has it posing as the leader)
		if (tl) g_carSwitch = { g.sliced ? (int)g.savedLeader : tl.Leader(), GetTickCount() };
	}
	if (g.p2Joined && (event & 0xFFFF) == 0x0C09 && (uintptr_t)g_trioEventCaller == 0x53C5F4 && g_corkRide.hero >= 0)
		g_corkRide.launched = GetTickCount();
	if (g.p2Joined && (event & 0xFFFF) == 0x0C0D && (uintptr_t)g_trioEventCaller == 0x548757) {
		Trio tl = GetTrio();
		if (tl) g_telepherRider = tl.Leader();   // (the leader its tick saw: the rider)
	}
	if (g.p2Joined && ((event & 0xFFFF) == 0x4802 || (event & 0xFFFF) == 0x4803)) {
		Trio tl = GetTrio();
		if ((event & 0xFFFF) == 0x4803) {
			// Its sequence goes on only while the locked hero still waits (AI order 3):
			// LV2-1's turnstile reached without its zone 03C8 (Obelix already past it)
			// left Asterix's circle lock open through the platform's ride; the keg's
			// 4801 + 4803 then ended it as a sequence under way, and Obelix walking on
			// to the iron crates "set it back" (the lead to Asterix).
			Hero lh = tl && g_lock.hero >= 0 ? tl.GetHero(g_lock.hero) : Hero();
			bool waits = lh && (int)lh.Order() == 3;
			if (lh && !waits) {
				static int s_told = 0;
				unsigned picF = 0, nowF = 0;   // (a spinning platform's +8: the old rule's "under way" if bits 0/1 differ)
				for (size_t i = 0; i < g_lock.pic.nFlags; i++)
					if (*(uintptr_t*)g_lock.pic.flags[i].hook == 0x628E0C &&
					    ((g_lock.pic.flags[i].f ^ *(uint32_t*)(g_lock.pic.flags[i].hook + 8)) & 3)) {
						picF = g_lock.pic.flags[i].f;
						nowF = *(uint32_t*)(g_lock.pic.flags[i].hook + 8);
					}
				if (s_told++ < 20)
					Log("coop: %s's lock ended after its sequence (AI order %d) -- nothing goes on (platform +8 %X, at the lock %X)",
					    HeroName(g_lock.hero), (int)lh.Order(), nowF, picF);
			}
			EndLock(waits);
		} else if (tl) {
			int waiter = tl.Leader() == HERO_ASTERIX ? HERO_OBELIX : HERO_ASTERIX;
			NoteLock(waiter, "the companion told to wait");
			if (g_lock.hero == waiter) g_lock.wait = true;
			NoteFreedomLock(tl, waiter);
		}
	}
	SpyWalk& w = g_spyWalk;
	event &= 0xFFFF;
	char* cs = CameraService();
	if (event == 0x5121) {
		w.on = false;
		void* to = cs ? *(void**)(cs + 0x34) : nullptr;
		uintptr_t toVt = to ? *(uintptr_t*)to : 0;
		// Only a dialogue camera is replaced. A follow camera is the game's own
		// choice: after the belly slide of level 2 the agent's dialogue returns to
		// the area's follow camera, while the track from before the dialogue was
		// the slide's start, which does not turn (right stick left/right dead).
		bool dialogueCam = toVt == kFixTrackVtbl || toVt == kPlainCameraVtbl;
		if (cs && g_trackBeforeSpy && to != g_trackBeforeSpy && !dialogueCam && to)
			Log("coop: Roman agent's dialogue ended on camera track %p (vt %08X); kept (not a dialogue camera)", to,
			    (unsigned)toVt);
		if (cs && g_trackBeforeSpy && to != g_trackBeforeSpy && (dialogueCam || !to)) {
			Log("coop: Roman agent's dialogue ended on camera track %p (vt %08X); back to %p (vt %08X)", to,
			    to ? (unsigned)*(uintptr_t*)to : 0, g_trackBeforeSpy, (unsigned)*(uintptr_t*)g_trackBeforeSpy);
			*(void**)(cs + 0x34) = g_trackBeforeSpy;
			*(float*)(cs + 0xE0) = 1.0f;
			*(uint8_t*)(cs + 0xE4) = 0;
		}
		g_trackBeforeSpy = nullptr;
		return;
	}
	if (event == 0x5120 && cs) {
		void* cur = *(void**)(cs + 0x38);
		uintptr_t vt = cur ? *(uintptr_t*)cur : 0;
		if (!g_trackBeforeSpy && cur && vt != kFixTrackVtbl && vt != kPlainCameraVtbl) g_trackBeforeSpy = cur;
	}
	if (event == 0x5120 && g.p2Joined && g.shopping) CloseShopFor("a Roman agent's dialogue");
	if (event != 0x5120 || !param || w.resending || !g.p2Joined) return;
	memcpy(w.param, param, sizeof(w.param));
	w.trio = trio;
	w.on = true;
	w.tries = 0;
	w.since = w.lastProgress = GetTickCount();
	w.best = 1e30f;
	const float* spot = (const float*)(w.param + 4);
	Log("coop: Roman agent's dialogue: the leader walks to (%.1f %.1f %.1f)", spot[0], spot[1], spot[2]);
	// Set off by the player who is not leading (their hero at the spy, the
	// leader far away), the leader would walk there by path finding -- and get
	// stuck on the way more often than not (level 2 sector 1: the second spy,
	// set off by Obelix with Asterix back at the first one). That hero leads
	// for the dialogue instead, as if it had set the dialogue off in single
	// player: it walks the few steps, and the game brings the other one in as
	// the companion (0x48BA, AI order 13). The lead goes back afterwards.
	Trio t = GetTrio();
	int nl = CoopNonLeaderPlayer();
	if (!t || nl < 0 || g.suspended || (char*)trio != t.raw()) return;
	int lead = t.Leader();
	Hero leader = t.GetHero(lead), other = t.GetHero(nl);
	if (!leader || !other || g.held[lead] || g.held[nl]) return;
	Vector3 to(spot[0], spot[1], spot[2]);
	float dl = sqrtf((leader.Pos() - to).len2()), dn = sqrtf((other.Pos() - to).len2());
	if (dl > 6.0f && dn < 25.0f && dn < dl) {
		uint8_t* tr = (uint8_t*)t.raw();
		tr[0x2C] = (uint8_t)nl;
		tr[0x2D] = (uint8_t)lead;
		if (g.groupTookLead < 0) {
			g.groupTookLead = nl;
			g.groupPrevLeader = lead;
		}
		Log("coop: %s set the dialogue off; it leads for it (%s, %.0f units away, comes in as the companion)",
		    HeroName(nl), HeroName(lead), dl);
	}
}

// A boss the heroes run from (level 2 sector 10, level 3 sector 6) was woken by
// one player: the other is put next to them, so both start the run -- the other
// could otherwise stay behind, outside the run's section.
static void BringAlongForBossRun(Trio t) {
	int by = g.bossRunHero;
	g.bossRunHero = -1;
	if (by < 0 || by >= HERO_COUNT || !g.p2Joined || !t) return;
	int other = by == g.p1Hero ? g.p2Hero : g.p1Hero;
	Hero h = t.GetHero(by), o = other >= 0 ? t.GetHero(other) : Hero();
	if (!h || !o) return;
	float apart = sqrtf((h.Pos() - o.Pos()).len2());
	if (apart < 6.0f) return;
	// At the merchant: the visit ends first (it went along in the shop's
	// situation, the shop still up in that player's half).
	if (g.shopping && !g.shopForcedClosed && g.shopper == other) CloseShopFor("the other player's boss run");
	if (g.held[other]) {
		Log("coop: %s woke the boss; %s is busy with the game (not brought along)", HeroName(by), HeroName(other));
		return;
	}
	Vector3 to = h.Pos();
	to.y += 1.0f;
	SetHeroPosition(o, to);
	ProximityForgetMove(o.hook->node, &to.x);
	Log("coop: %s woke the boss %.0f units away from %s: %s brought along to (%.1f %.1f %.1f)", HeroName(by), apart,
	    HeroName(other), HeroName(other), to.x, to.y, to.z);
}

// A cutscene set off by one player far from the other (player 2's Obelix at the
// Roman agent while Asterix was back at the lifter): the scene and the script
// after it want both heroes there, and the AI could not find a path across the
// level -- a softlock. Once the cutscene camera is up, a hero more than 40 units
// away from the other is brought to the one the camera is looking at.
static void RegroupForCutscene(Trio t, Hero h1, Hero h2) {
	CKYellowPages* yp = YellowPages();
	char* srv = yp ? *(char**)((char*)yp + 0x68) : nullptr;
	// The cutscene closed a player's shop: the other player set it off (the boss
	// zone in LV2-10) -- the shopper comes along, as in single player the one who
	// crossed it would have been the whole trio.
	if (g.shopClosedHero >= 0 && GetTickCount() - g.shopClosedAt < 3000) {
		int shopper = g.shopClosedHero, other = shopper == g.p1Hero ? g.p2Hero : g.p1Hero;
		g.shopClosedHero = -1;
		Hero s = t.GetHero(shopper), o = other >= 0 ? t.GetHero(other) : Hero();
		if (s && o && !g.held[shopper] && (s.Pos() - o.Pos()).len2() >= 6.0f * 6.0f) {
			Vector3 to = o.Pos();
			to.y += 1.0f;
			SetHeroPosition(s, to);
			ProximityForgetMove(s.hook->node, &to.x);
			Log("coop: cutscene while %s was at the merchant: brought to %s at (%.1f %.1f %.1f)", HeroName(shopper),
			    HeroName(other), to.x, to.y, to.z);
			g.storyZoneHero = -1;
			return;
		}
	}
	// The cutscene's zone is bound to one hero and the other set it off (level 2's
	// boss: Asterix's zone crossed by player 2's Obelix, who the game then walks
	// to his marker): the zone's own hero is brought to the one that crossed it.
	int story = g.storyZoneHero;
	g.storyZoneHero = -1;
	if (story >= 0 && story < HERO_COUNT && (int)(GetTickCount() - g.storyZoneAt) < 5000) {
		int own = story == g.p1Hero ? g.p2Hero : g.p1Hero;
		Hero by = t.GetHero(story), mine = own >= 0 ? t.GetHero(own) : Hero();
		if (by && mine && !g.held[own] && (by.Pos() - mine.Pos()).len2() >= 40.0f * 40.0f) {
			Vector3 to = by.Pos();
			to.y += 1.0f;
			SetHeroPosition(mine, to);
			Log("coop: cutscene started by %s from %s's zone: %s brought to (%.1f %.1f %.1f)", HeroName(story),
			    HeroName(own), HeroName(own), to.x, to.y, to.z);
			return;
		}
	}
	// A hero the game has borrowed stays where the game put it (seated in the
	// cable car) -- except one sent to its place in a Roman agent's dialogue
	// (trio 0x48BA, AI order 13): the talk waits for it to get there.
	const int both[2] = { g.p1Hero, g.p2Hero };
	for (int idx : both)
		if (g.held[idx] && (int)t.GetHero(idx).Order() != kOrderDialoguePlace) return;
	if (!srv) return;
	Vector3 look = *(Vector3*)(srv + 0x5BC);
	float apart = (h1.Pos() - h2.Pos()).len2();
	if (apart < 40.0f * 40.0f) return;
	bool p1Near = (h1.Pos() - look).len2() <= (h2.Pos() - look).len2();
	Hero away = p1Near ? h2 : h1, close = p1Near ? h1 : h2;
	int farIdx = p1Near ? g.p2Hero : g.p1Hero, nearIdx = p1Near ? g.p1Hero : g.p2Hero;
	Vector3 to = close.Pos();
	to.y += 1.0f;
	SetHeroPosition(away, to);
	Log("coop: cutscene with the heroes %.0f units apart: %s brought to %s at (%.1f %.1f %.1f)", sqrtf(apart),
	    HeroName(farIdx), HeroName(nearIdx), to.x, to.y, to.z);
}

// Puts `who` just above `to`'s position (it lands next to it; overlapping heroes
// push apart). Hero position +kHero_pos and the node's matrix (+0x40) both
// carry the position.
static void WarpToPartner(int whoIdx, Hero who, int toIdx, Hero to) {
	Vector3 dst = to.Pos();
	dst.y += 1.0f;
	who.Pos() = dst;
	if (who.hook->node && who.hook->node->matrix) {
		float* hp = (float*)((char*)who.hook->node->matrix + 0x40);
		hp[0] = dst.x; hp[1] = dst.y; hp[2] = dst.z;
	}
	Log("coop: %s warped to %s (Back + A) at (%.1f %.1f %.1f)", HeroName(whoIdx), HeroName(toIdx), dst.x, dst.y,
	    dst.z);
}

// The merchant's menu (CKHkAsterixShop's state function FUN_004BFC50) asks a
// named input view of CKInput (FUN_00437FF0 with the name at 0x64F28C; view =
// [CKInput +0x40][index] + 0x10) for actions 2 / 0x12 (left), 3 / 0x13
// (right), 4 (buy) and 5 (back); the view maps each action to a binding of the
// value array (view +8, one byte per action) -- not the bindings the heroes'
// view uses, which is all player 2's input fills. Player 2's presses are
// written into the menu view's bindings, right after CKInput's update.
static void ShopInputFromP2(const PadState& p, InputManager im, int n) {
	static bool wasLeft = false, wasRight = false;
	bool left = (p.buttons & PAD_DPAD_LEFT) || p.lx < -0.5f, right = (p.buttons & PAD_DPAD_RIGHT) || p.lx > 0.5f;
	uint32_t press = 0;
	if (left && !wasLeft) press |= 1u << 2;
	if (right && !wasRight) press |= 1u << 3;
	if (p.pressed & PAD_A) press |= 1u << 4;
	// Back: B, only in the item menu (shop +8 bits 0-2 == 3). X is not "back":
	// during the merchant's words (state 2) it moves the words on (either player,
	// shared.cpp) and closed the menu as soon as it opened; on an item it left
	// the shop, where player 1's X does not.
	int state = -1;
	for (unsigned i = 0, n = GetNumLvlClassInstances(2, 172); i < n && i < 16 && state <= 0; i++) {
		char* sh = (char*)GetLvlObject(2, 172, i);
		if (sh) state = *(uint8_t*)(sh + 8) & 7;
	}
	if ((p.pressed & PAD_B) && state == 3) press |= 1u << 5;
	wasLeft = left;
	wasRight = right;
	char* input = im.raw();
	uint8_t* v = g.p2Values;   // the menu's input is answered from the shopper's values (shared.cpp)
	if (!press || !input) return;
	typedef unsigned(__fastcall* FindViewFn)(void* input, void* edx, const char* name);
	unsigned idx = ((FindViewFn)0x437FF0)(input, nullptr, (const char*)0x64F28C) & 0xFF;
	char** views = *(char***)(input + 0x40);
	if (idx == 0xFF || !views || !views[idx]) return;
	uint8_t* remap = *(uint8_t**)(views[idx] + 0x10 + 8);
	for (int a = 2; remap && a <= 5; a++)
		if ((press & (1u << a)) && remap[a] < n) {
			v[remap[a]] = 0xFF;
			LogV("coop: merchant: player 2's action %d -> binding %u", a, remap[a]);
		}
}

void CoopFrame() {
	DWORD now = GetTickCount();
	g.frames++;
	g.frameNo++;
	if (g.lastFpsTick == 0) {
		Log("coop: first frame");
		g.lastFpsTick = now;
	} else if ((int)(now - g.lastFpsTick) >= 10000) {
		Log("coop: %.1f fps", g.frames * 1000.0 / (double)(now - g.lastFpsTick));
		g.frames = 0;
		g.lastFpsTick = now;
	}

	PadsPoll();
	PadsSetKeyboardOwnedByP2(g.p2Joined && g.p2Pad == kKeyboardPad);

	// The combo cheat's two hits ("a hit landed", 0x4C0E, through the hero's
	// sendEvent), once the HUD's bar has seen the hits one short of full.
	if (g.comboHitsHero >= 0 && now - g.comboHitsAt > 600) {
		int who = g.comboHitsHero;
		g.comboHitsHero = -1;
		Trio t = GetTrio();
		Hero h = t ? t.GetHero(who) : Hero();
		if (h) {
			typedef int(__fastcall* SendFn)(void* self, void* edx, int event, void* param);
			int prev = IndivEnter(who);
			for (int k = 0; k < 2; k++) ((SendFn)(*(void***)h.raw())[5])(h.raw(), nullptr, 0x4C0E, nullptr);
			IndivLeave(prev);
		}
	}
	IndivFrame();
	ExperimentalFrame();
	SendDeferredSwitch();
	TracePotionFrame();
	SoundSetP2Listener();
	CheckMarker(now);
	{
		InputManager im = GetInputManager();
		int n = im.NumActions();
		if (n > (int)kMaxActions) n = (int)kMaxActions;
		PadIdentFrame(im.Values(), n);
	}

	CKYellowPages* yp = YellowPages();
	int level = (yp && yp->gameManager)
		? *(int*)((char*)yp->gameManager + ax::kGM_currentLevel) : -1;
	if (level != g.lastLevel) {
		Log("coop: level %d -> %d", g.lastLevel, level);
		g.lastLevel = level;
		g_trackBeforeSpy = nullptr;   // the old level's camera tracks are gone
		g_corkRide = {};              // (and its corkscrews)
		g_autoRide = {};
		g.groupTookLead = g.groupPrevLeader = -1;
		g_spyWalk.on = false;
		g.joinHintPending = level > 0;
		g.spinHints = 0;
		g.machineReq = nullptr;
		if (g.p2Joined) {
			// The old hero objects are gone; there is nothing to hand back.
			ForgetP2();
			Log("coop: player 2 dropped for the level change (press Start to rejoin)");
		}
	}

	Trio t = GetTrio();
	TrackStuckModes(t);
	WatchSectors(t, now);   // every frame: the heroes' sectors are tracked in single player too
	MakeCutscenesSkippable();
	if (g_level0SkipAt) {   // level 0: the film after a skipped intro is skipped as it starts
		CKYellowPages* ypc = YellowPages();
		char* cin = ypc ? *(char**)((char*)ypc + 0x80) : nullptr;
		char** scenes = cin ? *(char***)(cin + 0x10) : nullptr;
		unsigned ns = cin ? *(unsigned*)(cin + 0x14) : 0;
		for (unsigned i = 0; level == 0 && scenes && i < ns && i < 512; i++) {
			uint16_t f = *(uint16_t*)(scenes[i] + 0x20);
			if (scenes[i] == g_level0Skipped || !(f & 1) || (f & 0x180) == 0x80 || *(int8_t*)(scenes[i] + 0x30) < 0) continue;
			Log("coop: level 0: the cutscene after the skipped one (%p) skipped as well", scenes[i]);
			((void(__fastcall*)(char*, void*))kSkipScene)(scenes[i], nullptr);
			g_level0SkipAt = 0;
			break;
		}
		if (level != 0 || (g_level0SkipAt && now - g_level0SkipAt > 6000)) g_level0SkipAt = 0;
	}
	{   // diagnostics: the streamed sector (level object = yellow pages +0x90, current +0x24, next +0x34)
		static int lastSector = -99;
		char* lvl = yp ? *(char**)((char*)yp + 0x90) : nullptr;
		int sec = lvl ? *(int*)(lvl + 0x24) : -99;
		if (sec != lastSector && t) {
			Hero ha = t.GetHero(HERO_ASTERIX), ho = t.GetHero(HERO_OBELIX);
			Log("coop: sector %d -> %d (next %d); leader %s; Asterix (%.0f %.0f %.0f) Obelix (%.0f %.0f %.0f)",
			    lastSector, sec, lvl ? *(int*)(lvl + 0x34) : -99, HeroName(t.Leader()), ha ? ha.Pos().x : 0,
			    ha ? ha.Pos().y : 0, ha ? ha.Pos().z : 0, ho ? ho.Pos().x : 0, ho ? ho.Pos().y : 0, ho ? ho.Pos().z : 0);
		}
		if (sec != lastSector) g_trackBeforeSpy = nullptr;   // no dialogue spans a sector change
		lastSector = sec;
	}
	// The cutscene hold counts down whether or not player 2 is in: a player 2
	// dropped by a level change during a cutscene left it frozen above zero, and
	// every Start after that was refused as "cutscene".
	if (CutsceneBars()) g.cutsceneHold = kCutsceneHoldFrames;
	else if (g.cutsceneHold > 0) g.cutsceneHold--;
	// The other player sets off something that takes the screen while one shops
	// (a save point's menu: bars and game manager +0x5C bit 20; the shop shows
	// neither): the visit ends first, as for a Roman agent's dialogue. Kept open,
	// its camera stayed on player 2's half after the save menu and player 1's
	// menu input was gone (xxl_coop.broken_merchant.log).
	if (g.p2Joined && g.shopping && !g.shopForcedClosed && CutsceneBars()) CloseShopFor("the save menu (or a cutscene)");
	{
		static bool dumped = false;
		if (!dumped && t && getenv("XXL_DUMPBIND")) { dumped = true; TraceDumpBindings(); }
	}
	WatchControllerPreset();
	WatchBindings();
	if (!g.p2Joined) {
		// Without player 2 the hero that is not leading is the AI companion. A
		// save made during co-op stores player 2's hero in the player variant:
		// loaded alone, Obelix then mirrored player 1's every move.
		if (t && CoopGameplayLive()) {
			int ci = t.Leader() == HERO_ASTERIX ? HERO_OBELIX : t.Leader() == HERO_OBELIX ? HERO_ASTERIX : -1;
			Hero c = ci >= 0 ? t.GetHero(ci) : Hero();
			uint8_t f = c ? *F18(c) : 0;
			if (c && HeroModePlayable(f) && HeroIsPlayerVariant(f) && !CutsceneBars()) {
				*F18(c) = HeroAiVariant(f);
				Log("coop: %s was a player hero without player 2 (from the save?) -- AI companion again (f18 %02X -> %02X)",
				    HeroName(ci), f, *F18(c));
			}
		}
		ShowJoinHint(t);
		TryJoin(t);
		return;
	}

	const PadState& p = PadGet(g.p2Pad);
	if (!p.connected) { Release("pad disconnected"); return; }
	if (PadIdentRole(g.p2Pad) == PAD_ROLE_GAME) {
		Release("that pad also drives player 1 -- player 2 has to use the other controller");
		return;
	}
	// Start and Back are left alone during a cutscene: Start may be what skips it.
	bool inCutscene = g.cutsceneHold > 0 || CutsceneBars() || g.shopping;
	// ... and leaving waits for the end of a scripted ride (the belly slide, the
	// boat): handed back in the middle of it, both heroes stood stuck.
	if (!inCutscene && (p.pressed & PAD_START)) {
		if (g.suspended) {
			static DWORD s_told = 0;
			if (GetTickCount() - s_told > 3000) { s_told = GetTickCount(); Log("coop: player 2's Start ignored -- a scripted ride is on"); }
		} else {
			Release("pressed Start");
			return;
		}
	}
	const uint16_t kFace = PAD_A | PAD_B | PAD_X | PAD_Y;
	if (!inCutscene && (p.buttons & PAD_BACK) && (p.pressed & kFace)) g.p2BackCombo = true;
	if (!(p.buttons & PAD_BACK)) g.p2BackCombo = false;
	if (!inCutscene && (p.buttons & PAD_BACK) && !g.p2BackCombo) {
		if (g.p2BackHeldAt == 0) g.p2BackHeldAt = now;
		// Individual heroes: Back shows player 2's HUD, held as long as they like
		// (Start leaves).
		else if ((int)(now - g.p2BackHeldAt) >= g_config.dropHoldMs && !IndivActive() && !g.suspended) { Release("held Back"); return; }
	} else {
		g.p2BackHeldAt = 0;
	}

	Hero h1 = t ? t.GetHero(g.p1Hero) : Hero();
	Hero h2 = t ? t.GetHero(g.p2Hero) : Hero();
	if (!h1 || !h2) {
		ForgetP2();
		Log("coop: heroes disappeared, player 2 dropped");
		return;
	}

	// The mod's combos: hold the right stick button (player 2 also Back) and
	//   A  your hero joins your partner's (lost, stuck, fell behind)
	//   B  take your hero back from the game ("wait here" orders that a sequence
	//      played out of order never ends); locked by the level (a light pillar,
	//      a cable car, the spinning platform), its sequence undone as for a fallen
	//      partner and the hero put on its last solid ground
	//   Y  [cheats] the magic potion: filled, or emptied when it runs
	//   X  [cheats] your hero becomes the leader
	//   D-pad up / down  [cheats] Asterix gets the nearest torch, lit / throws it away
	//   D-pad left  [cheats] every combo learned, your combo bar full
	// Player 1's controller is the one the game reads (pad identification) --
	// until that is known (it can take a while), any other connected pad.
	{
		int p1pad = Player1Pad();
		for (int i = 0; p1pad < 0 && i < kMaxPads; i++)
			if (i != g.p2Pad && i != kKeyboardPad && PadGet(i).connected) p1pad = i;
		const PadState& q = PadGet(p1pad);
		bool mod1 = p1pad >= 0 && p1pad != g.p2Pad && q.connected && (q.buttons & PAD_RTHUMB);
		bool mod2 = (p.buttons & (PAD_BACK | PAD_RTHUMB)) != 0;
		for (int who = 0; who < 2 && !inCutscene; who++) {
			bool held = who == 0 ? mod1 : mod2;
			uint16_t pressed = who == 0 ? q.pressed : p.pressed;
			if (!held || !(pressed & (kFace | PAD_DPAD_UP | PAD_DPAD_DOWN | PAD_DPAD_LEFT))) continue;
			int me = who == 0 ? g.p1Hero : g.p2Hero, mate = who == 0 ? g.p2Hero : g.p1Hero;
			Hero hm = who == 0 ? h1 : h2, hp = who == 0 ? h2 : h1;
			const char* player = who == 0 ? "player 1" : "player 2";
			if ((pressed & PAD_A) && !g.suspended && !g.held[me] && !g.held[mate])
				WarpToPartner(me, hm, mate, hp);
			if ((pressed & PAD_B) &&
			    (g.held[me] || (!g.suspended && (LockedByLevel(me, hm) || (g_seqHero == me && SequenceUnderWay()))))) {
				UnlockByPlayer(t, me, player);
			} else if ((pressed & PAD_B) && !g.suspended) {
				StartAiCycle(me, "R3 + B");
			}
			if ((pressed & PAD_Y) && g_config.cheats) {
				bool running = PotionGauge() > 0.01f;
				SetPotionGauge(running ? 0.001f : 300.0f);   // nearly empty: the game's own countdown ends it
				Log("coop: cheat: magic potion %s by %s", running ? "emptied" : "filled", player);
			}
			if ((pressed & PAD_DPAD_UP) && g_config.cheats) CoopTorchCheat(true);
			if ((pressed & PAD_DPAD_DOWN) && g_config.cheats) CoopTorchCheat(false);
			if ((pressed & PAD_DPAD_LEFT) && g_config.cheats) {
				int prevStats = IndivEnter(me);   // individual heroes: this hero's own combo bar
				if (CombosCheat(true)) { g.comboHitsHero = me; g.comboHitsAt = GetTickCount(); }
				IndivLeave(prevStats);
				Log("coop: cheat: every combo learned and %s's combo bar filled by %s", HeroName(me), player);
			}
			if ((pressed & PAD_X) && g_config.cheats && t.Leader() != me && !g.suspended) {
				typedef int(__fastcall* SendEventFn)(void* self, void* edx, int event, void* param);
				int ev = me == HERO_ASTERIX ? 0x4800 : 0x4801;
				Log("coop: cheat: %s makes %s the leader (trio event %04X)", player, HeroName(me), ev);
				((SendEventFn)(*(void***)t.raw())[5])(t.raw(), nullptr, ev, nullptr);
			}
		}
	}

	// Scripted modes belong to the game. So does a group action either player
	// starts (a launcher started by the non-leading player too: the game flies
	// the companion along and runs its own camera, as when the leader starts it)
	// (situation 13: a lifter waiting for the trio, and the like): it sends "the
	// companion" to a marker and waits for it, which only an AI companion does.
	bool groupAction = Situation(h1) == kSituationGroupAction || Situation(h2) == kSituationGroupAction;
	// The merchant (CKHkAsterixShop, 2,172): trio 0x5123 opens it, its state
	// (+8 bits 0-2) is not 0 while its camera and menu are up. The menu is
	// player 1's, full screen.
	bool shopping = false;
	for (unsigned i = 0, n = GetNumLvlClassInstances(2, 172); i < n && i < 16 && !shopping; i++) {
		char* s = (char*)GetLvlObject(2, 172, i);
		shopping = s && (*(uint8_t*)(s + 8) & 7) != 0;
	}
	if (shopping && !g.shopping) {
		// (Opened by the hero not leading: known since its tick -- CoopAfterLifeTick.)
		g.shopper = g.shopHero >= 0 ? g.shopHero : t.Leader();
		g.shopHero = g.shopper;
		Log("coop: %s is at the merchant -- in that player's half; the other player plays on", HeroName(g.shopper));
	}
	if (g.shopCloseCheck > 0 && --g.shopCloseCheck == 0) {
		CKYellowPages* yp = YellowPages();
		char* gm = yp ? (char*)yp->gameManager : nullptr;
		char* sh = g.closedShop;
		if (gm && sh) Log("coop: after the merchant's forced close: game manager +0x5C %08X; shop state %d pose %d flags %02X/%02X",
			*(unsigned*)(gm + 0x5C), *(uint8_t*)(sh + 8) & 7, (*(uint8_t*)(sh + 8) >> 3) & 7, *(uint8_t*)(sh + 8) & 0xC0, *(uint8_t*)(sh + 0x1CA));
	}
	if (g.shopForcedClosed && !shopping && Situation(h1) != kSituationShop && Situation(h2) != kSituationShop)
		g.shopForcedClosed = false;
	if (!shopping && g.shopping) {
		Log("coop: %s is done at the merchant", HeroName(g.shopper));
		g.shopLeadBack = true;
	}
	// The lead back once the shopper is out of the shop's situation too: given
	// back on the goodbye's last frames, the hero reset through the AI went back
	// in (0x48EF) as the companion -- the visit ran again as the leader's.
	WatchShopCamera(t);
	if (g.shopHero >= 0 && !shopping && !ShopBusy(t)) g.shopHero = -1;   // the game's camera back to the leader's
	if (g.shopLeadBack && !ShopBusy(t)) {
		g.shopLeadBack = false;
		if (!g.suspended) GiveBackLead(t);   // no suspension to resume from any more
	}
	g.shopping = shopping;
	// The shop's menu turns the gameplay bindings off (FUN_00437F10, thiscall(CKInput,
	// group, on), ret 8: group 0 gameplay, group 1 menus) -- player 1's controls,
	// which come from those bindings (player 2's are the mod's own values). With
	// player 2 at the merchant, player 1's are back on (its menu still answers
	// player 2 alone: shared.cpp).
	if (shopping && g.p2Joined && g.shopper == g.p2Hero) {
		InputManager im = GetInputManager();
		if (im && im.Binding(0) && !*(uint8_t*)(im.Binding(0) + 4)) {
			((void(__fastcall*)(char*, void*, int, int))0x437F10)(im.raw(), nullptr, 0, 1);
			static int s_told;
			if (s_told++ < 8) Log("coop: player 1's controls back on during player 2's shopping");
		}
	}
	{
		// player 1's gameplay bindings on / off (FUN_00437F10 group 0), when it changes
		InputManager im = GetInputManager();
		static int s_bindOn = -1;
		int on = im && im.Binding(0) ? *(uint8_t*)(im.Binding(0) + 4) != 0 : -1;
		if (on != s_bindOn && on >= 0) Log("coop: player 1's gameplay bindings %s", on ? "on" : "off");
		s_bindOn = on;
	}
	if (!g.suspended) TwisterNearness(t, h1, h2);
	// The fused tornado is one: one screen while it lasts (the heroes stay the
	// players' -- made the AI companion, the partner's Twister ended at once).
	bool fusion = FusionOn(h1, h2);
	if (fusion != g.oneScreen) {
		g.oneScreen = fusion;
		Log("coop: Twister Fusion %s -- split screen %s", fusion ? "on" : "over", fusion ? "off" : "back");
	}
	// Cutscenes: the letterbox bars, held a few frames after they go so a scene
	// handing over to the next one does not flicker co-op on and off.
	LogCutsceneSignals();
	bool cinematic = g.cutsceneHold > 0;
	// Asterix alone on the machine gun (mode 2, Obelix's player free): co-op and
	// the split screen go on; one screen only once Obelix is at the gun too.
	bool gunSolo = GunSolo(t);
	if (gunSolo != g.gunSolo) {
		g.gunSolo = gunSolo;
		Log("coop: Asterix %s the machine gun on his own%s", gunSolo ? "is on" : "is off",
		    gunSolo ? " -- co-op and the split screen go on" : "");
		if (!gunSolo && HeroModePlayable(t.GetHero(HERO_ASTERIX).Flags18()) && !g.suspended) GiveBackLead(t);
	}
	if (g.gunJoinReq) JoinGun(t, "X at the handles");
	if (g.dying >= 0 && Health() > 0) {
		Log("coop: health back (%d) -- %s's death is over", Health(), HeroName(g.dying));
		g.dying = -1;
		g.dyingLeads = false;
	}
	bool scripted = g.dying >= 0 || (!HeroModePlayable(h1.Flags18()) && !(gunSolo && g.p1Hero == HERO_ASTERIX)) ||
	                (!HeroModePlayable(h2.Flags18()) && !(gunSolo && g.p2Hero == HERO_ASTERIX)) ||
	                (h1.Busy() && !IndivFallen(g.p1Hero)) || (h2.Busy() && !IndivFallen(g.p2Hero)) || groupAction ||
	                cinematic;
	if (scripted != g.suspended) {
		g.suspended = scripted;
		Log("coop: %s (%s %s / %s %s%s%s%s)", scripted ? "suspended for a scripted mode" : "resumed",
			HeroName(g.p1Hero), ModeName(HeroMode(h1.Flags18())),
			HeroName(g.p2Hero), ModeName(HeroMode(h2.Flags18())),
			cinematic ? ", cutscene" : "", groupAction ? ", group action" : "", shopping ? ", merchant" : "");
		CoopLogState(scripted ? "suspend" : "resume");
		// A group action the non-leading player started is over: the lead goes
		// back where it was (single player never hands it over for a launcher).
		if (!scripted) GiveBackLead(t);
	}
	{
		CKYellowPages* yp = YellowPages();
		char* cs = yp ? *(char**)((char*)yp + 0x68) : nullptr;
		char* track = cs ? *(char**)(cs + 0x38) : nullptr;
		bool cineCam = track && *(uintptr_t*)track == 0x6245BC;   // plain CKCamera: a cutscene shot
		if (!cinematic) g.cineRegrouped = false;
		else if (!g.cineRegrouped && cineCam) {
			g.cineRegrouped = true;
			RegroupForCutscene(t, h1, h2);
		}
	}
	BringAlongForBossRun(t);
	ServeMachineRequest(t);
	WatchLifterHint(t);
	WatchPendingLead(t);
	WatchTelepherRider(t);
	WatchFreedomBeam(t);
	WatchCorkLaunch(t);
	FreedomFrame();
	WatchUnfinishedSequence(t);
	WatchStrandedCar();
	WatchSpyWalk(t, now);
	// While suspended, the hero that is not leading is the game's companion, in
	// whatever mode the game put it -- whichever player it belongs to. Rides only
	// work that way: on the belly slide (mode 3) Obelix leads and Asterix rides
	// along as the AI variant, whose handler carries him with Obelix; left in the
	// player variant he stayed at the top of the slide.
	// A fallen hero (individual heroes) is not: as the AI companion it got out of
	// its death (situation 0xFF, AI life) and walked toward the leader for the
	// frames a pause menu's bars took to go -- into a gap if one was on the way
	// (20261014-190940 log: each pause).
	if (g.suspended) {
		const int both[2] = { g.p1Hero, g.p2Hero };
		for (int idx : both) {
			Hero h = idx == g.p1Hero ? h1 : h2;
			uint8_t f = *F18(h);
			if (idx != t.Leader() && HeroIsPlayerVariant(f) && !IndivFallen(idx)) {
				*F18(h) = HeroAiVariant(f);
				Log("coop: %s is the game's companion while suspended (f18 %02X -> %02X)", HeroName(idx), f,
				    *F18(h));
			}
		}
	}

	InputManager im = GetInputManager();
	int n = im.NumActions();
	if (n > (int)kMaxActions) n = (int)kMaxActions;
	if (n > 0 && BuildP2Actions(g.p2Pad, g.p2Values, n)) {
		// The combo bytes after the actions belong to the recognizer below and
		// carry over from frame to frame.
		if (n + g.p2Combos < kP2ValuesSize) memset(g.p2Values + n + g.p2Combos, 0, kP2ValuesSize - n - g.p2Combos);
		g.p2ValuesReady = true;
		// Hold timers, as CKInput_UpdateBinding keeps them at binding +0x18.
		uint8_t held[kP2ValuesSize] = {};
		BuildP2Held(g.p2Pad, held, n);
		float t = GetTickCount() / 1000.0f;
		for (int i = 0; i < n; i++) {
			if (!held[i]) { g.p2HoldStart[i] = -1.0f; g.p2HoldTime[i] = 0.0f; continue; }
			if (g.p2HoldStart[i] <= 0) g.p2HoldStart[i] = t;
			g.p2HoldTime[i] = t - g.p2HoldStart[i] + 0.001f;   // non-zero from the first frame
		}
		// Player 2's combos (A A X B and so on), recognised the way the game
		// recognises player 1's.
		char* binds = im ? *(char**)(im.raw() + 0x18) : nullptr;
		if (binds) {
			static char comboBindings[kP2ValuesSize * 0x20];
			memcpy(comboBindings, binds, n * 0x20);
			for (int i = 0; i < n; i++) *(float*)(comboBindings + i * 0x20 + 0x18) = g.p2HoldTime[i];
			int c = P2RecognizeCombos(g.p2Values, n, comboBindings);
			if (c > 0 && n + c <= kP2ValuesSize) {
				g.p2Combos = c;
				for (int k = 0; k < c; k++)
					if (g.p2Values[n + k] == 0xFF) Log("coop: player 2's combo %d (action %02X) entered", k, 0x10 + k);
			}
		}
		// The merchant's menu reads the game's one input: either player can
		// browse it (player 2's Obelix may be the one who opened it).
		if (g.shopping) ShopInputFromP2(p, im, n);
	}

	// Heroes the game has borrowed: a scripted AI order given to a player's hero
	// while it is not the leader (Asterix riding the cable car Obelix pulls,
	// waiting on the spinning platform...) is the AI's to carry out. Back to its
	// player when the order is over or the hero leads again -- no timeout (an old
	// 2 min one let go of Asterix in the cable car during a long pause, and the car
	// was never usable again); R3 + B undoes a lock at any time.
	const int players[2] = { g.p1Hero, g.p2Hero };
	for (int idx : players) {
		Hero h = t.GetHero(idx);
		if (!h) continue;
		int order = (int)h.Order();
		bool leads = t.Leader() == idx;
		if (!g.held[idx] && !leads && order != g.lastOrder[idx] && IsScriptedOrder(order)) {
			Hold(t, idx, "new order");
		} else if (g.held[idx] && ((leads && order != AI_HANG_ON_BEAM) || order == AI_FOLLOW_LEADER)) {   // (riding the beam leading: its end)
			g.held[idx] = false;
			if (g_corkRide.hero == idx) g_corkRide = {};
			Log("coop: %s back to its player (%s)", HeroName(idx), leads ? "leads again" : "order done");
		}
		g.lastOrder[idx] = (uint8_t)order;
	}

	if (!g.suspended) {
		EnforcePlayer(t, g.p2Hero);
		EnforcePlayer(t, g.p1Hero);
	}
	P2CamFrame();
	KlusterSetP2Camera();
}

// ---------------------------------------------------------------------------
// Per-hero Life tick
// ---------------------------------------------------------------------------

// Torch for Asterix (cheat). The level's carryable torches are CKHkTorch (2,32;
// level 2's is at -744 514 -981). Near one, the leader is offered it every
// frame (torch tick -> trio 0xC06 -> trio +0x4C, cleared again by the trio),
// and the action button takes it (hero +0x3EC bit 0x10 -> bit 0x2000 ->
// situation 5); Asterix then carries it (hero +0x464 = the torch, torch +0x3E
// state 1, fire level in bits 2-4). Given here by making that offer and that
// press inside Asterix's own tick, from any distance, then lit (0x5128).
// Thrown away with the torch's 0x513C, as a level restart does.
struct TorchCheat { int want = 0; int frames = 0; char* torch = nullptr; } g_torch;

static char* NearestTorch(Hero a) {
	char* best = nullptr;
	float bd = 1e30f;
	unsigned n = GetNumLvlClassInstances(2, 32);
	for (unsigned i = 0; i < n; i++) {
		char* c = (char*)GetLvlObject(2, 32, i);
		char* node = c ? *(char**)(c + 0x10) : nullptr;
		char* fr = node ? *(char**)(node + 4) : nullptr;
		if (!fr) continue;
		Vector3 p(((float*)(fr + 0x80))[0], ((float*)(fr + 0x80))[1], ((float*)(fr + 0x80))[2]);
		float d = (p - a.Pos()).len2();
		if (d < bd) { bd = d; best = c; }
	}
	return best;
}

void CoopTorchCheat(bool give) {
	Trio t = GetTrio();
	Hero a = t ? t.GetHero(HERO_ASTERIX) : Hero();
	if (!a) return;
	if (give) {
		if (*(char**)(a.raw() + 0x464)) { Log("coop: cheat: Asterix already has a torch"); return; }
		g_torch.torch = NearestTorch(a);
		g_torch.want = g_torch.torch ? 1 : 0;
		g_torch.frames = 0;
		Log("coop: cheat: torch %p for Asterix", g_torch.torch);
		return;
	}
	char* c = *(char**)(a.raw() + 0x464);
	if (!c) { Log("coop: cheat: Asterix has no torch"); return; }
	typedef int(__fastcall* SendFn)(void*, void*, int, void*);
	((SendFn)(*(void***)c)[5])(c, nullptr, 0x513C, nullptr);
	*(char**)(a.raw() + 0x464) = nullptr;
	*(uint32_t*)(a.raw() + 0x3EC) &= ~0x400000u;
	Log("coop: cheat: Asterix threw torch %p away", c);
}

// The "take" press: Hero_ProcessPlayerInput puts action 5 into hero +0x3EC
// bit 0x10, then FUN_00499CF0 (both heroes' player-state handlers; thiscall,
// one argument, ret 4) turns it into +0x3EC bit 0x2000 when something is
// offered (FUN_0049A080), and the situation chooser takes the offer. While the
// cheat is pending the press is added at that handler's entry.
typedef void(__fastcall* TakeFn)(char* hero, void* edx, int arg);
const uintptr_t kTakeHandler = 0x499CF0;
const uint8_t   kTakePrologue[7] = { 0x6A, 0xFF, 0x68, 0x48, 0xD7, 0x61, 0x00 };
TakeFn g_origTake = nullptr;

// Machines (the machine gun, the boat). A machine's tick offers itself to the
// trio leader in reach (trio 0x4A01 gun / 0x4901 boat: trio +0x48 = machine,
// +0x3F4 bits 10-11 = 2 / 1); X takes it in FUN_0049A080 (called from here when
// X is down, hero +0x3EC bit 0x10) -- for Asterix only (hero vtable +0x3C == 0):
// trio 0x4809 (gun, mode 2) or 0x4807 (boat, mode 1), which act on Asterix with
// Obelix as the AI companion who walks to his place. In co-op Obelix's X at a
// machine is a request too (served next frame, ServeMachineRequest); his swing
// is held back.
// The machine gun in use (trio +0x48, CKHkMachinegun) and Obelix's place at its
// handles: the gun's 0x4A05 answer, gun +0xD0 (a node: +4 -> matrix, place
// +0x80, direction +0x70), where 0x4809 sends the companion (AI order 7).
const uintptr_t kMachinegunVtbl = 0x6265E0;
static char* TrioGun(Trio t) {
	char* gun = t ? *(char**)(t.raw() + 0x48) : nullptr;
	return gun && *(uintptr_t*)gun == kMachinegunVtbl ? gun : nullptr;
}
static bool GunHandles(Trio t, Vector3* pos, Vector3* dir) {
	char* gun = TrioGun(t);
	char* spot = gun ? *(char**)(gun + 0xD0) : nullptr;
	char* m = spot ? *(char**)(spot + 4) : nullptr;
	if (!m) return false;
	if (pos) *pos = *(Vector3*)(m + 0x80);
	if (dir) *dir = *(Vector3*)(m + 0x70);
	return true;
}

static void NoteMachineUse(char* hero) {
	if (!g.p2Joined || g.suspended) return;
	Trio t = GetTrio();
	Hero o = t ? t.GetHero(HERO_OBELIX) : Hero();
	if (!o || hero != o.raw() || (g.p1Hero != HERO_OBELIX && g.p2Hero != HERO_OBELIX)) return;
	uint32_t ec = *(uint32_t*)(hero + 0x3EC), f0 = *(uint32_t*)(hero + 0x3F0);
	if (g.gunSolo && (ec & 0x10) && !g.held[HERO_OBELIX]) {
		// Asterix is on the machine gun: X at its handles joins him there.
		Vector3 spot;
		bool known = GunHandles(t, &spot, nullptr);
		// (The AI's walk ends at the handles about 4 units off that target.)
		LogV("coop: Obelix's X with Asterix on the gun: handles target %s (%.1f %.1f %.1f), Obelix %.1f away",
		     known ? "at" : "unknown", spot.x, spot.y, spot.z, known ? sqrtf((o.Pos() - spot).len2()) : -1.0f);
		if (known && (o.Pos() - spot).len2() < 6.0f * 6.0f) {
			*(uint32_t*)(hero + 0x3EC) &= ~0x10u;
			g.gunJoinReq = true;
		}
		return;
	}
	if (!(ec & 0x10) || (ec & 0xC0000) || (f0 & 0x804129) || (*(uint32_t*)(hero + 0x384) & 0x200000)) return;
	if (*(uint8_t*)(hero + 0x3E9) != 0) return;
	char* tr = t.raw();
	char* machine = *(char**)(tr + 0x48);
	int kind = (*(uint32_t*)(tr + 0x3F4) >> 10) & 3;
	if (!machine || (kind != 1 && kind != 2) || *(uint32_t*)(tr + 0x4C)) return;
	*(uint32_t*)(hero + 0x3EC) &= ~0x10u;
	g.machineReq = machine;
	g.machineKind = kind;
	g.machineReqAt = GetTickCount();
}

// The world's pause (FUN_004FFCD0, thiscall on the game manager, 1 = on): every
// Life of the enemy groups gets its "paused" bit (+4 bit 1, skipped by the
// bundle tick) -- set by the merchant's opening (and a spy's talk), cleared when
// it closes. In co-op the merchant is one player's: the other plays on, so the
// world goes on too.
typedef void(__fastcall* FreezeFn)(char* gm, void* edx, int on);
const uintptr_t kFreezeWorld = 0x4FFCD0;
const uint8_t   kFreezePrologue[5] = { 0x53, 0x8B, 0x5C, 0x24, 0x08 };
FreezeFn g_origFreeze = nullptr;

static bool MerchantOpening() {
	if (g.shopForcedClosed) return false;
	for (unsigned i = 0, n = GetNumLvlClassInstances(2, 172); i < n && i < 16; i++) {
		char* sh = (char*)GetLvlObject(2, 172, i);
		if (sh && (*(uint8_t*)(sh + 8) & 7) != 0) return true;
	}
	Trio t = GetTrio();
	for (int i = 0; t && i < 2; i++)
		if (Situation(t.GetHero(i)) == kSituationShop) return true;
	return false;
}

static void __fastcall FreezeWorldHook(char* gm, void* edx, int on) {
	if (on && g.p2Joined && !g.suspended && MerchantOpening()) {
		Log("coop: the merchant's pause of the world skipped -- the other player plays on");
		return;
	}
	g_origFreeze(gm, edx, on);
}

static void __fastcall TakeHook(char* hero, void* edx, int arg) {
	if (g_torch.want) {
		Trio t = GetTrio();
		Hero a = t ? t.GetHero(HERO_ASTERIX) : Hero();
		if (a && hero == a.raw()) *(uint32_t*)(hero + 0x3EC) |= 0x10;
	}
	NoteMachineUse(hero);
	g_origTake(hero, edx, arg);
}

// The magic potion. Hero_ReadCommandBlock (0x49A350, both heroes' player
// states, thiscall, no arguments) sets hero +0x388 bit 0x4 -- "potion on",
// which the movement code (speed +0x31C instead of +0x310, ...) and the rest
// read -- only for a hero whose vtable slot 15 returns 0, which is Asterix
// (Obelix's returns his class id): single player never lets Obelix drink.
// A player's Obelix gets the bit too while the gauge (game manager +0x40 ->
// +0x9C) is above zero ([coop] obelix_potion).
typedef void(__fastcall* ReadCmdFn)(char* hero, void* edx);
const uintptr_t kReadCommandBlock = 0x49A350;
const uint8_t   kReadCmdPrologue[6] = { 0x51, 0x56, 0x8B, 0xF1, 0x8B, 0x06 };
ReadCmdFn g_origReadCmd = nullptr;

static void __fastcall ReadCmdHook(char* hero, void* edx) {
	g_origReadCmd(hero, edx);
	if (!g_config.obelixPotion || !g.p2Joined) return;
	Trio t = GetTrio();
	Hero o = t ? t.GetHero(HERO_OBELIX) : Hero();
	if (!o || hero != o.raw() || (g.p1Hero != HERO_OBELIX && g.p2Hero != HERO_OBELIX)) return;
	CKYellowPages* yp = YellowPages();
	char* gm = yp ? (char*)yp->gameManager : nullptr;
	char* sg = gm ? *(char**)(gm + 0x40) : nullptr;
	if (sg && *(float*)(sg + 0x9C) > 0.0f) *(uint32_t*)(hero + 0x388) |= 4;
}

// The HUD's action prompt ("Activate" on a launcher...). FUN_0049A1A0, run by
// every player state of both heroes (thiscall, no arguments), writes the
// prompt for what its hero is offered into game manager +0xCC bits 4-7 (5 =
// on a launcher, hero +0x3F0 bit 0x4000; 0xE = nothing). With two player
// heroes the last one ticked wins: the leader's "nothing" wiped player 2's
// "Activate" every frame. Within a frame a prompt from either player's hero
// stays (the leader's first).
typedef void(__fastcall* PromptFn)(char* hero, void* edx);
const uintptr_t kHeroPrompt = 0x49A1A0;
const uint8_t   kHeroPromptPrologue[8] = { 0xA1, 0xF4, 0x21, 0x66, 0x00, 0x56, 0x8B, 0xF1 };
const unsigned  kPromptNone = 0xE;
PromptFn g_origPrompt = nullptr;
struct { uint32_t frame = 0; unsigned kind = kPromptNone; bool leader = false; int hero = -1; bool both = false; } g_prompt;

static void __fastcall PromptHook(char* hero, void* edx) {
	g_origPrompt(hero, edx);
	// Individual heroes: a launcher (hero +0x3F0 bit 0x4000, set in the hero's own
	// tick: its "Activate") needs both heroes -- a fallen partner is revived next
	// to the hero at it (it may lie where the other can't reach it).
	if (*(uint32_t*)(hero + 0x3F0) & 0x4000) {
		Trio tt = GetTrio();
		int idx = !tt ? -1 : hero == tt.GetHero(HERO_ASTERIX).raw() ? HERO_ASTERIX : hero == tt.GetHero(HERO_OBELIX).raw() ? HERO_OBELIX : -1;
		IndivNoteLauncherSpot(idx);
		if (idx >= 0 && IndivFallenHero() == (idx ^ 1)) IndivReviveAtLauncher(idx);
	}
	CKYellowPages* yp = YellowPages();
	char* gm = yp ? (char*)yp->gameManager : nullptr;
	Trio t = GetTrio();
	if (!gm || !t || !g.p2Joined || g.suspended) return;
	int idx = hero == t.GetHero(g.p1Hero).raw() ? g.p1Hero : hero == t.GetHero(g.p2Hero).raw() ? g.p2Hero : -1;
	if (idx < 0) return;
	if (g_prompt.frame != g.frameNo) { g_prompt.frame = g.frameNo; g_prompt.kind = kPromptNone; g_prompt.leader = false; g_prompt.hero = -1; g_prompt.both = false; }
	uint32_t& cc = *(uint32_t*)(gm + 0xCC);
	unsigned kind = (cc >> 4) & 0xF;
	bool leads = idx == t.Leader();
	if (kind != kPromptNone) {
		if (g_prompt.kind == kind && g_prompt.hero != idx) g_prompt.both = true;   // offered to both heroes
		if (g_prompt.kind == kPromptNone || (leads && !g_prompt.leader)) { g_prompt.kind = kind; g_prompt.leader = leads; g_prompt.hero = idx; }
		else cc = (cc & ~0xF0u) | (g_prompt.kind << 4);
	} else if (g_prompt.kind != kPromptNone) {
		cc = (cc & ~0xF0u) | (g_prompt.kind << 4);
	}
}

int CoopPromptPlayer() {
	int r = !g.p2Joined || g_prompt.hero < 0 || g_prompt.both || g.frameNo - g_prompt.frame > 2 ? -1
	        : g_prompt.hero == g.p1Hero ? 0 : 1;
	static int last = -2, logs = 0;
	if (r != last && logs < 30) {
		logs++;
		Log("coop: the action prompt (kind %X) is for %s (hero %d%s, frame %u, now %u)", g_prompt.kind,
		    r < 0 ? "both players" : r ? "player 2" : "player 1", g_prompt.hero, g_prompt.both ? ", both offered" : "",
		    g_prompt.frame, g.frameNo);
	}
	last = r;
	return r;
}

// The Twister for both. A hero's attack state machine (table +0x130, current
// +0x135) takes its next state from FUN_00494DB0 (thiscall, the current state,
// ret 4). In the AI variant it copies the leader's Twister (states 9 / 10, from
// FUN_004963D0: 10 with Twister Fusion learned) -- in single player the
// companion always spins along, and the two can fuse. A player-driven partner
// never did. Now either player's Twister starts the other player's hero
// spinning too, when it is on the ground and not attacking.
typedef char(__fastcall* ChooseAttackFn)(char* hero, void* edx, int current);
const uintptr_t kChooseAttack = 0x494DB0;
const uint8_t   kChooseAttackPrologue[8] = { 0x53, 0x56, 0x8B, 0xF1, 0xF6, 0x46, 0x18, 0x01 };
ChooseAttackFn g_origChooseAttack = nullptr;

static bool Twisting(Hero h) {
	uint8_t s = *(uint8_t*)(h.raw() + 0x135);
	return s == 9 || s == 10;
}

// Only a Twister that just started, and not one that was itself copied: when
// the first one ends, the other must not start it again (and so on forever).
struct TwistTrack { bool on = false, copied = false; DWORD since = 0; } g_twist[HERO_COUNT];

static void TrackTwisters(Trio t) {
	const int both[2] = { HERO_ASTERIX, HERO_OBELIX };
	for (int i : both) {
		Hero h = t.GetHero(i);
		bool on = h && Twisting(h);
		if (on && !g_twist[i].on) g_twist[i].since = GetTickCount();
		if (!on) g_twist[i].copied = false;
		g_twist[i].on = on;
	}
}

static char __fastcall ChooseAttackHook(char* hero, void* edx, int current) {
	char next = g_origChooseAttack(hero, edx, current);
	// Individual heroes: each Twister is its own (its own combo bar).
	if (!g.p2Joined || g.suspended || next == 9 || next == 10 || IndivActive()) return next;
	Trio t = GetTrio();
	if (!t) return next;
	int me = hero == t.GetHero(g.p1Hero).raw() ? g.p1Hero : hero == t.GetHero(g.p2Hero).raw() ? g.p2Hero : -1;
	if (me < 0 || g.held[me]) return next;
	int mateIdx = me == g.p1Hero ? g.p2Hero : g.p1Hero;
	Hero mate = t.GetHero(mateIdx);
	TrackTwisters(t);
	if (g_twist[mateIdx].copied || GetTickCount() - g_twist[mateIdx].since > 400) return next;
	uint8_t cur = (uint8_t)current;
	bool free = cur == 0 || cur == 1 || cur == 4 || cur == 6 || cur == 0xFF;
	auto busyBody = [](Hero h) { uint8_t b = *(uint8_t*)(h.raw() + 0x38E) & 0x3F; return b == 0x25 || b == 0x26 || b == 0x27; };
	if (!mate || !Twisting(mate) || !free || Situation(Hero(t.GetHero(me))) != 0 || (uint8_t)next != cur ||
	    busyBody(mate) || busyBody(t.GetHero(me)))
		return next;
	typedef char(__fastcall* TwisterFn)(char* hero, void* edx);
	char twist = ((TwisterFn)0x4963D0)(hero, nullptr);
	g_twist[me].copied = true;
	Log("coop: %s spins along with %s's Twister (attack state %d)", HeroName(me),
	    HeroName(me == g.p1Hero ? g.p2Hero : g.p1Hero), twist);
	return twist;
}

// Twister Fusion. The Twister (attack state 10, handler 0x493F70) steps
// through body sub-states (hero +0x38E & 0x3F): 0x22 start, 0x23 spinning,
// 0x25 fused (then 0x27), 0x24/0x26 the plain end. From 0x23, FUN_004984F0
// (thiscall, no args, bool) fuses a hero whose fusion request (+0x188, set by
// X while spinning, but only with +0x189 "partner near") is up and whose
// companion spins (0x23) -- or, in the AI variant, the companion of a leader
// whose request is up. +0x189 is only ever set by the AI companion's spin
// (FUN_00498560, within trio +0x3D8 of the leader). Two player-driven heroes:
//  * spinning near each other, both get +0x189 (CoopFrame);
//  * the non-leader's request becomes the leader's -- the leader drives the
//    fused tornado, as in single player;
//  * co-op stands aside while a fusion is asked for or running, so the partner
//    is the AI companion that follows the leader's request (split screen off).
typedef char(__fastcall* FuseFn)(char* hero, void* edx);
const uintptr_t kFuseCheck = 0x4984F0;
const uint8_t   kFuseCheckPrologue[7] = { 0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x4E, 0x14 };
FuseFn g_origFuse = nullptr;
DWORD g_fusionAsked = 0;

static uint8_t SubState(Hero h) { return *(uint8_t*)(h.raw() + 0x38E) & 0x3F; }

static char __fastcall FuseHook(char* hero, void* edx) {
	Trio t = GetTrio();
	if (!g.p2Joined || !t) return g_origFuse(hero, edx);
	int me = hero == t.GetHero(g.p1Hero).raw() ? g.p1Hero : hero == t.GetHero(g.p2Hero).raw() ? g.p2Hero : -1;
	int leadIdx = g.sliced ? g.savedLeader : t.Leader();   // the real leader, also inside a sliced tick
	Hero lead = t.GetHero(leadIdx);
	if (me < 0 || !lead) return g_origFuse(hero, edx);
	if (me != leadIdx) {
		// The non-leader's own request becomes the leader's.
		if (*(uint8_t*)(hero + 0x188) && Twisting(lead)) {
			*(uint8_t*)(lead.raw() + 0x188) = 1;
			*(uint8_t*)(hero + 0x188) = 0;
			if (!g_fusionAsked || GetTickCount() - g_fusionAsked > 1500)
				Log("coop: %s asks for the Twister Fusion -- %s leads it", HeroName(me), HeroName(leadIdx));
			g_fusionAsked = GetTickCount();
			return 0;
		}
		// Follow the leader's request the way the AI companion does: the check's
		// AI branch, with the real leader and companion, this hero as AI.
		if (Twisting(lead) && *(uint8_t*)(lead.raw() + 0x188)) {
			uint8_t* tr = (uint8_t*)t.raw();
			uint8_t l = tr[0x2C], c = tr[0x2D], f = *(uint8_t*)(hero + 0x18);
			tr[0x2C] = (uint8_t)leadIdx;
			tr[0x2D] = (uint8_t)me;
			*(uint8_t*)(hero + 0x18) = f | 1;
			char r = g_origFuse(hero, edx);
			*(uint8_t*)(hero + 0x18) = f;
			tr[0x2C] = l;
			tr[0x2D] = c;
			if (r) Log("coop: %s joins %s's Twister Fusion", HeroName(me), HeroName(leadIdx));
			return r;
		}
		return g_origFuse(hero, edx);
	}
	if (*(uint8_t*)(hero + 0x188)) {
		if (!g_fusionAsked || GetTickCount() - g_fusionAsked > 1500) Log("coop: Twister Fusion asked for");
		g_fusionAsked = GetTickCount();
	}
	return g_origFuse(hero, edx);
}

// The fusion's steps: 0x25 (FUN_00498800: the companion closes in, and hides
// once the leader is at 0x26) and 0x26 (FUN_00498A30: the leader drives the
// one big tornado, the companion waits) -- each thiscall, no args, bool. The
// non-leading player's hero takes their AI branch (two tornados otherwise).
typedef char(__fastcall* FuseStepFn)(char* hero, void* edx);
const uintptr_t kFuseStep25 = 0x498800, kFuseStep26 = 0x498A30;
const uint8_t   kFuseStep25Prologue[7] = { 0x83, 0xEC, 0x14, 0x56, 0x8B, 0xF1, 0x57 };
const uint8_t   kFuseStep26Prologue[6] = { 0x83, 0xEC, 0x08, 0x56, 0x8B, 0xF1 };
FuseStepFn g_origFuseStep25 = nullptr, g_origFuseStep26 = nullptr;

static char FuseStepAsCompanion(FuseStepFn orig, char* hero, void* edx) {
	Trio t = GetTrio();
	if (!g.p2Joined || !t) return orig(hero, edx);
	int me = hero == t.GetHero(g.p1Hero).raw() ? g.p1Hero : hero == t.GetHero(g.p2Hero).raw() ? g.p2Hero : -1;
	int leadIdx = g.sliced ? g.savedLeader : t.Leader();
	Hero lead = t.GetHero(leadIdx);
	if (me < 0 || me == leadIdx || !lead || !Twisting(lead)) return orig(hero, edx);
	uint8_t ls = SubState(lead);
	bool fusing = ls == 0x25 || ls == 0x26 || (g_fusionAsked && GetTickCount() - g_fusionAsked < 1500);
	if (!fusing) return orig(hero, edx);
	uint8_t* tr = (uint8_t*)t.raw();
	uint8_t l = tr[0x2C], c = tr[0x2D], f = *(uint8_t*)(hero + 0x18);
	tr[0x2C] = (uint8_t)leadIdx;
	tr[0x2D] = (uint8_t)me;
	*(uint8_t*)(hero + 0x18) = f | 1;
	char r = orig(hero, edx);
	*(uint8_t*)(hero + 0x18) = (*(uint8_t*)(hero + 0x18) & ~1) | (f & 1);
	tr[0x2C] = l;
	tr[0x2D] = c;
	return r;
}
static char __fastcall FuseStep25Hook(char* hero, void* edx) { return FuseStepAsCompanion(g_origFuseStep25, hero, edx); }
static char __fastcall FuseStep26Hook(char* hero, void* edx) { return FuseStepAsCompanion(g_origFuseStep26, hero, edx); }

// Both player heroes spinning near each other: the "partner near" flag the AI
// companion's spin would set.
static void TwisterNearness(Trio t, Hero h1, Hero h2) {
	if (!Twisting(h1) || !Twisting(h2) || SubState(h1) != 0x23 || SubState(h2) != 0x23) return;
	float d = sqrtf((h1.Pos() - h2.Pos()).len2());
	float near2 = *(float*)(t.raw() + 0x3D8);
	if (!(d < near2)) return;
	if (!*(uint8_t*)(h1.raw() + 0x189) || !*(uint8_t*)(h2.raw() + 0x189))
		Log("coop: both Twisters %.1f apart (fusion range %.1f): Twister Fusion possible", d, near2);
	*(uint8_t*)(h1.raw() + 0x189) = 1;
	*(uint8_t*)(h2.raw() + 0x189) = 1;
}

static bool FusionOn(Hero h1, Hero h2) {
	if (g_fusionAsked && GetTickCount() - g_fusionAsked < 1500) return true;
	Hero both[2] = { h1, h2 };
	for (Hero h : both) {
		uint8_t s = SubState(h);
		if (Twisting(h) && (s == 0x25 || s == 0x26)) return true;
	}
	return false;
}

// For XXL Inspector's Cheats window (and anything else): 1 = give, 0 = throw away.
extern "C" __declspec(dllexport) void __cdecl XXLCoopTorchCheat(int give) { CoopTorchCheat(give != 0); }
// Every combo learned and the combo bar full (Inspector's Cheats window).
extern "C" __declspec(dllexport) void __cdecl XXLCoopCombosCheat() {
	Trio t = GetTrio();
	int lead = t ? t.Leader() : -1;
	int me = g.p2Joined ? g.p1Hero : lead;
	int prevStats = IndivEnter(me);
	if (CombosCheat(true) && me >= 0) { g.comboHitsHero = me; g.comboHitsAt = GetTickCount(); }
	IndivLeave(prevStats);
	Log("coop: cheat: every combo learned and the combo bar filled (Inspector)");
}

static void TorchBeforeTick(int heroIndex, Hero hero) {
	if (heroIndex != HERO_ASTERIX || !g_torch.want) return;
	Trio t = GetTrio();
	if (!t) return;
	if (*(char**)(hero.raw() + 0x464) == g_torch.torch) {
		// Lit, as from a brazier (0x5128: fire level 4 in torch +0x3E bits 2-4).
		typedef int(__fastcall* SendFn)(void*, void*, int, void*);
		char* c = g_torch.torch;
		if (((*(uint8_t*)(c + 0x3E) >> 2) & 7) == 0) ((SendFn)(*(void***)c)[5])(c, nullptr, 0x5128, nullptr);
		Log("coop: cheat: Asterix took the torch after %d frames (state %02X)", g_torch.frames, *(uint8_t*)(c + 0x3E));
		g_torch.want = 0;
		return;
	}
	if (++g_torch.frames > 60) {
		Log("coop: cheat: Asterix did not take the torch (situation %02X)", Situation(hero));
		g_torch.want = 0;
		return;
	}
	*(char**)((char*)t.raw() + 0x4C) = g_torch.torch;
}

void CoopBeforeLifeTick(int heroIndex, Hero hero) {
	TorchBeforeTick(heroIndex, hero);
	g.indivPrev = IndivEnter(heroIndex);   // this hero's health and combo bar in the game's place
	g.hpBeforeTick = (uint8_t)Health();
	if (!g.p2Joined || g.suspended) return;
	if (heroIndex != g.p1Hero && heroIndex != g.p2Hero) return;
	if (g.dying >= 0) {
		LeadTheDeath();
		if (heroIndex != g.dying) {
			if (HeroModePlayable(*F18(hero))) *F18(hero) = HeroAiVariant(*F18(hero));
			return;
		}
	}

	// The Life reads the control bits first thing in its tick, so this is the
	// moment they have to be right.
	Trio t = GetTrio();
	if (!t) return;
	if (g.held[heroIndex]) return;   // the game's AI drives it for now
	if (g.aiCycle[heroIndex] > 0) {
		// One pass through the AI variant resets the hero's player situation, as
		// leaving and rejoining co-op did (see StartAiCycle).
		g.aiCycle[heroIndex]--;
		if (HeroModePlayable(*F18(hero))) *F18(hero) = HeroAiVariant(*F18(hero));
		return;
	}
	EnforcePlayer(t, heroIndex);
	if (Situation(t.GetHero(g.p1Hero)) != kSituationGroupAction &&
	    Situation(t.GetHero(g.p2Hero)) != kSituationGroupAction)
		ClearStaleAiState(heroIndex, hero);

	// The magic potion's dash: while game manager +0x5C bit 19 is up, every
	// player-driven hero on the ground goes into situation 0x10 (dash), and only
	// the hero that drank gets the flag (+0x388 bit 0x4000) that ends it. In
	// single player the companion is an AI and never sees it. It is the drinker's
	// -- the hero the flask went to, else the leader -- and hidden from the other
	// player's hero. (Hidden from the hero not leading only, Obelix leading ran on
	// for good when Asterix took the flask: LV4-4's lever.)
	{
		CKYellowPages* yp = YellowPages();
		char* gm = yp ? (char*)yp->gameManager : nullptr;
		bool up = gm && (*(uint32_t*)(gm + 0x5C) & 0x80000);
		if (up && g.dashHero < 0) {
			int d = IndivPotionDrinker();
			g.dashHero = d == g.p1Hero || d == g.p2Hero ? d : t.Leader();
			Log("coop: the magic potion's dash is %s's", HeroName(g.dashHero));
		} else if (!up) {
			g.dashHero = -1;
		}
		g.hidDash = up && heroIndex != g.dashHero;
		if (g.hidDash) *(uint32_t*)(gm + 0x5C) &= ~0x80000u;
	}

	// What X takes (trio+0x4C) offered for the other player's hero -- an object
	// ticked as that hero's (proximity slicing: the torch by Asterix while Obelix
	// leads, LV2-1) -- is that hero's: hidden from the leader's tick. (It went to
	// the leader: Obelix took Asterix's torch from across the map.)
	if (heroIndex == t.Leader()) {
		uint32_t& offer = *(uint32_t*)(t.raw() + 0x4C);
		int other = heroIndex == g.p1Hero ? g.p2Hero : g.p1Hero;
		if (offer && offer == g.offerValue && g.offerHero == other) {
			g.leaderHidOffer = offer;
			offer = 0;
		}
	}

	// Player 1 steers relative to its own camera while player 2's hero leads.
	if (heroIndex == g.p1Hero && t.Leader() == g.p2Hero) P1CamBeginHeroTick();

	if (heroIndex == g.p2Hero && g.p2ValuesReady) {
		InputManager im = GetInputManager();
		if (im) {
			uint8_t** values = (uint8_t**)(im.raw() + 0x1C);
			g.savedValues = *values;
			*values = g.p2Values;
			// Some states read how long a binding has been held instead
			// (FUN_00435900: CKInput+0x18, 0x20 bytes per binding, seconds at
			// +0x18) -- Obelix holding the cable car's rope (situation 3) keeps hold
			// while X is. Player 2 gets a copy of the bindings carrying his own
			// values there.
			static char p2Bindings[256 * 0x20];
			char** bindings = (char**)(im.raw() + 0x18);
			unsigned nb = *(uint8_t*)(im.raw() + 0x3A);
			g.savedBindings = nullptr;
			if (*bindings && nb > 0 && nb <= 256) {
				memcpy(p2Bindings, *bindings, nb * 0x20);
				for (unsigned i = 0; i < nb; i++) *(float*)(p2Bindings + i * 0x20 + 0x18) = g.p2HoldTime[i];
				g.savedBindings = *bindings;
				*bindings = p2Bindings;
			}
			g.swapped = true;
			InterlockedIncrement(&g_hookCounters.p2Routed);
		}
	}

	// Player-side hero code assumes the hero running it is the trio's leader:
	// it asks the trio for "the companion" (trio+0x2D, or vtable +0x98) and means
	// the *other* hero. Run as the companion, it gets itself back -- the lifter
	// and launcher handler FUN_0049C030 then sends the hero to its own marker,
	// and the next launcher dereferences the empty slot and crashes
	// (axxl.exe+0x9C228). So for the length of the tick of whichever player's
	// hero is not leading (player 2's Obelix at the start of level 2, player 1's
	// Asterix after the belly slide), the trio says what that code expects:
	// this hero leads, the other one follows.
	if (heroIndex != t.Leader()) {
		uint8_t* trio = (uint8_t*)t.raw();
		int other = heroIndex == g.p1Hero ? g.p2Hero : g.p1Hero;
		g.savedLeader    = trio[0x2C];
		g.savedCompanion = trio[0x2D];
		g.slicedHero     = (uint8_t)heroIndex;
		g.slicedOther    = (uint8_t)other;
		trio[0x2C] = (uint8_t)heroIndex;
		trio[0x2D] = (uint8_t)other;
		g.sliced = true;
		g.sitBefore = Situation(hero);
		g.f18Before = *F18(hero);
		// Offers meant for the real leader stay with it: the magic potion
		// (latched at trio+0x4C for one frame) must not be taken by the other
		// hero while it poses as leader. One made for this hero stays (CoopNoteOffer).
		// The trio caches its nearby-enemy lists and distances once per frame,
		// from the leader (FUN_0047CB40, valid while +0x3F4 bit 0 is set):
		// lock-on and grabbing pick from them. Rebuild them for this hero, and
		// again after.
		*(uint32_t*)(trio + 0x3F4) &= ~1u;
		g.savedOffer = *(uint32_t*)(trio + 0x4C);
		if (g.savedOffer && g.savedOffer == g.offerValue && g.offerHero == heroIndex) g.savedOffer = 0;
		else *(uint32_t*)(trio + 0x4C) = 0;
	}

	// With split-screen, player 2 steers relative to player 2's own camera.
	if (heroIndex == g.p2Hero) P2CamBeginHeroTick();
}

// LB, the sidekick pass. In single player LB asks the AI companion to grab a
// Roman and spin it (AI order 5, body state 0x492660); spinning, it throws it to
// the leader when the leader presses LB again: FUN_00495F10 (let go) and Roman
// event 0x4C0F with no parameter -- the Roman's state 4 (vtable slot 44,
// 0x4D0D80) flies it to the trio leader's position (trio vf27), sending the trio
// 0x4C06 just before it arrives; the receiver can hit it in the air (x3) or with
// a Roman of its own (x10). With LB the companion that has nothing to grab says
// "no" (its sound bank +0x200). In co-op both heroes are players: a player
// spinning a Roman of their own (B on a stunned Roman: step +0x38E 0x0A grab,
// 0x0B lift, 0x0C spin, handler 0x492CF0; the Roman in state 3) passes it to the
// other player with LB, as the companion does -- let go, 0x4C0F with the other
// hero as the trio leader, the spin's throw step 0x0D; the Roman then ticks with
// that hero as the leader until it lands (ProximityPassTo), and the thrower
// whistles (bank +0x280, the companion's "ready" whistle). LB with no Roman
// spinning, or the other player too far or down: the other hero's "no".
const float kPassRange = 30.0f;

static void PlaySound(Hero h, uint32_t bankOffset) {
	CKYellowPages* yp = YellowPages();
	char* sm = yp ? *(char**)((char*)yp + 0x18) : nullptr;
	char* bank = h ? *(char**)(h.raw() + 0x374) : nullptr;
	if (!sm || !bank) return;
	char* entry = *(char**)(bank + 4) + bankOffset;
	typedef char(__fastcall* PlayingFn)(char* sm, void* edx, char* entry);
	typedef int(__fastcall* StartFn)(char* sm, void* edx, char* entry, float vol, float pitch, int a, int b, int c);
	if (((PlayingFn)0x43C130)(sm, nullptr, entry)) return;
	((StartFn)0x43B730)(sm, nullptr, entry, 1.0f, 1.0f, 0, 0, 0);
}

static void WatchLbPass(int idx, Hero h) {
	if (!g.p2Joined || g.suspended || (idx != g.p1Hero && idx != g.p2Hero) || !h) return;
	if (!(*(uint32_t*)(h.raw() + 0x3EC) & 0x80)) return;   // LB this frame (on the ground)
	Trio t = GetTrio();
	int other = idx == g.p1Hero ? g.p2Hero : g.p1Hero;
	Hero o = t ? t.GetHero(other) : Hero();
	if (!o) return;
	char* roman = *(char**)(h.raw() + 0xF8);
	uint8_t step = *(uint8_t*)(h.raw() + 0x38E) & 0x3F;
	bool spinning = roman && step == 0x0C && *(uint8_t*)(roman + 0x21) == 3;
	float apart = sqrtf((h.Pos() - o.Pos()).len2());
	bool down = IndivFallen(other);
	if (!spinning || down || apart > kPassRange || g.held[other] || !HeroModePlayable(o.Flags18())) {
		Log("coop: %s pressed LB: %s", HeroName(idx), !spinning ? "no Roman spinning" : down ? "the other player is down" :
		    apart > kPassRange ? "the other player is too far" : "the other player is busy");
		PlaySound(o, 0x200);
		return;
	}
	typedef void(__fastcall* ThisFn)(char* self, void* edx);
	typedef int(__fastcall* SendFn)(char* self, void* edx, int ev, void* param);
	((ThisFn)0x495F10)(h.raw(), nullptr);   // let go
	uint8_t* tr = (uint8_t*)t.raw();
	uint8_t lead = tr[0x2C], comp = tr[0x2D];
	tr[0x2C] = (uint8_t)other;
	tr[0x2D] = (uint8_t)idx;
	((SendFn)(*(void***)roman)[5])(roman, nullptr, 0x4C0F, nullptr);   // state 4: fly to the leader
	if (tr[0x2C] == (uint8_t)other) { tr[0x2C] = lead; tr[0x2D] = comp; }
	*(uint32_t*)(h.raw() + 0x38C) = (*(uint32_t*)(h.raw() + 0x38C) & 0xFFC0FFFF) | 0xD0000;   // the spin's throw step
	ProximityPassTo(roman, other);
	g.passDone = true;
	HintHide(HINT_PASS);
	// The whistle: the AI companion's lift of a grabbed Roman (its spin's step 5)
	// plays sound bank +0x280; the player's own spin never reaches that step.
	PlaySound(h, 0x280);
	if (getenv("XXL_PASSWATCH")) {   // diagnostics: who changes the passed Roman's state
		const void* w[1] = { roman + 0x21 };
		WatchBytes(w, 1, 0, 0);
	}
	Log("coop: %s passes the Roman %p to %s (%.0f units away), state now %d", HeroName(idx), roman, HeroName(other), apart,
	    *(uint8_t*)(roman + 0x21));
}

// Hint: while a player's hero spins a Roman (its grab-spin at step 0x0C, the
// Roman held in state 3) with the other player in passing range and not down, "LB passes
// it" -- the first three spins of a level, until a Roman has been passed once.
static void WatchSpinHint(int idx, Hero h) {
	if (!g.p2Joined || g.suspended || (idx != g.p1Hero && idx != g.p2Hero) || !h) return;
	char* roman = *(char**)(h.raw() + 0xF8);
	bool spin = roman && (*(uint8_t*)(h.raw() + 0x38E) & 0x3F) == 0x0C && *(uint8_t*)(roman + 0x21) == 3;
	bool started = spin && !g.spinning[idx];
	bool ended = !spin && g.spinning[idx];
	g.spinning[idx] = spin;
	if ((ended && !g.spinning[OtherHero(idx)]) || (spin && IndivFallen(OtherHero(idx)))) HintHide(HINT_PASS);
	if (!started || g.passDone || g.spinHints >= 3) return;
	Trio t = GetTrio();
	int other = idx == g.p1Hero ? g.p2Hero : g.p1Hero;
	Hero o = t ? t.GetHero(other) : Hero();
	if (!o || IndivFallen(other) || (h.Pos() - o.Pos()).len2() > kPassRange * kPassRange) return;
	g.spinHints++;
	int player = idx == g.p1Hero ? 0 : 1;
	wchar_t text[96];
	swprintf(text, 96, L"%s %s passes\nthe Roman to %s", HintPlayer(player), HintButtonName(player, HB_LB),
	         HintHeroName(other));
	HintShow(HINT_PASS, text, 8000);
	Log("coop: hint %d/3: %s can pass the spinning Roman with LB", g.spinHints, HeroName(idx));
}

void CoopAfterLifeTick(int heroIndex, Hero hero) {
	(void)heroIndex; (void)hero;
	P2CamEndHeroTick();
	if (g.leaderHidOffer) {
		Trio t = GetTrio();
		if (t && *(uint32_t*)(t.raw() + 0x4C) == 0) *(uint32_t*)(t.raw() + 0x4C) = g.leaderHidOffer;
		g.leaderHidOffer = 0;
	}
	if (g.hidDash) {
		g.hidDash = false;
		CKYellowPages* yp = YellowPages();
		char* gm = yp ? (char*)yp->gameManager : nullptr;
		if (gm) *(uint32_t*)(gm + 0x5C) |= 0x80000u;
	}
	if (g.sliced) {
		g.sliced = false;
		Trio t = GetTrio();
		if (t) {
			uint8_t* trio = (uint8_t*)t.raw();
			*(uint32_t*)(trio + 0x3F4) &= ~1u;
			if (g.savedOffer && *(uint32_t*)(trio + 0x4C) == 0) *(uint32_t*)(trio + 0x4C) = g.savedOffer;
			g.savedOffer = 0;
			Hero sh = t.GetHero(g.slicedHero);
			uint8_t sitNow = Situation(sh);
			bool startedGroup = sh && sitNow == kSituationGroupAction && g.sitBefore != kSituationGroupAction;
			// Talking to a Roman agent (X on the spy: situation 09, which sends the
			// spy 0x5139 and the trio 0x48BA, then runs the dialogue from this
			// hero's own player state). Left the companion, the cutscene made it
			// the AI companion, which ended that state, and the dialogue never went
			// on (level 3 sector 2).
			bool startedTalk = sh && sitNow == kSituationTalk && g.sitBefore != kSituationTalk;
			// Opening the merchant's shop (situation 0A). The lead stays: the
			// merchant's tick runs with this hero as the leader (proximity.cpp)
			// and the game's camera -- which the shop takes over -- is handed to
			// it for the visit (gamecam.cpp). (It took the lead for the visit
			// before: the shop's camera went to the leader's half otherwise.)
			if (sh && sitNow == kSituationShop && g.sitBefore != kSituationShop && trio[0x2C] == g.slicedHero) {
				g.shopHero = g.slicedHero;
				Log("coop: %s opened the merchant's shop while not leading -- %s keeps the lead; the merchant and the game's camera are %s's",
				    HeroName(g.slicedHero), HeroName(g.savedLeader), HeroName(g.slicedHero));
				GameCamHandTo(g.slicedHero, "the merchant");
			}
			// Getting on a machine (X at a machinegun: trio 0x4809, mode 2) -- the
			// trio's mode events act on the leader, and back as the companion the
			// hero was made the AI companion of that mode, unable to shoot (level 3
			// sector 6).
			uint8_t modeNow = sh ? HeroMode(*F18(sh)) : 0;
			bool startedMode = sh && HeroModePlayable(g.f18Before) && (modeNow == 1 || modeNow == 2);
			// (The machine gun, mode 2, Asterix alone: he stays the hero not leading -- GunSolo.)
			bool gunAlone = startedMode && modeNow == 2 && g.slicedHero == HERO_ASTERIX;
			if (gunAlone) {
				startedMode = false;
				Log("coop: Asterix took the machine gun while not leading -- %s keeps the lead", HeroName(g.savedLeader));
			}
			if (trio[0x2C] == g.slicedHero && trio[0x2D] == g.slicedOther && (startedGroup || startedTalk || startedMode)) {
				// It started a group action (a launcher, a lifter): that sequence
				// asks the trio for its leader all along, outside this tick too --
				// with the other hero still leading it kept putting that one on
				// the activator's spot. The hero that started it leads from now.
				Log("coop: %s started %s while not leading -- it leads until it is over", HeroName(g.slicedHero),
				    startedTalk ? "a Roman agent's dialogue" : startedMode ? "a machine (mode change)" : "a group action");
				g.groupTookLead = g.slicedHero;
				g.groupPrevLeader = g.savedLeader;
				// (A launcher -- trio 0x4804 from 0x49C699 in this tick; lifters send
				// none: only the leader uses one in single player, and it still leads
				// on landing. The lead the hero took for it stays.)
				g.groupLauncher = startedGroup && g.launcherAt && GetTickCount() - g.launcherAt < 200;
			} else if (trio[0x2C] == g.slicedHero && trio[0x2D] == g.slicedOther) {
				trio[0x2C] = g.savedLeader;
				trio[0x2D] = g.savedCompanion;
			} else {
				// A switch event ran during the tick and picked a new leader on
				// purpose; that is the game's decision now.
				Log("coop: the game changed the trio's leader during %s's tick (now %s)",
					HeroName(g.slicedHero), HeroName(trio[0x2C]));
			}
		}
	}
	if (g.swapped) {
		InputManager im = GetInputManager();
		if (im) *(uint8_t**)(im.raw() + 0x1C) = g.savedValues;
		if (im && g.savedBindings) {
			// What the game changed in the copy during the tick is the real table's:
			// a menu's FUN_00437F10 turning the gameplay bindings on (+4) -- the
			// merchant's shop closed by player 2's X on a launcher (0x4804 in Obelix's
			// tick) left player 1's controls off for good. Only the hold times
			// (+0x18) are player 2's own.
			char* copy = *(char**)(im.raw() + 0x18);
			unsigned nb = *(uint8_t*)(im.raw() + 0x3A);
			int changed = 0;
			for (unsigned i = 0; copy && copy != g.savedBindings && i < nb; i++)
				for (unsigned k = 0; k < 0x20; k++) {
					if (k >= 0x18 && k < 0x1C) continue;
					char& real = g.savedBindings[i * 0x20 + k];
					if (real != copy[i * 0x20 + k]) { real = copy[i * 0x20 + k]; changed++; }
				}
			static int s_told;
			if (changed && s_told++ < 8)
				Log("coop: the game changed the bindings during %s's tick (%d bytes) -- kept", HeroName(heroIndex), changed);
			*(char**)(im.raw() + 0x18) = g.savedBindings;
		}
		g.savedBindings = nullptr;
		g.swapped = false;
	}
	WatchLbPass(heroIndex, hero);
	WatchSpinHint(heroIndex, hero);
	if (g.hpBeforeTick > 0 && Health() == 0 && (heroIndex == g.p1Hero || heroIndex == g.p2Hero) &&
	    !IndivHeroDown(heroIndex, "in its own tick")) {
		NoteDeath(heroIndex, "in its own tick");
		LeadTheDeath();
	}
	IndivAfterFallenTick(heroIndex);
	IndivAfterHeroTick(heroIndex);
	// A lifter waits for this hero with the partner on it, or its car was just
	// sent back: no cable car (WatchLifterHint, WatchStrandedCar).
	static int s_keptOff = 0;
	if (heroIndex == KeptOffCars() && g.p2Joined && !g.suspended && IndivFallenHero() < 0 && IndivLeaveCableCar(heroIndex) &&
	    s_keptOff++ < 20)
		Log("coop: %s kept off a cable car (%s)", HeroName(heroIndex),
		    heroIndex == g.lifterWaitsFor ? "a lifter waits for it" : "its car was just sent back");
	IndivLeave(g.indivPrev);
	g.indivPrev = -1;
}

// Hits reach a hero through its sendEvent (CKHkAsterix / CKHkObelix vtable slot
// 5, shared 0x48BC70; damage FUN_00495FA0): the one whose event took the health
// to 0 is the one that died.
typedef int(__fastcall* HeroSendEventFn)(char* hero, void* edx, int event, void* param);
HeroSendEventFn g_origHeroSendEvent = nullptr, g_origHeroSendEvent2 = nullptr;   // the same function
int __fastcall HeroSendEventHook(char* hero, void* edx, int event, void* param) {
	Trio t = GetTrio();
	int idx = -1;
	for (int i = HERO_ASTERIX; t && i <= HERO_OBELIX; i++)
		if (t.GetHero(i).raw() == hero) idx = i;
	if (IndivDropEvent(idx, event)) return 1;   // no more hits for a fallen hero
	int prev = IndivEnter(idx);                 // its own health and combo bar
	int before = Health();
	// Two heroes touching: contact event 0x0300 (a list: +0 next, +4 the pair of
	// collision volumes, each one's owner at +0x20). Touched by a player's hero,
	// a hero is pushed aside and, running into it, hops out of the way
	// (FUN_0049CDF0, vertical speed 12) -- the AI sidekick dodging the player;
	// touched by a waiting sidekick (AI state 3), the player's hero is pushed
	// back -- blocked -- without the hop. Two players' heroes both took the
	// dodging sidekick's part: the "automatic jump". For the length of the
	// handler the other hero passes for a waiting sidekick (+0x18 bit 0, AI
	// state +0x561 = 3), so each is blocked by the other and neither hops.
	Hero bumped;
	uint8_t bumpedF18 = 0, bumpedAi = 0;
	if ((event & 0xFFFF) == 0x0300 && g.p2Joined && idx >= 0 && param && HeroIsPlayerVariant(*F18(t.GetHero(idx)))) {
		Hero other = t.GetHero(OtherHero(idx));
		int n = 0;
		bool touching = false;
		for (char** node = (char**)param; node && n < 32 && !touching; node = (char**)node[0], n++) {
			char** pair = node[1] && !IsBadReadPtr(node[1], 8) ? (char**)node[1] : nullptr;
			if (!pair) break;
			for (int k = 0; k < 2; k++)
				if (pair[k] && !IsBadReadPtr(pair[k], 0x24) && *(char**)(pair[k] + 0x20) == other.raw()) touching = true;
		}
		if (touching && other && HeroIsPlayerVariant(*F18(other))) {
			bumped = other;
			bumpedF18 = *F18(other);
			bumpedAi = *(uint8_t*)(other.raw() + 0x561);
			*F18(other) = (uint8_t)(bumpedF18 | 1);
			*(uint8_t*)(other.raw() + 0x561) = 3;
		}
	}
	// A hero spinning in the Twister (attack state +0x135 == 10) touching
	// Romans: the same handler pushes it back from them (push vector +0x22C..
	// +0x234), and in a crowd the Twister picks up too few in time (Obelix's
	// most of all, user, a modded level full of Romans): the tornado stopped on
	// them. A player's Twister goes through: that push is undone.
	bool twister = (event & 0xFFFF) == 0x0300 && g.p2Joined && idx >= 0 && *(uint8_t*)(hero + 0x135) == 10;
	float push[3];
	if (twister) memcpy(push, hero + 0x22C, sizeof(push));
	int r = g_origHeroSendEvent(hero, edx, event, param);
	if (twister) memcpy(hero + 0x22C, push, sizeof(push));
	if (bumped) {
		*F18(bumped) = bumpedF18;
		*(uint8_t*)(bumped.raw() + 0x561) = bumpedAi;
	}
	if (before > 0 && Health() == 0 && g.p2Joined && idx >= 0) {
		char how[32];
		sprintf(how, "hero event %04X", event & 0xFFFF);
		if (!IndivHeroDown(idx, how)) NoteDeath(idx, how);
	}
	IndivLeave(prev);
	return r;
}

// A machine is taken with both heroes at it: the other one within 12 units
// (4 up or down). Single player always has the companion at hand; in co-op the
// other player may be anywhere, and the game would carry (boat) or walk (gun)
// that hero over from wherever it is.
static bool MachineMatesNear(Trio t) {
	Hero a = t ? t.GetHero(HERO_ASTERIX) : Hero(), o = t ? t.GetHero(HERO_OBELIX) : Hero();
	if (!a || !o) return false;
	Vector3 d = a.Pos() - o.Pos();
	return d.x * d.x + d.z * d.z < 12.0f * 12.0f && fabsf(d.y) < 4.0f;
}

static void RefuseMachine(Trio t, int kind, int who) {
	PlaySound(t.GetHero(who), 0x200);
	wchar_t text[96];
	swprintf(text, 96, L"%s has to be\nat the %s too", OtherHero(who) == HERO_ASTERIX ? L"Asterix" : L"Obelix",
	         kind == 2 ? L"machine gun" : L"boat");
	HintShow(HINT_MACHINE, text, 4000);
	Log("coop: %s's X at the %s refused: %s is not there", HeroName(who), kind == 2 ? "machine gun" : "boat",
	    HeroName(OtherHero(who)));
}

// Obelix's X at a machine (NoteMachineUse): Asterix takes it from where Obelix
// stands -- the machines are his (trio 0x4809 / 0x4807 act on Asterix) -- and
// leads while it lasts, as when he presses X there himself; Obelix goes to his
// place as the game's companion.
static void ServeMachineRequest(Trio t) {
	if (!g.machineReq) return;
	char* machine = g.machineReq;
	int kind = g.machineKind;
	g.machineReq = nullptr;
	if (!t || g.suspended || GetTickCount() - g.machineReqAt > 500) return;
	Hero a = t.GetHero(HERO_ASTERIX), o = t.GetHero(HERO_OBELIX);
	if (!a || !o) return;
	uint8_t sit = Situation(a);   // standing, or swimming (0xFF) beside a boat
	if (!MachineMatesNear(t) || sit == kSituationGroupAction || sit == kSituationTalk || g.held[HERO_ASTERIX] ||
	    a.Busy() || !HeroModePlayable(a.Flags18())) {
		RefuseMachine(t, kind, HERO_OBELIX);
		return;
	}
	Vector3 dst = o.Pos();
	a.Pos() = dst;
	if (a.hook->node && a.hook->node->matrix) {
		float* hp = (float*)((char*)a.hook->node->matrix + 0x40);
		hp[0] = dst.x; hp[1] = dst.y; hp[2] = dst.z;
	}
	uint8_t* tr = (uint8_t*)t.raw();
	if (tr[0x2C] != HERO_ASTERIX) {
		g.groupTookLead = HERO_ASTERIX;
		g.groupPrevLeader = tr[0x2C];
		tr[0x2C] = HERO_ASTERIX;
		tr[0x2D] = HERO_OBELIX;
	}
	*(char**)(tr + 0x48) = machine;
	*(uint32_t*)(tr + 0x3F4) = (*(uint32_t*)(tr + 0x3F4) & ~0xC00u) | ((uint32_t)kind << 10) | 8u;
	int ev = kind == 2 ? 0x4809 : 0x4807;
	Log("coop: Obelix's X at the %s: Asterix takes it from Obelix's spot (%.1f %.1f %.1f), trio event %04X",
	    kind == 2 ? "machine gun" : "boat", dst.x, dst.y, dst.z, ev);
	typedef int(__fastcall* SendFn)(void* self, void* edx, int event, void* param);
	g.machineSending = true;
	((SendFn)(*(void***)t.raw())[5])(t.raw(), nullptr, ev, nullptr);
	g.machineSending = false;
	if (kind == 2) JoinGun(t, "took the gun with Asterix");   // Obelix is at the gun too
}

// Asterix on the machine gun (mode 2) by himself: Obelix is a player's hero,
// not in the gun's mode and not the game's.
static bool GunSolo(Trio t) {
	if (!g.p2Joined || !t || (g.p1Hero != HERO_OBELIX && g.p2Hero != HERO_OBELIX)) return false;
	Hero a = t.GetHero(HERO_ASTERIX), o = t.GetHero(HERO_OBELIX);
	return a && o && HeroMode(a.Flags18()) == 2 && HeroMode(o.Flags18()) != 2 && !g.held[HERO_OBELIX] && TrioGun(t);
}

// Obelix joins Asterix at the machine gun: the game's companion with the order
// 0x4809 gives one (AI order 7 to the handles, gun +0xD0); once there the AI
// sends trio 0x480A (Obelix in the gun's mode) -- one screen, Obelix's player
// turns the gun (shared.cpp). He is his player's again when Asterix leaves the
// gun (0x4806: orders back to 0).
static void JoinGun(Trio t, const char* why) {
	g.gunJoinReq = false;
	Hero o = t ? t.GetHero(HERO_OBELIX) : Hero();
	Vector3 pos, dir;
	if (!o || g.held[HERO_OBELIX] || !GunHandles(t, &pos, &dir)) return;
	// Asterix took the gun while Obelix led (he stays the hero not leading on it
	// alone): the walk to the handles is the companion's -- a leading hero is
	// handed back to its player at once. Asterix leads while both are at the gun
	// (one screen), and the lead goes back when they leave it (GiveBackLead).
	uint8_t* tr = (uint8_t*)t.raw();
	if (tr[0x2C] == HERO_OBELIX && g.groupTookLead < 0) {
		g.groupTookLead = HERO_ASTERIX;
		g.groupPrevLeader = HERO_OBELIX;
		tr[0x2C] = HERO_ASTERIX;
		tr[0x2D] = HERO_OBELIX;
		Log("coop: Obelix joins the gun while leading -- Asterix leads while both are at it");
	}
	Hold(t, HERO_OBELIX, why, true);
	uint8_t* ai = (uint8_t*)(o.raw() + 0x561);
	if (*ai == 0xFF) *ai = 0;
	o.SetOrder(AI_GOTO_BALLISTA);
	o.AiTargetPos() = pos;
	o.AiTargetDir() = dir;
	g.lastOrder[HERO_OBELIX] = AI_GOTO_BALLISTA;
	Log("coop: Obelix joins Asterix at the machine gun (%s): to the handles at (%.1f %.1f %.1f)", why, pos.x, pos.y,
	    pos.z);
}

// A hero that took the lead for something it started (a group action, a talk,
// the merchant, a machine) hands it back to the one who led before.
static void GiveBackLead(Trio t) {
	if (g.groupTookLead < 0 || !t) return;
	int took = g.groupTookLead, prev = g.groupPrevLeader;
	bool launcher = g.groupLauncher;
	g.groupTookLead = g.groupPrevLeader = -1;
	g.groupLauncher = false;
	if (launcher) {
		Log("coop: %s landed from the launcher it started -- it keeps the lead (the launcher's, as in single player)", HeroName(took));
		return;
	}
	// (Held with its order already back to "follow" -- Obelix at the gun's handles
	// once Asterix leaves it, 0x4806 -- it is let go this same frame: not held.)
	Hero ph = prev >= 0 ? t.GetHero(prev) : Hero();
	bool held = prev >= 0 && g.held[prev] && !(ph && ph.Order() == AI_FOLLOW_LEADER);
	if (t.Leader() == took && prev >= 0 && prev != took && !held) {
		typedef int(__fastcall* SendEventFn)(void* self, void* edx, int event, void* param);
		int ev = prev == HERO_ASTERIX ? 0x4800 : 0x4801;
		Log("coop: %s's group action is over; %s leads again (trio event %04X)", HeroName(took), HeroName(prev), ev);
		((SendEventFn)(*(void***)t.raw())[5])(t.raw(), nullptr, ev, nullptr);
	}
}

// Hint: at the start of a level, how player 2 joins (and leaves) -- for 10 s,
// once play can start (no cutscene; level 1: once Obelix is found).
static void ShowJoinHint(Trio t) {
	if (!g.joinHintPending || !t || !CoopGameplayLive()) return;
	const char* why = "";
	if (!CoopAllowedNow(t, &why)) return;
	g.joinHintPending = false;
	wchar_t text[96];
	if (g_config.keyboard && Player1Pad() != kKeyboardPad)
		swprintf(text, 96, L"%s press %s to join co-op\n(keyboard: %s), again to leave", HintPlayer(1),
		         HintButtonName(1, HB_START), HintKeyName(g_config.keys[CoopConfig::KEY_START]));
	else
		swprintf(text, 96, L"%s press %s to join co-op\nand again to leave", HintPlayer(1), HintButtonName(1, HB_START));
	HintShow(HINT_JOIN, text, 10000);
}

// Hint: a lifter waiting at the bottom for the trio (CKHkLifter, 2/47: state
// (+8 >> 1) & 3 == 0 with flag +8 bit 0) with one player's hero on its platform
// (+0x50: the object under a hero's feet, hero +0x280, on the ground) -- in co-op
// it waits for both (proximity.cpp). Shown after 4 s of waiting, not at once.
// The hero it waits for locked by the level (LV2-1: Asterix on the spinning
// platform's circle waits for Obelix at the turnstile, Obelix went back to the
// lifter) can't come: after 2 s its sequence is undone as for a fallen partner
// (the circle's light pillar back, the hero freed and on its last solid ground).
// Left alone, the lifter's own wait (+0x34, ~8 s) dropped him onto it still locked.
// While it waits, that hero can't board a cable car either (as with the partner
// down): its situation 2 is left and its boarding events held back.
static void WatchLifterHint(Trio t) {
	static char* waiting = nullptr;
	static DWORD since = 0;
	g.lifterWaitsFor = -1;
	if (!g.p2Joined || g.suspended || !t) { waiting = nullptr; return; }
	Hero h[2] = { t.GetHero(HERO_ASTERIX), t.GetHero(HERO_OBELIX) };
	for (unsigned i = 0, n = GetNumLvlClassInstances(2, 47); i < n && i < 32; i++) {
		char* l = (char*)GetLvlObject(2, 47, i);
		if (!l || ((*(uint32_t*)(l + 8) >> 1) & 3) != 0 || !(*(uint32_t*)(l + 8) & 1)) continue;
		void* platform = *(void**)(l + 0x50);
		bool on[2];
		for (int k = 0; k < 2; k++)
			on[k] = h[k] && platform && (*(uint32_t*)(h[k].raw() + 0x384) & 7) == 0 &&
			        *(void**)(h[k].raw() + 0x280) == platform;
		if (on[0] == on[1]) continue;
		if (waiting != l) { waiting = l; since = GetTickCount(); }
		int missing = on[0] ? HERO_OBELIX : HERO_ASTERIX;
		g.lifterWaitsFor = missing;
		if (GetTickCount() - since >= 2000 && LockedByLevel(missing, h[missing]) && GetTickCount() - g_lock.since >= 2000) {
			Log("coop: the lifter waits for %s, locked by the level -- its sequence undone", HeroName(missing));
			UndoLock(t, missing, false);
			since = GetTickCount();
			return;
		}
		// (Freed already with its spinning platform's turn not done -- Obelix crossed
		// the turnstile's zone 03C8 on his way here: WatchUnfinishedSequence.)
		if (GetTickCount() - since < 4000) return;
		wchar_t text[64];
		swprintf(text, 64, L"The lifter waits for %s\nto go up", HintHeroName(on[0] ? HERO_OBELIX : HERO_ASTERIX));
		HintShow(HINT_LIFTER, text, 300);
		return;
	}
	waiting = nullptr;
}

// ---------------------------------------------------------------------------
// Trio events
// ---------------------------------------------------------------------------

namespace {
bool IsModeOrSwitchEvent(int ev) {
	if (((ev >> 8) & 0xFF) != 0x48) return false;
	int lo = ev & 0xFF;
	return lo == 0x00 || lo == 0x01 || lo == 0xB7 || lo == 0xB8 ||   // switch leader
	       lo == 0x06 || lo == 0x07 || (lo >= 0x09 && lo <= 0x0F);   // mode changes
}
}

// Events that give "the companion" something to do: the 0x48xx trio orders,
// the 0x51xx requests and 0x0Cxx interactions. Several only act on a companion
// whose AI is idle (+0x561 == 0, e.g. 0x48EF sends it somewhere) or that is not
// player-driven (the switch handler refuses a target whose companion bit is
// clear), so around them the non-leader hero is shown as an idle AI follower.
bool IsCompanionEvent(int ev) {
	int cls = (ev >> 8) & 0xFF;
	return cls == 0x48 || cls == 0x51 || cls == 0x0C;
}

struct EventFrame {
	bool    used[HERO_COUNT];
	uint8_t order[HERO_COUNT];
	bool    idled[HERO_COUNT];   // +0x561 was 0xFF and we set 0
};
EventFrame g_eventStack[8];
int g_eventDepth = 0;

int CoopBeforeTrioEvent(int ev, void* caller) {
	InterlockedIncrement(&g_hookCounters.trioEvents);
	if (!g.p2Joined) {
		static const bool logTrio = getenv("XXL_LOGTRIO") != nullptr;   // diagnostics: single player too
		static int last = -1;
		static const bool all = logTrio && getenv("XXL_LOGTRIO")[0] == '2';
		if (logTrio && (ev & 0xFFFF) != last && (all || ((ev >> 8) & 0xFF) != 0x51)) {
			last = ev & 0xFFFF;
			Log("coop: (single player) trio event %04X from %p", ev & 0xFFFF, caller);
		}
		return 0;
	}
	ev &= 0xFFFF;
	static int lastEv = -1;
	static DWORD lastEvAt = 0;
	DWORD now = GetTickCount();
	if (ev != lastEv || now - lastEvAt > 1000) Log("coop: trio event %04X from %p", ev, caller);
	lastEv = ev;
	lastEvAt = now;
	if (!IsModeOrSwitchEvent(ev) && !IsCompanionEvent(ev)) return 0;
	if (g_eventDepth >= 8) return 0;
	// The machine gun (0x4809) sends an idle AI companion to its handles (AI
	// order 7). Obelix's player is left alone: Asterix uses the gun by himself
	// until Obelix's player joins him there (JoinGun).
	if (ev == 0x4809 && !g.gunJoining) return 0;
	// Likewise the merchant (X at the shop, 0x49BD4B: trio 0x48EF sends the
	// companion there too, AI order 14): the other player plays on meanwhile.
	if (ev == 0x48EF && caller == (void*)0x49BD4B) return 0;

	Trio t = GetTrio();
	if (!t) return 0;
	EventFrame& fr = g_eventStack[g_eventDepth++];
	for (int i = 0; i < HERO_COUNT; i++) {
		fr.used[i] = fr.idled[i] = false;
		if (i == HERO_IDEFIX || (i != g.p1Hero && i != g.p2Hero)) continue;
		Hero h = t.GetHero(i);
		if (!h || i == t.Leader() || g.held[i]) continue;
		uint8_t f = *F18(h);
		// A hero in its reset pass (StartAiCycle) is still a player's: the boss
		// sends "Obelix leads" and then "Asterix waits" one after the other, and
		// the wait must still hand Asterix to the game.
		bool cycling = g.aiCycle[i] > 0;
		if (!HeroModePlayable(f) || (!HeroIsPlayerVariant(f) && !cycling)) continue;
		fr.used[i] = true;
		fr.order[i] = (uint8_t)h.Order();
		*F18(h) = HeroAiVariant(f);
		uint8_t* ai = (uint8_t*)(h.raw() + 0x561);
		if (*ai == 0xFF) { *ai = 0; fr.idled[i] = true; }
	}
	return g_eventDepth;
}

static void SetHeroPosition(Hero h, const Vector3& to) {
	h.Pos() = to;
	if (h.hook->node && h.hook->node->matrix) {
		float* hp = (float*)((char*)h.hook->node->matrix + 0x40);
		hp[0] = to.x; hp[1] = to.y; hp[2] = to.z;
	}
}

// Two ticks in the AI variant: the Life's state change runs the player
// situation's exit, the AI picks up, and the player variant starts afresh.
void StartAiCycle(int idx, const char* why) {
	Trio t = GetTrio();
	Hero h = t ? t.GetHero(idx) : Hero();
	if (!h || idx < 0 || idx >= HERO_COUNT) return;
	g.aiCycle[idx] = 2;
	if (HeroModePlayable(*F18(h))) *F18(h) = HeroAiVariant(*F18(h));
	Log("coop: %s reset through the AI (%s, situation %02X)", HeroName(idx), why, Situation(h));
}

void CoopAfterTrioEvent(int ev, int cookie) {
	if (!cookie) return;
	if (cookie != g_eventDepth) { g_eventDepth = cookie - 1 < 0 ? 0 : cookie - 1; return; }
	EventFrame& fr = g_eventStack[--g_eventDepth];
	if (!g.p2Joined) return;
	Trio t = GetTrio();
	if (!t) return;
	ev &= 0xFFFF;
	if (IsModeOrSwitchEvent(ev)) Log("coop: after trio event %04X leader=%s", ev, HeroName(t.Leader()));
	for (int i = 0; i < HERO_COUNT; i++) {
		if (!fr.used[i]) continue;
		Hero h = t.GetHero(i);
		if (!h) continue;
		uint8_t* ai = (uint8_t*)(h.raw() + 0x561);
		if (fr.idled[i] && *ai == 0) *ai = 0xFF;   // the AI driver enters its state from scratch
		int order = (int)h.Order();
		// "The companion waits here" (0x4802) is the game's even when the order
		// value was already 3 from an earlier wait: the boss puts Asterix on its
		// button (situation 0x0F), hands the lead to Obelix and tells Asterix to
		// wait -- left to player 1 he walked off the button and was never launched.
		bool waitEvent = ev == 0x4802 && order == 3;
		if (i != t.Leader() && (order != fr.order[i] || waitEvent) && IsScriptedOrder(order)) {
			char why[32];
			sprintf(why, "event %04X", ev);
			Hold(t, i, why);
		}
	}
	// Not inside another event either: a switch sent by a mode event (0x480B,
	// belly slide, sends 0x4801) is followed by the mode change itself.
	// A switch that takes the lead from a player's hero in the middle of a
	// situation of its own: in single player that hero becomes the AI companion,
	// which ends the situation; kept as a player hero it stays in it. The boss
	// of level 2 puts Obelix in situation 0x0F after his hit, then gives the lead
	// to Asterix -- and the next blue circle handed player 2 an Obelix that could
	// not move.
	int lo = ev & 0xFF;
	if (((ev >> 8) & 0xFF) == 0x48 && (lo == 0x00 || lo == 0x01 || lo == 0xB7 || lo == 0xB8)) {
		const int both[2] = { g.p1Hero, g.p2Hero };
		for (int idx : both) {
			Hero h = t.GetHero(idx);
			uint8_t sit = h ? Situation(h) : 0xFF;
			if (h && idx != t.Leader() && !g.held[idx] && sit != 0x00 && sit != 0xFF && !IndivFallen(idx))
				StartAiCycle(idx, "lost the lead");
		}
	}
	if (!g.suspended && g_eventDepth == 0) {
		EnforcePlayer(t, g.p1Hero);
		EnforcePlayer(t, g.p2Hero);
	}
}

// ---------------------------------------------------------------------------

void CoopInit() {
	InstallDetour(kTakeHandler, (void*)TakeHook, (void**)&g_origTake, kTakePrologue, sizeof(kTakePrologue),
	              "hero take press (fire cheat)");
	InstallDetour(kReadCommandBlock, (void*)ReadCmdHook, (void**)&g_origReadCmd, kReadCmdPrologue,
	              sizeof(kReadCmdPrologue), "hero potion flag (Obelix's potion)");
	InstallDetour(kHeroPrompt, (void*)PromptHook, (void**)&g_origPrompt, kHeroPromptPrologue,
	              sizeof(kHeroPromptPrologue), "HUD action prompt (either player's)");
	InstallDetour(kFuseStep25, (void*)FuseStep25Hook, (void**)&g_origFuseStep25, kFuseStep25Prologue,
	              sizeof(kFuseStep25Prologue), "Twister Fusion step 0x25 (partner as companion)");
	InstallDetour(kFuseStep26, (void*)FuseStep26Hook, (void**)&g_origFuseStep26, kFuseStep26Prologue,
	              sizeof(kFuseStep26Prologue), "Twister Fusion step 0x26 (partner as companion)");
	InstallDetour(kFuseCheck, (void*)FuseHook, (void**)&g_origFuse, kFuseCheckPrologue, sizeof(kFuseCheckPrologue),
	              "Twister Fusion check (both players)");
	InstallDetour(kChooseAttack, (void*)ChooseAttackHook, (void**)&g_origChooseAttack, kChooseAttackPrologue,
	              sizeof(kChooseAttackPrologue), "hero attack chooser (Twister for both players)");
	InstallDetour(kFreezeWorld, (void*)FreezeWorldHook, (void**)&g_origFreeze, kFreezePrologue,
	              sizeof(kFreezePrologue), "world pause (not for the merchant in co-op)");
	InstallDetour(kSkipScene, (void*)SkipSceneHook, (void**)&g_origSkipScene, kSkipScenePrologue,
	              sizeof(kSkipScenePrologue), "cutscene skip (its later events still flagged)");
	PatchCallSite(kShopMenuInputOff, kShopMenuInputOffBytes, sizeof(kShopMenuInputOffBytes), (void*)ShopMenuInputOff,
	              "merchant's menu input (player 1's controls on while player 2 shops)");
	InstallDetour(kSetPreset, (void*)SetPresetHook, (void**)&g_origSetPreset, kSetPresetPrologue,
	              sizeof(kSetPresetPrologue), "controller preset (picked without a pad)");
	PatchVtableSlot(0x625A8C, 5, 0x48BC70, (void*)HeroSendEventHook, (void**)&g_origHeroSendEvent,
	                "CKHkAsterix::sendEvent (who died)");
	PatchVtableSlot(0x625BB0, 5, 0x48BC70, (void*)HeroSendEventHook, (void**)&g_origHeroSendEvent2, "CKHkObelix::sendEvent (who died)");
	PadsInit();
	if (g_config.p1Pad >= 0) PadIdentForce(g_config.p1Pad, PAD_ROLE_GAME);
	Log("coop: init (player 1 pad: %s)", g_config.p1Pad >= 0 ? "fixed by ini" : "auto-detect");
}

void CoopShutdown() {
	PadsShutdown();
}
