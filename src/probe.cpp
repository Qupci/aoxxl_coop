#include "probe.h"
#include "coop.h"
#include "config.h"
#include "inputmgr.h"
#include "log.h"
#include "pads.h"

#include <cstdio>
#include <cstring>
#include <cmath>

namespace {

// --- CKInput action array ---------------------------------------------------

uint8_t  g_prevActions[kMaxActions] = {};
uint8_t  g_curActions[kMaxActions]  = {};
uint8_t  g_numActions = 0;
bool     g_haveActions = false;
bool     g_loggedLayout = false;

// How often action i was non-zero while pad button b was held, and while
// nothing at all was held. An action that lights up with exactly one button and
// stays dark otherwise is that button.
uint32_t g_actHeld[kMaxActions][16] = {};
uint32_t g_actIdle[kMaxActions]     = {};
uint32_t g_btnHeldFrames[16]        = {};
uint32_t g_idleFrames               = 0;
uint32_t g_frame                    = 0;

// Stick-direction correlation. Actions 0..3 (and their mirrors) are analog
// half-axes carrying 0x00..0xFF, so the mapping is found by holding the stick in
// each cardinal direction and seeing which index tracks the magnitude.
enum { DIR_LEFT, DIR_RIGHT, DIR_UP, DIR_DOWN, DIR_COUNT };
const char* const kDirNames[DIR_COUNT] = { "left", "right", "up", "down" };
uint32_t g_dirFrames[DIR_COUNT] = {};
uint32_t g_dirHits[kMaxActions][DIR_COUNT] = {};
uint32_t g_dirSum[kMaxActions][DIR_COUNT] = {};

// --- movement command block -------------------------------------------------

struct Snapshot {
	bool    valid = false;
	uint8_t bytes[kProbeBytes] = {};
};
Snapshot g_prevBlock, g_curBlock;

const char* ButtonName(int bit) {
	static const char* const kNames[16] = {
		"DPadUp", "DPadDown", "DPadLeft", "DPadRight",
		"Start", "Back", "LThumb", "RThumb",
		"LB", "RB", "bit10", "bit11",
		"A", "B", "X", "Y",
	};
	return kNames[bit & 15];
}

// The block and the action array live on the game's heap and we do not know how
// long either really is, so reads past the documented fields are guarded.
bool SafeRead(const void* src, void* dst, size_t n) {
	__try {
		memcpy(dst, src, n);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

void LogBlockDiff(const Snapshot& a, const Snapshot& b) {
	if (!a.valid || !b.valid) return;
	char storage[256];
	LineBuf line(storage);
	line.Add("    block:");
	int shown = 0;
	for (size_t i = 0; i < kProbeBytes; i++) {
		if (a.bytes[i] == b.bytes[i]) continue;
		if (shown == 10) { line.Add(" ..."); break; }
		line.Add(" %03X:%02X>%02X", (unsigned)i, a.bytes[i], b.bytes[i]);
		shown++;
	}
	if (shown == 0) return;  // nothing moved, not worth a line
	Log("%s", line.c_str());
}

void LogActionDiff() {
	char storage[256];
	LineBuf line(storage);
	line.Add("    actions:");
	int shown = 0;
	for (int i = 0; i < g_numActions; i++) {
		if (g_prevActions[i] == g_curActions[i]) continue;
		if (shown == 12) { line.Add(" ..."); break; }
		line.Add(" [%d]%02X>%02X", i, g_prevActions[i], g_curActions[i]);
		shown++;
	}
	if (shown == 0) return;
	Log("%s", line.c_str());
}

} // namespace

bool ProbeActive() { return g_config.inputProbe; }

void ProbeFrame() {
	if (!g_config.inputProbe) return;
	g_frame++;

	// Refuse to collect data unless the player really is in control. Otherwise a
	// whole session gets spent measuring a level that loaded but never started.
	static bool wasLive = false;
	bool live = CoopGameplayLive();
	if (live != wasLive) {
		wasLive = live;
		Log(live ? "probe: GAMEPLAY LIVE -- now collecting, go ahead"
		         : "probe: gameplay not live (no hero in play) -- NOT collecting");
	}
	if (!live) return;

	InputManager im = GetInputManager();
	if (!im) {
		static bool warned = false;
		if (!warned) { Log("probe: CKInput manager (0,4,0) not found"); warned = true; }
		return;
	}

	if (!g_loggedLayout) {
		g_loggedLayout = true;
		Log("probe: CKInput manager @ %p  numActions=%u numDevices=%u values=%p",
			im.obj, im.NumActions(), im.NumDevices(), im.Values());
		// Only the inactive ones are worth naming; listing all 59 with "(off)"
		// suffixes is what overran the old fixed buffer.
		char storage[512];
		LineBuf line(storage);
		line.Add("probe: inactive bindings:");
		int off = 0;
		for (int i = 0; i < im.NumActions() && i < (int)kMaxActions; i++) {
			if (im.BindingActive(i)) continue;
			line.Add(" %d", i);
			off++;
		}
		if (!off) line.Add(" none");
		Log("%s (%d of %u off)", line.c_str(), off, im.NumActions());
	}

	// --- sample the action array ---
	g_numActions = im.NumActions();
	if (g_numActions > kMaxActions) g_numActions = kMaxActions;
	uint8_t* values = im.Values();
	memcpy(g_prevActions, g_curActions, sizeof(g_curActions));
	bool haveNow = values && SafeRead(values, g_curActions, g_numActions);
	bool hadPrev = g_haveActions;
	g_haveActions = haveNow;

	// --- sample the movement command block ---
	HeroCommandBlock* cb = GetCommandBlock();
	g_prevBlock = g_curBlock;
	g_curBlock.valid = cb && SafeRead(cb, g_curBlock.bytes, kProbeBytes);

	const PadState& p = PadGet(g_config.probePad);
	if (!p.connected) {
		static bool warned = false;
		if (!warned) { Log("probe: pad %d is not connected", g_config.probePad); warned = true; }
		return;
	}

	bool idle = (p.buttons == 0) && p.lx == 0 && p.ly == 0 && p.rx == 0 && p.ry == 0 &&
	            p.lt == 0 && p.rt == 0;

	// Correlate "action is non-zero" against "button is held". Held-state rather
	// than edges, because an action byte stays set for as long as the button is
	// down and that is far less sensitive to a frame of lag between our XInput
	// poll and the game's DirectInput poll.
	if (haveNow) {
		for (int i = 0; i < g_numActions; i++) {
			if (!g_curActions[i]) continue;
			if (idle) { g_actIdle[i]++; continue; }
			for (int b = 0; b < 16; b++)
				if (p.buttons & (1 << b)) g_actHeld[i][b]++;
		}
	}
	for (int b = 0; b < 16; b++)
		if (p.buttons & (1 << b)) g_btnHeldFrames[b]++;
	if (idle) g_idleFrames++;

	// Only sample a direction when the stick is clearly committed to it and not
	// far off-axis, so left/up do not contaminate each other.
	if (haveNow && p.buttons == 0) {
		int dir = -1;
		if (p.lx < -0.6f && fabsf(p.ly) < 0.4f)      dir = DIR_LEFT;
		else if (p.lx > 0.6f && fabsf(p.ly) < 0.4f)  dir = DIR_RIGHT;
		else if (p.ly > 0.6f && fabsf(p.lx) < 0.4f)  dir = DIR_UP;
		else if (p.ly < -0.6f && fabsf(p.lx) < 0.4f) dir = DIR_DOWN;
		if (dir >= 0) {
			g_dirFrames[dir]++;
			for (int i = 0; i < g_numActions; i++) {
				if (!g_curActions[i]) continue;
				g_dirHits[i][dir]++;
				g_dirSum[i][dir] += g_curActions[i];
			}
		}
	}

	// Log every edge as it happens so a single deliberate press is readable in
	// the raw log even before the summary.
	uint16_t edges = (uint16_t)(p.pressed | p.released);
	if (edges) {
		char storage[160];
		LineBuf names(storage);
		for (int b = 0; b < 16; b++) {
			if (!(edges & (1 << b))) continue;
			names.Add("%s%s%s", names.len ? "," : "",
			          (p.pressed & (1 << b)) ? "+" : "-", ButtonName(b));
		}
		Log("probe: f%u edge %s (held=%04X)", g_frame, names.c_str(), p.buttons);
		if (haveNow && hadPrev) LogActionDiff();
		LogBlockDiff(g_prevBlock, g_curBlock);
	}

	// Stick sampling confirms the movement vector mapping, and tells us straight
	// away whether the game is reading this pad at all.
	static uint32_t lastStickLog = 0;
	if ((p.lx != 0 || p.ly != 0) && g_frame - lastStickLog >= 30) {
		lastStickLog = g_frame;
		Log("probe: f%u stick=(%.2f %.2f) blockMove=(%.3f %.3f %.3f) heading=%.3f ctrl=%08X",
			g_frame, p.lx, p.ly,
			cb ? cb->moveDir.x : 0.f, cb ? cb->moveDir.y : 0.f, cb ? cb->moveDir.z : 0.f,
			cb ? cb->heading : 0.f, cb ? cb->controlBits : 0u);
	}
}

void ProbeReport() {
	if (!g_config.inputProbe) return;

	// A report is only meaningful once there is something to correlate against;
	// otherwise it is 59 lines of "never seen active" burying the real log.
	int buttonsSeen = 0;
	for (int b = 0; b < 16; b++)
		if (g_btnHeldFrames[b] >= 5) buttonsSeen++;
	if (g_frame < 120 || buttonsSeen == 0) {
		Log("probe: %u frames, %u idle, %d button(s) held so far -- "
			"nothing to report yet, keep playing", g_frame, g_idleFrames, buttonsSeen);
		return;
	}

	Log("=== probe report: %u frames, %u idle, %u actions ===",
		g_frame, g_idleFrames, g_numActions);

	int quiet = 0;
	for (int i = 0; i < g_numActions; i++) {
		// Which button best explains this action lighting up?
		int   best = -1;
		float bestScore = 0.0f;
		for (int b = 0; b < 16; b++) {
			if (g_btnHeldFrames[b] < 5) continue;
			float score = (float)g_actHeld[i][b] / (float)g_btnHeldFrames[b];
			if (score > bestScore) { bestScore = score; best = b; }
		}
		float idleRate = g_idleFrames ? (float)g_actIdle[i] / (float)g_idleFrames : 0.0f;

		if (best >= 0 && bestScore >= 0.5f && idleRate < 0.25f) {
			Log("  action[%2d] = %-9s  (on for %.0f%% of holds, %.0f%% while idle)",
				i, ButtonName(best), bestScore * 100.0f, idleRate * 100.0f);
		} else if (idleRate >= 0.25f) {
			Log("  action[%2d] : always-on / analog (%.0f%% while idle)", i, idleRate * 100.0f);
		} else if (best >= 0 && bestScore > 0.0f) {
			Log("  action[%2d] ? weak match %s (%.0f%%)", i, ButtonName(best), bestScore * 100.0f);
		} else {
			quiet++;  // never seen active; not worth a line each
		}
	}
	Log("  (%d of %u actions never went active)", quiet, g_numActions);

	Log("  -- stick axes (frames per direction: L=%u R=%u U=%u D=%u) --",
		g_dirFrames[DIR_LEFT], g_dirFrames[DIR_RIGHT],
		g_dirFrames[DIR_UP], g_dirFrames[DIR_DOWN]);
	for (int d = 0; d < DIR_COUNT; d++) {
		if (g_dirFrames[d] < 10) {
			Log("     %-5s : not enough samples, hold the stick that way for a second",
				kDirNames[d]);
			continue;
		}
		char storage[256];
		LineBuf line(storage);
		line.Add("     %-5s ->", kDirNames[d]);
		int shown = 0;
		for (int i = 0; i < g_numActions; i++) {
			// Present for most of this direction's frames, and clearly less
			// present for the opposite one.
			int opp = (d == DIR_LEFT) ? DIR_RIGHT : (d == DIR_RIGHT) ? DIR_LEFT
			        : (d == DIR_UP)   ? DIR_DOWN  : DIR_UP;
			float here = (float)g_dirHits[i][d] / (float)g_dirFrames[d];
			float there = g_dirFrames[opp]
				? (float)g_dirHits[i][opp] / (float)g_dirFrames[opp] : 0.0f;
			if (here < 0.6f || there > 0.25f) continue;
			if (shown == 8) { line.Add(" ..."); break; }
			line.Add(" [%d](avg %02X)", i,
			         g_dirHits[i][d] ? g_dirSum[i][d] / g_dirHits[i][d] : 0);
			shown++;
		}
		if (!shown) line.Add(" nothing exclusive to this direction");
		Log("%s", line.c_str());
	}
	Log("=== end probe report ===");
}

