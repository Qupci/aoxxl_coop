#include "p2input.h"
#include "inputmgr.h"

#include <cstring>

namespace {

// Analog half-axes. Each carries 0x00..0xFF magnitude; the engine keeps several
// parallel copies of the same logical input, and all of them have to agree.
struct AxisMap {
	int indices[3];   // -1 terminated
};
const AxisMap kAxisUp    = { {  0, 39, -1 } };
const AxisMap kAxisDown  = { {  1, 40, -1 } };
const AxisMap kAxisLeft  = { {  2, 36, 41 } };
const AxisMap kAxisRight = { {  3, 37, 42 } };

// Buttons. `held` stays set while the button is down (it is the index that
// clears on release); `edge` indices fire only on the press frame, matching the
// engine's own binding logic, which clears them again the next frame.
struct ButtonMap {
	uint16_t padBit;
	int      held;
	int      edge[2];   // -1 terminated
};
const ButtonMap kButtons[] = {
	{ PAD_A,     43, {  4, 44 } },
	{ PAD_X,     38, {  5, 45 } },
	{ PAD_B,     -1, {  6, -1 } },
	{ PAD_Y,     -1, {  7, -1 } },
	{ PAD_LB,    -1, {  8, -1 } },
	{ PAD_RB,    -1, { 10, -1 } },
	// Back and Start are deliberately not forwarded: they are player 2's
	// join/drop gesture and must not reach the game as pause/menu input.
};

void SetAxis(uint8_t* out, int n, const AxisMap& axis, float magnitude) {
	if (magnitude <= 0.0f) return;
	if (magnitude > 1.0f) magnitude = 1.0f;
	uint8_t v = (uint8_t)(magnitude * 255.0f + 0.5f);
	if (!v) return;
	for (int i = 0; i < 3 && axis.indices[i] >= 0; i++)
		if (axis.indices[i] < n) out[axis.indices[i]] = v;
}

} // namespace

bool BuildP2Actions(int padIndex, uint8_t* out, int numActions) {
	const PadState& p = PadGet(padIndex);
	if (!p.connected) return false;
	BuildActionsFromPad(p, out, numActions);
	return true;
}

void BuildActionsFromPad(const PadState& pad, uint8_t* out, int numActions) {
	memset(out, 0, numActions);
	// The mod's own combos (Back or the right stick button held, plus A, B, X
	// or Y): the face buttons do not reach the hero while the modifier is held.
	PadState p = pad;
	const uint16_t kFace = PAD_A | PAD_B | PAD_X | PAD_Y;
	if (p.buttons & (PAD_BACK | PAD_RTHUMB)) {
		p.buttons &= ~kFace;
		p.pressed &= ~kFace;
	}

	// Stick: split into four one-sided axes the way the engine stores them.
	SetAxis(out, numActions, kAxisUp,    p.ly);
	SetAxis(out, numActions, kAxisDown,  -p.ly);
	SetAxis(out, numActions, kAxisRight, p.lx);
	SetAxis(out, numActions, kAxisLeft,  -p.lx);

	for (const ButtonMap& b : kButtons) {
		bool down = (p.buttons & b.padBit) != 0;
		bool justPressed = (p.pressed & b.padBit) != 0;
		if (down && b.held >= 0 && b.held < numActions)
			out[b.held] = 0xFF;
		if (justPressed)
			for (int i = 0; i < 2 && b.edge[i] >= 0; i++)
				if (b.edge[i] < numActions) out[b.edge[i]] = 0xFF;
	}
}

// Which actions are physically held right now (level, for edge-triggered
// bindings too) -- what the engine's per-binding hold timers follow.
bool BuildP2Held(int padIndex, uint8_t* out, int numActions) {
	const PadState& p = PadGet(padIndex);
	if (!p.connected) return false;
	BuildActionsFromPad(p, out, numActions);   // stick axes
	for (const ButtonMap& b : kButtons) {
		if (!(p.buttons & b.padBit)) continue;
		if ((b.padBit & (PAD_A | PAD_B | PAD_X | PAD_Y)) && (p.buttons & (PAD_BACK | PAD_RTHUMB))) continue;
		if (b.held >= 0 && b.held < numActions) out[b.held] = 0xFF;
		for (int i = 0; i < 2; i++)
			if (b.edge[i] >= 0 && b.edge[i] < numActions) out[b.edge[i]] = 0xFF;
	}
	return true;
}
