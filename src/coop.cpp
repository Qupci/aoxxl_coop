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

#include <cstdio>
#include <cstdlib>
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
	bool    p2BackCombo = false;   // Back is held for Back+A (warp), not to leave

	uint8_t p2Values[kP2ValuesSize] = {};
	bool    p2ValuesReady = false;

	uint8_t* savedValues = nullptr;   // CKInput's own array while player 2's is in
	char*    savedBindings = nullptr; // CKInput+0x18 likewise (hold time in seconds at +0x18 each)
	float    p2HoldStart[256];
	float    p2HoldTime[256] = {};
	bool     swapped = false;

	// The trio's leader/companion bytes while player 2's hero is ticking (see
	// CoopBeforeLifeTick): saved values, and the values we put there.
	bool     sliced = false;
	uint8_t  savedLeader = 0, savedCompanion = 0;
	uint32_t savedOffer = 0;          // trio+0x4C hidden from player 2's tick

	bool    suspended = false;
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
	if (!h || (idx >= 0 && idx < HERO_COUNT && g.held[idx])) return false;
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
			Log("coop: Start on %s also reached the game -- that is player 1's controller, "
				"player 2 has to use another one", PadName(pad));
			return;
		}
		const char* why = "";
		if (!CoopAllowedNow(t, &why)) {
			Log("coop: %s pressed Start but co-op cannot start now (%s)", PadName(pad), why);
			return;
		}
		g.p2Joined = true;
		g.p2Pad    = pad;
		g.p1Hero   = t.Leader();
		g.p2Hero   = OtherHero(g.p1Hero);
		// Joining in the middle of a scripted sequence (Asterix already seated in
		// the cable car, Obelix leading to pull it): player 1 keeps the hero the
		// game borrowed, and gets it back when the sequence is over.
		{
			Hero other = t.GetHero(g.p2Hero);
			int order = other ? (int)other.Order() : 0;
			if (other && !other.IsPlayerControlled() && IsScriptedOrder(order)) {
				g.p2Hero = g.p1Hero;
				g.p1Hero = OtherHero(g.p2Hero);
				g.held[g.p1Hero] = true;
				g.heldSince[g.p1Hero] = GetTickCount();
				g.lastOrder[g.p1Hero] = (uint8_t)order;
				Log("coop: joining mid-sequence: %s is the game's (AI order %d), player 1 gets it back after",
					HeroName(g.p1Hero), order);
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
int  CoopP1Hero()    { return g.p1Hero; }
int  CoopP2Pad()     { return g.p2Pad; }
bool CoopSuspended() { return g.suspended; }
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

// Puts `who` just above `to`'s position (it lands next to it; overlapping heroes
// push apart). Hero position +kHero_pos and the node's matrix (+0x40) both
// carry the position.
static void WarpToPartner(Hero who, Hero to) {
	Vector3 dst = to.Pos();
	dst.y += 1.0f;
	who.Pos() = dst;
	if (who.hook->node && who.hook->node->matrix) {
		float* hp = (float*)((char*)who.hook->node->matrix + 0x40);
		hp[0] = dst.x; hp[1] = dst.y; hp[2] = dst.z;
	}
	Log("coop: player 2's %s warped to %s (Back + A) at (%.1f %.1f %.1f)", HeroName(g.p2Hero),
	    HeroName(g.p1Hero), dst.x, dst.y, dst.z);
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
		if (g.p2Joined) {
			// The old hero objects are gone; there is nothing to hand back.
			ForgetP2();
			Log("coop: player 2 dropped for the level change (press Start to rejoin)");
		}
	}

	Trio t = GetTrio();
	{
		static bool dumped = false;
		if (!dumped && t && getenv("XXL_DUMPBIND")) { dumped = true; TraceDumpBindings(); }
	}
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
	// Start and Back are left alone during a cutscene: Start may be what skips it.
	bool inCutscene = g.cutsceneHold > 0 || CutsceneBars();
	if (!inCutscene && (p.pressed & PAD_START)) { Release("pressed Start"); return; }
	if (!inCutscene && (p.buttons & PAD_BACK) && (p.pressed & PAD_A)) g.p2BackCombo = true;
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

	// Back + A: player 2's hero joins player 1's (lost, stuck, or fell behind).
	if (g.p2BackCombo && (p.pressed & PAD_A) && !inCutscene && !g.suspended && !g.held[g.p1Hero] &&
	    !g.held[g.p2Hero])
		WarpToPartner(h2, h1);

	// Scripted modes belong to the game. So does a group action player 1 starts
	// (situation 13: a lifter waiting for the trio, and the like): it sends "the
	// companion" to a marker and waits for it, which only an AI companion does.
	bool groupAction = Situation(h1) == kSituationGroupAction;
	// Cutscenes: the letterbox bars, held a few frames after they go so a scene
	// handing over to the next one does not flicker co-op on and off.
	LogCutsceneSignals();
	if (CutsceneBars()) g.cutsceneHold = kCutsceneHoldFrames;
	else if (g.cutsceneHold > 0) g.cutsceneHold--;
	bool cinematic = g.cutsceneHold > 0;
	bool scripted = !HeroModePlayable(h1.Flags18()) || !HeroModePlayable(h2.Flags18()) ||
	                h1.Busy() || h2.Busy() || groupAction || cinematic;
	if (scripted != g.suspended) {
		g.suspended = scripted;
		Log("coop: %s (%s %s / %s %s%s%s)", scripted ? "suspended for a scripted mode" : "resumed",
			HeroName(g.p1Hero), ModeName(HeroMode(h1.Flags18())),
			HeroName(g.p2Hero), ModeName(HeroMode(h2.Flags18())),
			cinematic ? ", cutscene" : "", groupAction ? ", group action" : "");
		if (scripted) {
			// Hand player 2's hero to the game for the duration.
			uint8_t f = *F18(h2);
			if (HeroModePlayable(f) && HeroIsPlayerVariant(f) && t.Leader() != g.p2Hero)
				*F18(h2) = HeroAiVariant(f);
		}
		CoopLogState(scripted ? "suspend" : "resume");
	}

	InputManager im = GetInputManager();
	int n = im.NumActions();
	if (n > (int)kMaxActions) n = (int)kMaxActions;
	if (n > 0 && BuildP2Actions(g.p2Pad, g.p2Values, n)) {
		memset(g.p2Values + n, 0, kP2ValuesSize - n);
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

// Events the non-leading player hero sets off in its own update. Hero code
// flags some level events only for a player-controlled hero (+0x18 bit 0
// clear) -- in single player that is the leader alone, the AI companion never
// runs those paths. In co-op both heroes are player-driven, and player 2's
// Obelix walking onto the spinning platform's blue circle ran "switch to
// Obelix, Asterix waits here" with Asterix elsewhere. Around that hero's update
// the event service's flag table (yellow pages +0x6C: 3 bytes per event group
// at +0xC, flags in byte 2; group counts at +0x1C/+0x1E/+0x20) is snapshot,
// and whatever it newly flagged is taken back.
namespace {
uint8_t g_eventSnap[8192];
unsigned g_eventSnapN = 0;
bool g_eventSnapActive = false;
int g_eventSnapHero = -1;

char* EventService(unsigned* groups) {
	CKYellowPages* yp = YellowPages();
	char* ev = yp ? *(char**)((char*)yp + 0x6C) : nullptr;
	if (!ev || !*(uint8_t**)(ev + 0xC)) return nullptr;
	*groups = (unsigned)*(uint16_t*)(ev + 0x1C) + *(uint16_t*)(ev + 0x1E) + *(uint16_t*)(ev + 0x20);
	return ev;
}

void SnapEvents(int hero) {
	unsigned n = 0;
	char* ev = EventService(&n);
	g_eventSnapActive = false;
	if (!ev || n == 0 || n > sizeof(g_eventSnap)) return;
	uint8_t* tab = *(uint8_t**)(ev + 0xC);
	for (unsigned i = 0; i < n; i++) g_eventSnap[i] = tab[i * 3 + 2];
	g_eventSnapN = n;
	g_eventSnapHero = hero;
	g_eventSnapActive = true;
}

void UndoNewEvents() {
	if (!g_eventSnapActive) return;
	g_eventSnapActive = false;
	unsigned n = 0;
	char* ev = EventService(&n);
	if (!ev || n != g_eventSnapN) return;
	uint8_t* tab = *(uint8_t**)(ev + 0xC);
	for (unsigned i = 0; i < n; i++) {
		uint8_t added = tab[i * 3 + 2] & ~g_eventSnap[i];
		if (!added) continue;
		tab[i * 3 + 2] &= ~added;
		for (int b = 0; b < 8; b++)
			if (added & (1 << b))
				Log("coop: event %04X set off by %s (not leading) -- dropped", (i << 3) | b,
					HeroName(g_eventSnapHero));
	}
}
}

void CoopBeforeLifeTick(int heroIndex, Hero hero) {
	(void)hero;
	if (!g.p2Joined || g.suspended) return;
	if (heroIndex != g.p1Hero && heroIndex != g.p2Hero) return;

	// The Life reads the control bits first thing in its tick, so this is the
	// moment they have to be right.
	Trio t = GetTrio();
	if (!t) return;
	if (g.held[heroIndex]) return;   // the game's AI drives it for now
	if (heroIndex != t.Leader()) SnapEvents(heroIndex);
	EnforcePlayer(t, heroIndex);
	if (Situation(t.GetHero(g.p1Hero)) != kSituationGroupAction &&
	    Situation(t.GetHero(g.p2Hero)) != kSituationGroupAction)
		ClearStaleAiState(heroIndex, hero);

	// Player 1 steers relative to its own camera while player 2's hero leads.
	if (heroIndex == g.p1Hero && t.Leader() == g.p2Hero) P1CamBeginHeroTick();

	if (heroIndex != g.p2Hero || !g.p2ValuesReady) return;
	InputManager im = GetInputManager();
	if (!im) return;
	uint8_t** values = (uint8_t**)(im.raw() + 0x1C);
	g.savedValues = *values;
	*values = g.p2Values;
	// Some states read how long a binding has been held instead (FUN_00435900:
	// CKInput+0x18, 0x20 bytes per binding, seconds at +0x18) -- Obelix holding
	// the cable car's rope (situation 3) keeps hold while X is. Player 2 gets a copy of the
	// bindings carrying his own values there.
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
	// Offers meant for the real leader stay with it: the magic potion (CKHkTorch
	// checks the leader is Asterix, then latches itself at trio+0x4C for one
	// frame) must not be taken by player 2's Obelix while he poses as leader.
	// The trio caches its nearby-enemy lists and distances once per frame, from
	// the leader (FUN_0047CB40, valid while +0x3F4 bit 0 is set): lock-on and
	// grabbing pick from them. Rebuild them for player 2's hero, and again after.
	*(uint32_t*)(trio + 0x3F4) &= ~1u;
	g.savedOffer = 0;
	if (g.savedLeader != (uint8_t)g.p2Hero) {
		g.savedOffer = *(uint32_t*)(trio + 0x4C);
		*(uint32_t*)(trio + 0x4C) = 0;
	}

	// With split-screen, player 2 steers relative to player 2's own camera.
	P2CamBeginHeroTick();
}

void CoopAfterLifeTick(int heroIndex, Hero hero) {
	(void)heroIndex; (void)hero;
	UndoNewEvents();
	P2CamEndHeroTick();
	if (g.sliced) {
		g.sliced = false;
		Trio t = GetTrio();
		if (t) {
			uint8_t* trio = (uint8_t*)t.raw();
			*(uint32_t*)(trio + 0x3F4) &= ~1u;
			if (g.savedOffer && *(uint32_t*)(trio + 0x4C) == 0) *(uint32_t*)(trio + 0x4C) = g.savedOffer;
			g.savedOffer = 0;
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
	if (im && g.savedBindings) *(char**)(im.raw() + 0x18) = g.savedBindings;
	g.savedBindings = nullptr;
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
	if (!g.p2Joined) return 0;
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
		if (!HeroModePlayable(f) || !HeroIsPlayerVariant(f)) continue;
		fr.used[i] = true;
		fr.order[i] = (uint8_t)h.Order();
		*F18(h) = HeroAiVariant(f);
		uint8_t* ai = (uint8_t*)(h.raw() + 0x561);
		if (*ai == 0xFF) { *ai = 0; fr.idled[i] = true; }
	}
	return g_eventDepth;
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
		if (i != t.Leader() && order != fr.order[i] && IsScriptedOrder(order)) {
			char why[32];
			sprintf(why, "event %04X", ev);
			Hold(t, i, why);
		}
	}
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
