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
#include "crash.h"
#include "proximity.h"
#include "detour.h"
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

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

void StartAiCycle(int idx, const char* why);   // below: two ticks through the AI variant

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
	int     joinLeader = -1;        // the trio's leader when player 2 joined
	int     storyZoneHero = -1;     // a cutscene zone bound to the other hero set off by this one
	DWORD   storyZoneAt = 0;
	int     bossRunHero = -1;       // woke a boss the heroes run from; the other player is brought along

	uint8_t p2Values[kP2ValuesSize] = {};
	bool    p2ValuesReady = false;
	uint8_t p2CamValues[kP2ValuesSize] = {};   // p2Values plus the right stick, for the game camera's ticks
	int     p2Combos = 0;             // combo bytes after the actions (P2RecognizeCombos)

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
	int      aiCycle[HERO_COUNT] = {};   // ticks left in the AI variant (situation reset)
	DWORD    stuckSince[HERO_COUNT] = {};
	DWORD    outsideSince[HERO_COUNT] = {};   // WatchSectors: outside the streamed sector since
	Vector3  stuckPos[HERO_COUNT];
	uint8_t  savedLeader = 0, savedCompanion = 0;
	uint8_t  slicedHero = 0, slicedOther = 0;   // what the trio said during the sliced tick
	int      groupTookLead = -1;      // a hero that took the lead to start a group action
	int      groupPrevLeader = -1;    // ... and the leader before it, who gets it back after
	uint32_t savedOffer = 0;          // trio+0x4C hidden from player 2's tick

	bool    suspended = false;
	bool    shopping = false;            // a merchant's menu is open
	bool    oneScreen = false;           // Twister Fusion: co-op on, split screen off
	bool    held[HERO_COUNT] = {};       // a player's hero the game has borrowed (scripted AI order)
	uint8_t lastOrder[HERO_COUNT] = {};
	DWORD   heldSince[HERO_COUNT] = {};
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
} g;

const int kJoinSettleFrames = 5;
const int kCutsceneHoldFrames = 15;   // ~0.25 s

// Hero situation-machine state (hero+0x3E9) of group actions: FUN_0049C030, which
// sends the companion and Idefix to markers and waits for them (lifters).
const uint8_t kSituationGroupAction = 0x0D;
// The walk to a Roman agent and the talk with him (X on the spy, or trio 0x5120).
const uint8_t kSituationTalk = 0x09;
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

// Puts hero `idx` into the player variant of its current mode if that mode is a
// playable one (normal or swimming). Scripted modes are left to the game.
bool EnforcePlayer(Trio t, int idx) {
	Hero h = t.GetHero(idx);
	if (!h || (idx >= 0 && idx < HERO_COUNT && (g.held[idx] || g.aiCycle[idx] > 0))) return false;
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

void Hold(Trio t, int idx, const char* why) {
	Hero h = t.GetHero(idx);
	if (!h || g.held[idx]) return;
	g.held[idx] = true;
	g.aiCycle[idx] = 0;   // the game's now; no reset pass afterwards
	g.heldSince[idx] = GetTickCount();
	uint8_t f = *F18(h);
	if (HeroModePlayable(f)) *F18(h) = HeroAiVariant(f);
	Log("coop: %s handed to the game (%s, AI order %d)", HeroName(idx), why, (int)h.Order());
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
	for (int i = 0; i < HERO_COUNT; i++) g.held[i] = false;
}

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
		Hero h1 = t.GetHero(p1), h2 = t.GetHero(p2);
		// Back to player 1's hero only if it led when player 2 came in: joined
		// while the game led with Obelix (after the belly slides), it stays so.
		if (t.Leader() == p2 && g.joinLeader == p1 && h1 && h2) {
			// The switch handler only accepts a target that looks like a companion.
			*F18(h1) = HeroAiVariant(*F18(h1));
			t.SwitchTo(p1);
		}
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
		g.joinLeader = t.Leader();
		// Joining in the middle of a scripted sequence (Asterix already seated in
		// the cable car, Obelix leading to pull it): a hero the game has borrowed
		// stays the game's until the sequence is over.
		const int both[2] = { g.p1Hero, g.p2Hero };
		for (int idx : both) {
			Hero h = t.GetHero(idx);
			int order = h ? (int)h.Order() : 0;
			if (h && idx != t.Leader() && !h.IsPlayerControlled() && IsScriptedOrder(order)) {
				g.held[idx] = true;
				g.heldSince[idx] = GetTickCount();
				g.lastOrder[idx] = (uint8_t)order;
				Log("coop: joining mid-sequence: %s is the game's (AI order %d) until the sequence ends",
					HeroName(idx), order);
			}
		}
		g.p2BackHeldAt = 0;
		g.suspended = false;
		Log("coop: player 2 joined on %s as %s (player 1 keeps %s)", PadName(pad),
			HeroName(g.p2Hero), HeroName(g.p1Hero));
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
// picks it in CKAsterixGameManager::vf10 (with a pad the saved options -- preset 3
// -- else the keyboard's, 1); before that it is 0, a layout with X/B/Y shuffled
// and the menus on the left stick. Twice a session was played on preset 0 until
// a return to the menu replayed the intro and set it -- both times after
// force_level jumped from the title into level 2 and XXL Inspector jumped on to
// level 5 seconds later (what skipped the game's choice is not certain). 0 is never
// a preset the game picks for play: a level running on it gets the game's choice.
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
	if (g.lastLevel <= 0 || preset != 0 || now - lastCheck < 1000) return;
	lastCheck = now;
	typedef char(__fastcall* HasPadFn)(char* in, void* edx);
	typedef void(__fastcall* SetPresetFn)(char* in, void* edx, int preset);
	int want = ((HasPadFn)0x438AB0)(in, nullptr) ? 3 : 1;
	((SetPresetFn)0x438950)(in, nullptr, want);
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

static bool HeroFlying(Trio t) {
	for (int i = 0; i < 2; i++) {
		Hero h = t.GetHero(i);
		if (h && Situation(h) == kSituationCatapultFlight) return true;
	}
	return false;
}

bool CoopDeferTrioEvent(void* trio, int ev) {
	Trio t = GetTrio();
	if (t && t.raw() == (char*)trio && ProximityBossRunDropsWalk(ev & 0xFFFF)) {
		Log("coop: trio event %04X (the sidekick runs ahead from the boss) dropped: each player runs their own hero", ev & 0xFFFF);
		return true;
	}
	if (!t || t.raw() != (char*)trio || !IsLeaderSwitch(ev) || !HeroFlying(t)) return false;
	g_deferredSwitch = ev & 0xFFFF;
	g_deferredSince = GetTickCount();
	Log("coop: trio event %04X while flying from a catapult -- after the landing", g_deferredSwitch);
	return true;
}

static void SendDeferredSwitch() {
	if (g_deferredSwitch < 0) return;
	Trio t = GetTrio();
	if (!t) { g_deferredSwitch = -1; return; }
	if (HeroFlying(t) && GetTickCount() - g_deferredSince < 20000) return;
	int ev = g_deferredSwitch;
	g_deferredSwitch = -1;
	Log("coop: landed -- trio event %04X sent now", ev);
	typedef int(__fastcall* SendFn)(void* self, void* edx, int event, void* param);
	((SendFn)(*(void***)t.raw())[5])(t.raw(), nullptr, ev, nullptr);
}

void CoopNoteTrioEvent(void* trio, int event, void* param) {
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
	if (p.pressed & (PAD_X | PAD_B)) press |= 1u << 5;
	wasLeft = left;
	wasRight = right;
	char* input = im.raw();
	uint8_t* v = im.Values();
	if (!press || !input || !v) return;
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
		g.groupTookLead = g.groupPrevLeader = -1;
		g_spyWalk.on = false;
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
	if (!inCutscene && (p.pressed & PAD_START)) { Release("pressed Start"); return; }
	const uint16_t kFace = PAD_A | PAD_B | PAD_X | PAD_Y;
	if (!inCutscene && (p.buttons & PAD_BACK) && (p.pressed & kFace)) g.p2BackCombo = true;
	if (!(p.buttons & PAD_BACK)) g.p2BackCombo = false;
	if (!inCutscene && (p.buttons & PAD_BACK) && !g.p2BackCombo) {
		if (g.p2BackHeldAt == 0) g.p2BackHeldAt = now;
		else if ((int)(now - g.p2BackHeldAt) >= g_config.dropHoldMs) { Release("held Back"); return; }
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
	//      played out of order never ends)
	//   Y  [cheats] fill the magic potion
	//   X  [cheats] your hero becomes the leader
	//   D-pad up / down  [cheats] Asterix gets the nearest torch, lit / throws it away
	//   D-pad left  [cheats] every combo learned, combo bar full
	// Player 1's controller is the one the game reads (pad identification).
	{
		int p1pad = Player1Pad();
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
			if ((pressed & PAD_B) && g.held[me]) {
				g.held[me] = false;
				Log("coop: %s took %s back from the game (AI order %d left behind)", player, HeroName(me),
				    (int)hm.Order());
				EnforcePlayer(t, me);
			} else if ((pressed & PAD_B) && !g.suspended) {
				StartAiCycle(me, "R3 + B");
			}
			if ((pressed & PAD_Y) && g_config.cheats) {
				SetPotionGauge(300.0f);
				Log("coop: cheat: magic potion filled by %s", player);
			}
			if ((pressed & PAD_DPAD_UP) && g_config.cheats) CoopTorchCheat(true);
			if ((pressed & PAD_DPAD_DOWN) && g_config.cheats) CoopTorchCheat(false);
			if ((pressed & PAD_DPAD_LEFT) && g_config.cheats) {
				CombosCheat();
				Log("coop: cheat: every combo learned and the combo bar filled by %s", player);
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
	g.shopping = shopping;
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
	bool scripted = !HeroModePlayable(h1.Flags18()) || !HeroModePlayable(h2.Flags18()) ||
	                h1.Busy() || h2.Busy() || groupAction || cinematic || shopping;
	if (scripted != g.suspended) {
		g.suspended = scripted;
		Log("coop: %s (%s %s / %s %s%s%s%s)", scripted ? "suspended for a scripted mode" : "resumed",
			HeroName(g.p1Hero), ModeName(HeroMode(h1.Flags18())),
			HeroName(g.p2Hero), ModeName(HeroMode(h2.Flags18())),
			cinematic ? ", cutscene" : "", groupAction ? ", group action" : "", shopping ? ", merchant" : "");
		CoopLogState(scripted ? "suspend" : "resume");
		// A group action the non-leading player started is over: the lead goes
		// back where it was (single player never hands it over for a launcher).
		if (!scripted && g.groupTookLead >= 0) {
			int took = g.groupTookLead, prev = g.groupPrevLeader;
			g.groupTookLead = g.groupPrevLeader = -1;
			if (t.Leader() == took && prev >= 0 && prev != took && !g.held[prev]) {
				typedef int(__fastcall* SendEventFn)(void* self, void* edx, int event, void* param);
				int ev = prev == HERO_ASTERIX ? 0x4800 : 0x4801;
				Log("coop: %s's group action is over; %s leads again (trio event %04X)", HeroName(took), HeroName(prev), ev);
				((SendEventFn)(*(void***)t.raw())[5])(t.raw(), nullptr, ev, nullptr);
			}
		}
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
	WatchSpyWalk(t, now);
	// While suspended, the hero that is not leading is the game's companion, in
	// whatever mode the game put it -- whichever player it belongs to. Rides only
	// work that way: on the belly slide (mode 3) Obelix leads and Asterix rides
	// along as the AI variant, whose handler carries him with Obelix; left in the
	// player variant he stayed at the top of the slide.
	if (g.suspended) {
		const int both[2] = { g.p1Hero, g.p2Hero };
		for (int idx : both) {
			Hero h = idx == g.p1Hero ? h1 : h2;
			uint8_t f = *F18(h);
			if (idx != t.Leader() && HeroIsPlayerVariant(f)) {
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
	// player when the order is over or the hero leads again.
	const int players[2] = { g.p1Hero, g.p2Hero };
	for (int idx : players) {
		Hero h = t.GetHero(idx);
		if (!h) continue;
		int order = (int)h.Order();
		bool leads = t.Leader() == idx;
		if (!g.held[idx] && !leads && order != g.lastOrder[idx] && IsScriptedOrder(order)) {
			Hold(t, idx, "new order");
		} else if (g.held[idx] && (leads || order == AI_FOLLOW_LEADER)) {
			g.held[idx] = false;
			Log("coop: %s back to its player (%s)", HeroName(idx), leads ? "leads again" : "order done");
		} else if (g.held[idx] && order == g.lastOrder[idx] && GetTickCount() - g.heldSince[idx] > 120000) {
			// Safety valve: an order that never ends must not take a player's hero for good.
			g.held[idx] = false;
			Log("coop: %s back to its player (order %d still running after 2 min)", HeroName(idx), order);
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

static void __fastcall TakeHook(char* hero, void* edx, int arg) {
	if (g_torch.want) {
		Trio t = GetTrio();
		Hero a = t ? t.GetHero(HERO_ASTERIX) : Hero();
		if (a && hero == a.raw()) *(uint32_t*)(hero + 0x3EC) |= 0x10;
	}
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
struct { uint32_t frame = 0; unsigned kind = kPromptNone; bool leader = false; } g_prompt;

static void __fastcall PromptHook(char* hero, void* edx) {
	g_origPrompt(hero, edx);
	CKYellowPages* yp = YellowPages();
	char* gm = yp ? (char*)yp->gameManager : nullptr;
	Trio t = GetTrio();
	if (!gm || !t || !g.p2Joined || g.suspended) return;
	int idx = hero == t.GetHero(g.p1Hero).raw() ? g.p1Hero : hero == t.GetHero(g.p2Hero).raw() ? g.p2Hero : -1;
	if (idx < 0) return;
	if (g_prompt.frame != g.frameNo) { g_prompt.frame = g.frameNo; g_prompt.kind = kPromptNone; g_prompt.leader = false; }
	uint32_t& cc = *(uint32_t*)(gm + 0xCC);
	unsigned kind = (cc >> 4) & 0xF;
	bool leads = idx == t.Leader();
	if (kind != kPromptNone) {
		if (g_prompt.kind == kPromptNone || (leads && !g_prompt.leader)) { g_prompt.kind = kind; g_prompt.leader = leads; }
		else cc = (cc & ~0xF0u) | (g_prompt.kind << 4);
	} else if (g_prompt.kind != kPromptNone) {
		cc = (cc & ~0xF0u) | (g_prompt.kind << 4);
	}
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
	if (!g.p2Joined || g.suspended || next == 9 || next == 10) return next;
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
	CombosCheat();
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
	if (!g.p2Joined || g.suspended) return;
	if (heroIndex != g.p1Hero && heroIndex != g.p2Hero) return;

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
		// The magic potion's dash: while game manager +0x5C bit 19 is up, every
		// player-driven hero on the ground goes into situation 0x10 (dash), and
		// only the hero that drank gets the flag (+0x388 bit 0x4000) that ends it.
		// In single player the companion is an AI and never sees it; player 2's
		// Obelix dashed until a cutscene. Hidden from the non-leader's tick.
		CKYellowPages* yp = YellowPages();
		char* gm = yp ? (char*)yp->gameManager : nullptr;
		g.hidDash = gm && (*(uint32_t*)(gm + 0x5C) & 0x80000);
		if (g.hidDash) *(uint32_t*)(gm + 0x5C) &= ~0x80000u;
		// Offers meant for the real leader stay with it: the magic potion
		// (latched at trio+0x4C for one frame) must not be taken by the other
		// hero while it poses as leader.
		// The trio caches its nearby-enemy lists and distances once per frame,
		// from the leader (FUN_0047CB40, valid while +0x3F4 bit 0 is set):
		// lock-on and grabbing pick from them. Rebuild them for this hero, and
		// again after.
		*(uint32_t*)(trio + 0x3F4) &= ~1u;
		g.savedOffer = *(uint32_t*)(trio + 0x4C);
		*(uint32_t*)(trio + 0x4C) = 0;
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
// spinning, or the other player too far: the other hero's "no".
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
	if (!spinning || apart > kPassRange || g.held[other] || !HeroModePlayable(o.Flags18())) {
		Log("coop: %s pressed LB: %s", HeroName(idx), !spinning ? "no Roman spinning" : apart > kPassRange ?
		    "the other player is too far" : "the other player is busy");
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

void CoopAfterLifeTick(int heroIndex, Hero hero) {
	(void)heroIndex; (void)hero;
	P2CamEndHeroTick();
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
			// Getting on a machine (X at a machinegun: trio 0x4809, mode 2) -- the
			// trio's mode events act on the leader, and back as the companion the
			// hero was made the AI companion of that mode, unable to shoot (level 3
			// sector 6).
			uint8_t modeNow = sh ? HeroMode(*F18(sh)) : 0;
			bool startedMode = sh && HeroModePlayable(g.f18Before) && (modeNow == 1 || modeNow == 2);
			if (trio[0x2C] == g.slicedHero && trio[0x2D] == g.slicedOther && (startedGroup || startedTalk || startedMode)) {
				// It started a group action (a launcher, a lifter): that sequence
				// asks the trio for its leader all along, outside this tick too --
				// with the other hero still leading it kept putting that one on
				// the activator's spot. The hero that started it leads from now.
				Log("coop: %s started %s while not leading -- it leads until it is over", HeroName(g.slicedHero),
				    startedTalk ? "a Roman agent's dialogue" : startedMode ? "a machine (mode change)" : "a group action");
				g.groupTookLead = g.slicedHero;
				g.groupPrevLeader = g.savedLeader;
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
		if (im && g.savedBindings) *(char**)(im.raw() + 0x18) = g.savedBindings;
		g.savedBindings = nullptr;
		g.swapped = false;
	}
	WatchLbPass(heroIndex, hero);
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
			if (h && idx != t.Leader() && !g.held[idx] && sit != 0x00 && sit != 0xFF) StartAiCycle(idx, "lost the lead");
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
	              sizeof(kChooseAttackPrologue), "hero attack chooser (Twister for both players)");	PadsInit();
	if (g_config.p1Pad >= 0) PadIdentForce(g_config.p1Pad, PAD_ROLE_GAME);
	Log("coop: init (player 1 pad: %s)", g_config.p1Pad >= 0 ? "fixed by ini" : "auto-detect");
}

void CoopShutdown() {
	PadsShutdown();
}
