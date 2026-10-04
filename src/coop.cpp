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
#include "autotest.h"
#include "config.h"
#include "inputmgr.h"
#include "life.h"
#include "log.h"
#include "p2input.h"
#include "padident.h"
#include "pads.h"

#include <cstring>

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

	uint8_t p2Values[kP2ValuesSize] = {};
	bool    p2ValuesReady = false;

	uint8_t* savedValues = nullptr;   // CKInput's own array while player 2's is in
	bool     swapped = false;

	// The trio's leader/companion bytes while player 2's hero is ticking (see
	// CoopBeforeLifeTick): saved values, and the values we put there.
	bool     sliced = false;
	uint8_t  savedLeader = 0, savedCompanion = 0;

	bool    suspended = false;
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

// Hero situation-machine state (hero+0x3E9) of group actions: FUN_0049C030, which
// sends the companion and Idefix to markers and waits for them (lifters).
const uint8_t kSituationGroupAction = 0x0D;

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
	if (!h) return false;
	uint8_t f = *F18(h);
	if (!HeroModePlayable(f) || HeroIsPlayerVariant(f)) return false;
	*F18(h) = HeroPlayerVariant(f);
	InterlockedIncrement(&g_hookCounters.enforced);
	LogV("coop: %s -> player (f18 %02X -> %02X)", HeroName(idx), f, *F18(h));
	return true;
}

uint8_t Situation(Hero h) { return h ? *(uint8_t*)(h.raw() + 0x3E9) : 0xFF; }

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
	if (!HeroModePlayable(a.Flags18()) || !HeroModePlayable(o.Flags18())) { *why = "scripted mode"; return false; }
	if (a.Busy() || o.Busy()) { *why = "hero busy"; return false; }
	return true;
}

void ForgetP2() {
	g.p2Joined = false;
	g.p2Pad = -1;
	g.p1Hero = g.p2Hero = -1;
	g.p2BackHeldAt = 0;
	g.p2ValuesReady = false;
	g.suspended = false;
}

// Hand player 2's hero back to the game: make player 1's hero the trio leader
// again if a switch moved leadership, then put player 2's hero on AI, following.
void Release(const char* why) {
	Trio t = GetTrio();
	int p1 = g.p1Hero, p2 = g.p2Hero;
	ForgetP2();   // first, so our trio-event wrapper stays out of the way
	if (t && p1 >= 0 && p2 >= 0) {
		Hero h1 = t.GetHero(p1), h2 = t.GetHero(p2);
		if (t.Leader() == p2 && h1 && h2) {
			// The switch handler only accepts a target that looks like a companion.
			*F18(h1) = HeroAiVariant(*F18(h1));
			t.SwitchTo(p1);
		}
		if (h2 && HeroModePlayable(*F18(h2))) *F18(h2) = HeroAiVariant(*F18(h2));
		t.CompanionFollow();
		Log("coop: player 2 left (%s); %s is AI again, leader=%s", why, HeroName(p2),
			HeroName(t.Leader()));
	} else {
		Log("coop: player 2 left (%s)", why);
	}
}

void TryJoin(Trio t) {
	if (g.pendingPad >= 0) {
		if (PadIdentGameEventRecently(0)) g.pendingEcho = true;
		if (g.frameNo - g.pendingFrame < (uint32_t)kJoinSettleFrames) return;
		int pad = g.pendingPad;
		g.pendingPad = -1;
		if (g.pendingEcho || PadIdentRole(pad) == PAD_ROLE_GAME) {
			Log("coop: Start on pad %d also reached the game -- that is player 1's controller, "
				"player 2 has to press Start on the other one", pad);
			return;
		}
		const char* why = "";
		if (!CoopAllowedNow(t, &why)) {
			Log("coop: pad %d pressed Start but co-op cannot start now (%s)", pad, why);
			return;
		}
		g.p2Joined = true;
		g.p2Pad    = pad;
		g.p1Hero   = t.Leader();
		g.p2Hero   = OtherHero(g.p1Hero);
		g.p2BackHeldAt = 0;
		g.suspended = false;
		Log("coop: player 2 joined on pad %d as %s (player 1 keeps %s)", pad,
			HeroName(g.p2Hero), HeroName(g.p1Hero));
		CoopLogState("join");
		return;
	}

	for (int pad = 0; pad < kMaxPads; pad++) {
		const PadState& p = PadGet(pad);
		if (!p.connected || !(p.pressed & PAD_START)) continue;
		if (PadIdentRole(pad) == PAD_ROLE_GAME) {
			Log("coop: Start on pad %d ignored -- the game reads that pad, it is player 1's", pad);
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
int  CoopP1Hero()    { return g.p1Hero; }
int  CoopP2Pad()     { return g.p2Pad; }
bool CoopSuspended() { return g.suspended; }

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
		if (g.p2Joined) {
			// The old hero objects are gone; there is nothing to hand back.
			ForgetP2();
			Log("coop: player 2 dropped for the level change (press Start to rejoin)");
		}
	}

	Trio t = GetTrio();
	if (!g.p2Joined) {
		TryJoin(t);
		return;
	}

	const PadState& p = PadGet(g.p2Pad);
	if (!p.connected) { Release("pad disconnected"); return; }
	if (PadIdentRole(g.p2Pad) == PAD_ROLE_GAME) {
		Release("that pad also drives player 1 -- player 2 has to use the other controller");
		return;
	}
	if (p.pressed & PAD_START) { Release("pressed Start"); return; }
	if (p.buttons & PAD_BACK) {
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

	// Scripted modes belong to the game. So does a group action player 1 starts
	// (situation 13: a lifter waiting for the trio, and the like): it sends "the
	// companion" to a marker and waits for it, which only an AI companion does.
	bool groupAction = Situation(h1) == kSituationGroupAction;
	bool scripted = !HeroModePlayable(h1.Flags18()) || !HeroModePlayable(h2.Flags18()) ||
	                h1.Busy() || h2.Busy() || groupAction;
	if (scripted != g.suspended) {
		g.suspended = scripted;
		Log("coop: %s (%s %s / %s %s)", scripted ? "suspended for a scripted mode" : "resumed",
			HeroName(g.p1Hero), ModeName(HeroMode(h1.Flags18())),
			HeroName(g.p2Hero), ModeName(HeroMode(h2.Flags18())));
		CoopLogState(scripted ? "suspend" : "resume");
	}

	InputManager im = GetInputManager();
	int n = im.NumActions();
	if (n > (int)kMaxActions) n = (int)kMaxActions;
	if (n > 0 && BuildP2Actions(g.p2Pad, g.p2Values, n)) {
		memset(g.p2Values + n, 0, kP2ValuesSize - n);
		g.p2ValuesReady = true;
	}

	if (!g.suspended) {
		EnforcePlayer(t, g.p2Hero);
		EnforcePlayer(t, g.p1Hero);
	}
}

// ---------------------------------------------------------------------------
// Per-hero Life tick
// ---------------------------------------------------------------------------

void CoopBeforeLifeTick(int heroIndex, Hero hero) {
	(void)hero;
	if (!g.p2Joined || g.suspended) return;
	if (heroIndex != g.p1Hero && heroIndex != g.p2Hero) return;

	// The Life reads the control bits first thing in its tick, so this is the
	// moment they have to be right.
	Trio t = GetTrio();
	if (!t) return;
	EnforcePlayer(t, heroIndex);
	if (Situation(t.GetHero(g.p1Hero)) != kSituationGroupAction &&
	    Situation(t.GetHero(g.p2Hero)) != kSituationGroupAction)
		ClearStaleAiState(heroIndex, hero);

	if (heroIndex != g.p2Hero || !g.p2ValuesReady) return;
	InputManager im = GetInputManager();
	if (!im) return;
	uint8_t** values = (uint8_t**)(im.raw() + 0x1C);
	g.savedValues = *values;
	*values = g.p2Values;
	g.swapped = true;
	InterlockedIncrement(&g_hookCounters.p2Routed);

	// Player-side hero code assumes the hero running it is the trio's leader:
	// it asks the trio for "the companion" (trio+0x2D, or vtable +0x98) and means
	// the *other* hero. Run as the companion, it gets itself back -- the lifter
	// handler FUN_0049C030 then dereferences its own empty marker slot and
	// crashes (axxl.exe+0x9C228). So for the length of player 2's tick, the trio
	// says what that code expects: player 2's hero leads, player 1's follows.
	uint8_t* trio = (uint8_t*)t.raw();
	g.savedLeader    = trio[0x2C];
	g.savedCompanion = trio[0x2D];
	trio[0x2C] = (uint8_t)g.p2Hero;
	trio[0x2D] = (uint8_t)g.p1Hero;
	g.sliced = true;
}

void CoopAfterLifeTick(int heroIndex, Hero hero) {
	(void)heroIndex; (void)hero;
	if (g.sliced) {
		g.sliced = false;
		Trio t = GetTrio();
		if (t) {
			uint8_t* trio = (uint8_t*)t.raw();
			if (trio[0x2C] == (uint8_t)g.p2Hero && trio[0x2D] == (uint8_t)g.p1Hero) {
				trio[0x2C] = g.savedLeader;
				trio[0x2D] = g.savedCompanion;
			} else {
				// A switch event ran during the tick and picked a new leader on
				// purpose; that is the game's decision now.
				Log("coop: the game changed the trio's leader during %s's tick (now %s)",
					HeroName(g.p2Hero), HeroName(trio[0x2C]));
			}
		}
	}
	if (!g.swapped) return;
	InputManager im = GetInputManager();
	if (im) *(uint8_t**)(im.raw() + 0x1C) = g.savedValues;
	g.swapped = false;
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

int CoopBeforeTrioEvent(int ev) {
	InterlockedIncrement(&g_hookCounters.trioEvents);
	if (!g.p2Joined) return 0;
	Log("coop: trio event %04X", ev);
	if (!IsModeOrSwitchEvent(ev)) return 0;

	// Show the trio the arrangement it was written for: only its leader drives.
	// The switch handler, for one, refuses to switch to a hero whose companion
	// bit is clear, and the mode events derive each hero's new state from it.
	Trio t = GetTrio();
	if (!t) return 0;
	for (int i = HERO_ASTERIX; i <= HERO_OBELIX; i++) {
		Hero h = t.GetHero(i);
		if (!h || i == t.Leader()) continue;
		uint8_t f = *F18(h);
		if (HeroModePlayable(f) && HeroIsPlayerVariant(f)) *F18(h) = HeroAiVariant(f);
	}
	return 1;
}

void CoopAfterTrioEvent(int ev, int cookie) {
	if (!cookie || !g.p2Joined) return;
	Trio t = GetTrio();
	if (!t) return;
	Log("coop: after trio event %04X leader=%s", ev, HeroName(t.Leader()));
	EnforcePlayer(t, g.p1Hero);
	EnforcePlayer(t, g.p2Hero);
}

// ---------------------------------------------------------------------------

void CoopInit() {
	PadsInit();
	if (g_config.p1Pad >= 0) PadIdentForce(g_config.p1Pad, PAD_ROLE_GAME);
	Log("coop: init (player 1 pad: %s)", g_config.p1Pad >= 0 ? "fixed by ini" : "auto-detect");
}

void CoopShutdown() {
	PadsShutdown();
}
