#include "padident.h"
#include "log.h"
#include "pads.h"

#include <cmath>
#include <cstring>

namespace {

// The same physical pad reaches the game's DirectInput read and our XInput read
// within the same frame (both happen in CKInput::update's frame, microseconds
// apart), so the match window is tight. A loose window would let a busy player 1
// "match" player 2's presses by coincidence and drop player 2 mid-fight.
const int kWindow       = 1;     // frames either side that count as "the same moment"
const int kMaxPending   = 16;
const int kDecayFrames  = 1800;  // ~30 s: halve the evidence so roles can change
const int kRing         = 32;

// Action indices that carry the analog stick (measured, docs/RESEARCH.md 6b).
// They change continuously while a stick is held, so 0 -> nonzero transitions on
// them are not discrete events; the stick is compared as a vector instead.
bool IsStickIndex(int i) {
	return (i >= 0 && i <= 3) || i == 36 || i == 37 || (i >= 39 && i <= 42);
}

enum EventKind { EV_BUTTON, EV_STICK };

struct PendingEvent {
	uint32_t  frame;
	EventKind kind;
	float     dx, dy;   // EV_STICK: unit direction after the change
};

struct GameEvent {
	uint32_t  frame = 0;
	EventKind kind = EV_BUTTON;
	float     dx = 0, dy = 0;
};

struct PadTrack {
	float        prevLx = 0, prevLy = 0;
	PendingEvent pending[kMaxPending] = {};
	int          pendingCount = 0;
	int          matched = 0, unmatched = 0;
	PadRole      role = PAD_ROLE_UNKNOWN;
	bool         forced = false;
};

PadTrack  g_pad[kMaxPads];
uint32_t  g_frame = 0;
GameEvent g_events[kRing];
int       g_eventHead = 0;
uint32_t  g_lastGameButtonEvent = 0;
uint8_t   g_prevValues[256] = {};
float     g_prevGx = 0, g_prevGy = 0;

void PushGameEvent(EventKind k, float dx, float dy) {
	GameEvent& e = g_events[g_eventHead];
	e.frame = g_frame; e.kind = k; e.dx = dx; e.dy = dy;
	g_eventHead = (g_eventHead + 1) % kRing;
}

// A button press matches any discrete game event at the same moment; a stick
// movement matches only a game stick movement in (nearly) the same direction,
// since two people rarely push the same way in the same frame.
bool Matches(const PendingEvent& pe) {
	for (const GameEvent& e : g_events) {
		if (!e.frame || e.frame + kWindow < pe.frame || e.frame > pe.frame + kWindow) continue;
		if (pe.kind != e.kind) continue;
		if (pe.kind == EV_BUTTON) return true;
		if (pe.dx * e.dx + pe.dy * e.dy > 0.8f) return true;
	}
	return false;
}

void SetRole(int i, PadRole r, const char* why) {
	PadTrack& t = g_pad[i];
	if (t.role == r) return;
	t.role = r;
	Log("pads: pad %d is %s (%s; matched %d, unmatched %d)", i,
		r == PAD_ROLE_GAME ? "player 1's -- the game reads it" : "free for player 2",
		why, t.matched, t.unmatched);
}

} // namespace

const char* PadRoleName(PadRole r) {
	switch (r) {
		case PAD_ROLE_GAME: return "game";
		case PAD_ROLE_FREE: return "free";
		default:            return "unknown";
	}
}

PadRole PadIdentRole(int pad) {
	if (pad < 0 || pad >= kMaxPads) return PAD_ROLE_UNKNOWN;
	return g_pad[pad].role;
}

void PadIdentForce(int pad, PadRole role) {
	if (pad < 0 || pad >= kMaxPads) return;
	g_pad[pad].forced = true;
	g_pad[pad].role = role;
	Log("pads: pad %d forced to role '%s' by xxl_coop.ini", pad, PadRoleName(role));
}

bool PadIdentGameEventRecently(int frames) {
	return g_lastGameButtonEvent && g_frame - g_lastGameButtonEvent <= (uint32_t)frames;
}

void PadIdentFrame(const uint8_t* v, int n) {
	g_frame++;
	if (n > (int)sizeof(g_prevValues)) n = (int)sizeof(g_prevValues);

	// --- events in the game's own input --------------------------------------
	if (v && n > 3) {
		bool button = false;
		for (int i = 0; i < n; i++) {
			if (IsStickIndex(i)) continue;
			if (!g_prevValues[i] && v[i]) button = true;
		}
		if (button) { PushGameEvent(EV_BUTTON, 0, 0); g_lastGameButtonEvent = g_frame; }

		float gx = (v[3] - v[2]) / 255.0f, gy = (v[0] - v[1]) / 255.0f;
		float mag = sqrtf(gx * gx + gy * gy);
		if (fabsf(gx - g_prevGx) + fabsf(gy - g_prevGy) > 0.2f && mag > 0.3f)
			PushGameEvent(EV_STICK, gx / mag, gy / mag);
		g_prevGx = gx; g_prevGy = gy;
		memcpy(g_prevValues, v, n);
	}

	// --- events on each XInput pad --------------------------------------------
	// Only inputs every control preset binds: the left stick and the face and
	// shoulder buttons. Start/Back/d-pad/right stick may be unbound, and an
	// unbound input on the game's own pad would look like "not the game's pad".
	const uint16_t kBound = PAD_A | PAD_B | PAD_X | PAD_Y | PAD_LB | PAD_RB;
	for (int i = 0; i < kMaxPads; i++) {
		PadTrack& t = g_pad[i];
		const PadState& p = PadGet(i);
		if (!p.connected) { t.pendingCount = 0; continue; }

		if ((p.pressed & kBound) && t.pendingCount < kMaxPending)
			t.pending[t.pendingCount++] = { g_frame, EV_BUTTON, 0, 0 };
		float mag = sqrtf(p.lx * p.lx + p.ly * p.ly);
		if (fabsf(p.lx - t.prevLx) + fabsf(p.ly - t.prevLy) > 0.25f && mag > 0.5f &&
		    t.pendingCount < kMaxPending)
			t.pending[t.pendingCount++] = { g_frame, EV_STICK, p.lx / mag, p.ly / mag };
		t.prevLx = p.lx; t.prevLy = p.ly;

		// Settle events old enough that the matching game event, if any, has had
		// time to show up.
		int keep = 0;
		for (int k = 0; k < t.pendingCount; k++) {
			const PendingEvent& pe = t.pending[k];
			if (g_frame < pe.frame + kWindow) { t.pending[keep++] = pe; continue; }
			if (Matches(pe)) t.matched++; else t.unmatched++;
		}
		t.pendingCount = keep;

		if (g_frame % kDecayFrames == 0) { t.matched /= 2; t.unmatched /= 2; }
		if (t.forced) continue;

		// The game's pad matches essentially every time; another person's pad
		// only lines up with player 1's input now and then.
		if (t.matched >= 10 && t.matched >= 6 * t.unmatched)
			SetRole(i, PAD_ROLE_GAME, "its input shows up in the game's input");
		else if (t.unmatched >= 6 && t.unmatched >= 2 * t.matched)
			SetRole(i, PAD_ROLE_FREE, "the game does not react to it");
	}
}
