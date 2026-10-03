#include "coop.h"
#include "config.h"
#include "log.h"
#include "inputmgr.h"
#include "p2input.h"
#include "pads.h"
#include "probe.h"

#include <cstdio>
#include <cstring>
#include <cmath>

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

namespace {

struct CoopState {
	bool  p2Joined     = false;
	int   p2PadIndex   = -1;     // XInput slot player 2 claimed
	int   p2Hero       = -1;     // hero index player 2 drives
	DWORD p2BackHeldAt = 0;      // for the hold-to-drop gesture

	// Set while player 2's input is swapped in, so the after-tick hook knows it
	// has something to put back.
	bool    swapped = false;
	uint8_t savedActions[kMaxActions] = {};
	uint8_t p2Actions[kMaxActions] = {};
	int     savedActionCount = 0;
	uint8_t savedFlags18 = 0;
	uint8_t savedIgnore = 0;

	// Trio leader/companion indices, borrowed for the duration of P2's tick.
	bool    leaderSwapped = false;
	uint8_t savedLeader = 0;
	uint8_t savedCompanion = 0;

	// hero+0x3D4 / +0x3DC at three points: before our call to the game's input
	// reader, right after it, and after the hero's own state tick. Tells apart
	// "the reader did nothing" from "the tick overwrote it".
	float dbgBeforeX = 0, dbgBeforeY = 0;
	float dbgAfterReadX = 0, dbgAfterReadY = 0;
	float dbgAfterTickX = 0, dbgAfterTickY = 0;
	uint32_t dbgMoveFlags = 0;

	DWORD    lastFrameTick   = 0;
	DWORD    lastDiagTick    = 0;
	DWORD    lastProbeReport = 0;
	uint32_t frames          = 0;
	int      lastLevel       = -2;
} g;

} // namespace

bool CoopHeroIsPlayerDriven(int heroIndex) {
	return g.p2Joined && g_config.p2Control && heroIndex >= 0 && heroIndex == g.p2Hero;
}

bool           CoopP2Joined() { return g.p2Joined; }
int            CoopP2Hero() { return g.p2Hero; }
int            CoopP2Pad() { return g.p2PadIndex; }
const uint8_t* CoopP2ActionSnapshot() { return g.p2Actions; }
int            CoopP2ActionCount() { return g.savedActionCount; }

unsigned CoopP2MoveFlags() { return g.dbgMoveFlags; }

void CoopP2DebugAxes(float* out6) {
	out6[0] = g.dbgBeforeX;    out6[1] = g.dbgBeforeY;
	out6[2] = g.dbgAfterReadX; out6[3] = g.dbgAfterReadY;
	out6[4] = g.dbgAfterTickX; out6[5] = g.dbgAfterTickY;
}

bool CoopGameplayLive() {
	if (!KalLevelIsLoaded()) return false;
	Trio t = GetTrio();
	if (!t) return false;
	Hero leader = t.GetHero(t.Leader());
	return leader.Exists() && leader.IsPlayerControlled();
}

HeroCommandBlock* GetCommandBlock() {
	CKYellowPages* yp = YellowPages();
	if (!yp || !yp->gameManager) return nullptr;
	void* block = *(void**)((char*)yp->gameManager + 0x40);
	return (HeroCommandBlock*)block;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

static void DumpCommandBlock() {
	HeroCommandBlock* cb = GetCommandBlock();
	if (!cb) { Log("diag: no command block"); return; }

	Log("diag: command block @ %p  ctrl=%08X move=(%.3f %.3f %.3f) ignore=%d heading=%.3f",
		cb, cb->controlBits, cb->moveDir.x, cb->moveDir.y, cb->moveDir.z,
		(int)cb->forceIgnore, cb->heading);

	// Raw hex so the unmapped fields (buttons, most likely) can be decoded by
	// diffing dumps taken while pressing things.
	const uint8_t* p = (const uint8_t*)cb;
	char line[128];
	for (size_t off = 0; off < kCommandBlockSize; off += 16) {
		int n = sprintf(line, "  %03X:", (unsigned)off);
		for (size_t i = 0; i < 16 && off + i < kCommandBlockSize; i++)
			n += sprintf(line + n, " %02X", p[off + i]);
		Log("%s", line);
	}
}

static void DumpTrio(Trio t) {
	if (!t) { Log("diag: no trio"); return; }
	Log("diag: trio @ %p  leader=%d(%s) companion=%d(%s) flags=%08X switchRadius=%.2f",
		t.grp, t.Leader(), HeroName(t.Leader()),
		t.Companion(), HeroName(t.Companion()), t.Flags(), t.SwitchRadius());
	for (int i = 0; i < HERO_COUNT; i++) {
		Hero h = t.GetHero(i);
		if (!h) { Log("  %-8s <null>", HeroName(i)); continue; }
		Log("  %-8s @ %p companion=%d ctrl=%X ignoreCmd=%d busy=%d order=%d(%s) action=%02X "
			"pos=(%.1f %.1f %.1f) f390=%08X f884=%08X",
			HeroName(i), h.hook, h.IsCompanion(), h.ControlBits(), h.IgnoresCommands(),
			h.Busy(), h.Order(), AiOrderName(h.Order()), h.Action(),
			h.Pos().x, h.Pos().y, h.Pos().z, h.Flags390(), h.Flags884());
	}
	Log("  instances: asterix=%u obelix=%u idefix=%u",
		GetNumLvlClassInstances(ax::kCKHkAsterix),
		GetNumLvlClassInstances(ax::kCKHkObelix),
		GetNumLvlClassInstances(ax::kCKHkIdefix));
}

// ---------------------------------------------------------------------------
// Join / drop
// ---------------------------------------------------------------------------

// Co-op can only be armed when both heroes actually exist and neither is locked
// into a scripted sequence. This is what keeps the mod quiet during the opening
// of level 1 (no Obelix yet), belly/boat rides and cutscenes.
static bool CoopAllowedNow(Trio t, const char** why) {
	if (!t)                              { *why = "no trio";            return false; }
	if (GetNumLvlClassInstances(ax::kCKHkObelix) == 0) { *why = "Obelix not in level"; return false; }

	Hero a = t.GetHero(HERO_ASTERIX);
	Hero o = t.GetHero(HERO_OBELIX);
	if (!a || !o)                        { *why = "hero missing";       return false; }
	if (a.Busy() || o.Busy())            { *why = "hero busy";          return false; }
	// Exactly one of them should be the player's; anything else means the game
	// is mid-transition or has both locked into a scripted sequence.
	if (a.IsCompanion() == o.IsCompanion()) { *why = "no single leader"; return false; }
	return true;
}

// Hand player 2's hero back to its AI: clear the "no AI action" marker we hold
// on it and put it back to following the leader. Without this the hero is left
// inert after player 2 leaves.
static void ReleaseP2Hero(const char* why) {
	Trio t = GetTrio();
	if (t && g.p2Hero >= 0) {
		Hero h = t.GetHero(g.p2Hero);
		if (h) {
			*(uint8_t*)(h.raw() + 0x18) |= 1;      // companion again
			*(uint8_t*)(h.raw() + 0x561) = 0;      // let the AI state machine run
			h.SetOrder(AI_FOLLOW_LEADER);
		}
	}
	Log("coop: player 2 released %s (%s)",
		g.p2Hero >= 0 ? HeroName(g.p2Hero) : "?", why);
	g.p2Joined = false;
	g.p2PadIndex = -1;
	g.p2Hero = -1;
	g.p2BackHeldAt = 0;
}

static void TryJoin(Trio t) {
	int pad = PadFindPressed(PAD_START, -1);
	if (pad < 0) return;

	const char* why = "";
	if (!CoopAllowedNow(t, &why)) {
		Log("coop: pad %d asked to join but cannot right now (%s)", pad, why);
		return;
	}

	g.p2Joined   = true;
	g.p2PadIndex = pad;
	g.p2Hero     = t.Companion();
	Log("coop: player 2 joined on pad %d, taking %s", pad, HeroName(g.p2Hero));
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void CoopFrame() {
	// Called from CKHkAsterixLife::tick, i.e. once per rendered gameplay frame.
	DWORD now = GetTickCount();
	g.frames++;
	if (g.lastFrameTick == 0) {
		Log("coop: first gameplay frame");
		g.lastFrameTick = now;
	} else if ((int)(now - g.lastFrameTick) >= 5000) {
		Log("coop: %u frames (%.1f fps)", g.frames,
			g.frames * 1000.0 / (double)(now - g.lastFrameTick));
		g.frames = 0;
		g.lastFrameTick = now;
	}

	PadsPoll();
	ProbeFrame();

	if (g_config.inputProbe &&
	    (int)(now - g.lastProbeReport) >= g_config.probeReportSecs * 1000) {
		g.lastProbeReport = now;
		ProbeReport();
	}

	Trio t = GetTrio();

	// Reset co-op state across level transitions.
	CKYellowPages* yp = YellowPages();
	int level = (yp && yp->gameManager)
		? *(int*)((char*)yp->gameManager + ax::kGM_currentLevel) : -1;
	if (level != g.lastLevel) {
		Log("coop: level changed %d -> %d", g.lastLevel, level);
		g.lastLevel = level;
		if (g.p2Joined) {
			// The old hero objects are gone by now, so just forget the state.
			g.p2Joined = false;
			g.p2PadIndex = -1;
			g.p2Hero = -1;
			Log("coop: dropped player 2 for the level transition");
		}
	}

	// While probing, the pad belongs to the probe: Start/Back have to register as
	// data, not as join/drop gestures.
	if (ProbeActive()) {
		// fall through to diagnostics only
	} else if (!g.p2Joined) {
		TryJoin(t);
	} else {
		const PadState& p = PadGet(g.p2PadIndex);
		if (!p.connected) {
			ReleaseP2Hero("pad disconnected");
		} else {
			// Hold Back to leave co-op.
			if (p.buttons & PAD_BACK) {
				if (g.p2BackHeldAt == 0) g.p2BackHeldAt = now;
				else if ((int)(now - g.p2BackHeldAt) >= g_config.dropHoldMs)
					ReleaseP2Hero("held Back");
			} else {
				g.p2BackHeldAt = 0;
			}

			// With p2_control off the mod falls back to its earlier behaviour:
			// Start hands the camera and control to the other hero, the same
			// thing the blue circles do. With it on, Start is left alone so it
			// does not fight the input player 2 is actually giving.
			if (!g_config.p2Control && t && (p.pressed & PAD_START)) {
				const char* why = "";
				if (CoopAllowedNow(t, &why)) {
					t.Toggle();
					g.p2Hero = t.Companion();
					Log("coop: switched, leader is now %s", HeroName(t.Leader()));
				} else {
					Log("coop: switch refused (%s)", why);
				}
			}
		}
	}

	if (g_config.diagnostics && (int)(now - g.lastDiagTick) >= g_config.diagIntervalMs) {
		g.lastDiagTick = now;
		DumpTrio(t);
		DumpCommandBlock();
	}
}

// ---------------------------------------------------------------------------
// Per-hero tick wrappers
// ---------------------------------------------------------------------------

// While player 2's hero is ticking it must look like the player's hero rather
// than the AI companion, or the state machine keeps resetting its AI order and
// ignores the command input entirely. Both bits are put back immediately
// afterwards, so CKGrpTrio still sees the arrangement it expects.
void CoopBeforeHeroTick(int heroIndex, Hero hero) {
	g.swapped = false;

	// Diagnostic: blank the action array for player 1's hero. If Asterix still
	// moves with this on, the hero does not read input anywhere inside this
	// bracket and the whole swap approach is hooked at the wrong place.
	if (g_config.testBlockP1 && heroIndex == HERO_ASTERIX && hero) {
		InputManager im = GetInputManager();
		uint8_t* values = im ? im.Values() : nullptr;
		int n = im.NumActions();
		if (values && n > 0) {
			if (n > (int)kMaxActions) n = (int)kMaxActions;
			memcpy(g.savedActions, values, n);
			memset(values, 0, n);
			g.savedActionCount = n;
			g.savedFlags18 = hero.Flags18();
			g.savedIgnore = *(uint8_t*)(hero.raw() + 0x1A);
			g.swapped = true;
			return;
		}
	}

	if (!g.p2Joined || heroIndex != g.p2Hero || !hero) return;
	if (!g_config.p2Control) return;
	// The guard sampler wants the array untouched by us, or our own swap is the
	// only thing it ever catches.
	if (g_config.watchActionReads) return;

	InputManager im = GetInputManager();
	uint8_t* values = im ? im.Values() : nullptr;
	int n = im.NumActions();
	if (!values || n <= 0) { g_hookCounters.p2SwapSkips++; return; }
	if (n > (int)kMaxActions) n = (int)kMaxActions;

	if (!BuildP2Actions(g.p2PadIndex, g.p2Actions, n)) {
		g_hookCounters.p2SwapSkips++;
		return;
	}
	g_hookCounters.p2Swaps++;

	// Order mode drives the hero through the AI interface instead of pretending
	// he is a second player-controlled hero, so the input swap and the
	// companion-bit override are both unwanted here: the AI machine must see the
	// hero exactly as it normally would.
	if (g_config.p2OrderMode) {
		P2DriveByOrder(GetTrio(), hero, g.p2PadIndex);
		return;
	}

	memcpy(g.savedActions, values, n);
	memcpy(values, g.p2Actions, n);
	g.savedActionCount = n;

	g.savedFlags18 = hero.Flags18();
	g.savedIgnore  = *(uint8_t*)(hero.raw() + 0x1A);
	*(uint8_t*)(hero.raw() + 0x18) = (uint8_t)(g.savedFlags18 & ~1u);  // not the companion
	*(uint8_t*)(hero.raw() + 0x1A) = 0;                                // read commands

	// Time-slice the leader role.
	//
	// "Player" is not a property of a hero in this engine -- it is the role held
	// by the trio's leader, and there is exactly one, indexed by the single byte
	// at trio+0x2C. FUN_004815C0 builds the movement basis from
	// `trio + 0x30 + trio[0x2C]*4` unconditionally, and the player input path is
	// reached only for that hero. Clearing the hero's own companion bit was never
	// going to be enough, because nothing asks the hero -- it asks the trio.
	//
	// So while player 2's hero is updating, the trio is told he is the leader,
	// and it is put back immediately afterwards. Each hero gets the full player
	// treatment during its own tick and the engine never sees two leaders.
	Trio t = GetTrio();
	if (t) {
		g.savedLeader    = (uint8_t)t.Leader();
		g.savedCompanion = (uint8_t)t.Companion();
		g.leaderSwapped  = true;
		*(uint8_t*)(t.raw() + 0x2C) = (uint8_t)heroIndex;
		*(uint8_t*)(t.raw() + 0x2D) =
			(uint8_t)(heroIndex == HERO_ASTERIX ? HERO_OBELIX : HERO_ASTERIX);
	}

	// Nothing else is forced here. The hero's own action state machine
	// (FUN_0049DE10) runs normally and its handlers do the whole job -- read
	// input, transform it, set the movement flags, apply speed and gravity --
	// provided it believes this hero is the player's, which is what clearing
	// bit 0 of +0x18 above achieves.
	//
	// Earlier builds hand-rolled that sequence (FUN_0049A3B0 / FUN_0048ABF0 /
	// FUN_0048ACA0 / FUN_0049A1A0 / FUN_004A1BF0 plus the movement bitfields) and
	// forced hero+0x561 to 0xFF. That reproduced the animation but never the
	// locomotion, because the real work lives inside the state handlers rather
	// than in those calls.

	g.dbgBeforeX = *(float*)(hero.raw() + 0x3D4);
	g.dbgBeforeY = *(float*)(hero.raw() + 0x3DC);
	g.dbgMoveFlags = *(uint32_t*)(hero.raw() + 0x384);

	g.swapped = true;
}

void CoopAfterHeroTick(int heroIndex, Hero hero) {
	// Re-assert the steering order after the AI machine has run. Its handlers
	// reassign the order themselves (arrival, combat, "follow leader"), and a
	// reassignment that survives to the next frame is a frame of lost control.
	if (g_config.p2OrderMode && g.p2Joined && heroIndex == g.p2Hero && hero) {
		P2DriveByOrder(GetTrio(), hero, g.p2PadIndex);
		return;
	}
	(void)heroIndex;
	if (!g.swapped) return;
	g.swapped = false;

	if (hero) {
		g.dbgAfterTickX = *(float*)(hero.raw() + 0x3D4);
		g.dbgAfterTickY = *(float*)(hero.raw() + 0x3DC);
	}

	// Give the leader role straight back, before anything else in the frame can
	// observe the trio mid-swap.
	if (g.leaderSwapped) {
		Trio t = GetTrio();
		if (t) {
			*(uint8_t*)(t.raw() + 0x2C) = g.savedLeader;
			*(uint8_t*)(t.raw() + 0x2D) = g.savedCompanion;
		}
		g.leaderSwapped = false;
	}

	InputManager im = GetInputManager();
	uint8_t* values = im ? im.Values() : nullptr;
	if (values && g.savedActionCount > 0)
		memcpy(values, g.savedActions, g.savedActionCount);

	if (hero) {
		// Preserve whatever the tick changed in the other bits; only restore
		// the companion bit we cleared.
		uint8_t now = hero.Flags18();
		*(uint8_t*)(hero.raw() + 0x18) = (uint8_t)((now & ~1u) | (g.savedFlags18 & 1u));
		*(uint8_t*)(hero.raw() + 0x1A) = g.savedIgnore;
	}
}

// ---------------------------------------------------------------------------

void CoopInit() {
	Log("coop: init");
	PadsInit();
}

void CoopShutdown() {
	PadsShutdown();
}



