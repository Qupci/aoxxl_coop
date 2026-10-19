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
	// The mod's own combos (Back or the right stick button held, plus A, B, X,
	// Y or D-pad up/down): those do not reach the hero while the modifier is held.
	PadState p = pad;
	const uint16_t kFace = PAD_A | PAD_B | PAD_X | PAD_Y | PAD_DPAD_UP | PAD_DPAD_DOWN;
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

// Combos. CKInput::Update ends with the combo recognizer (FUN_004353C0,
// thiscall on CKInput +0x34, args: value array, bindings; ret 8): every combo
// entry (+8, 0x14 bytes: +4 on, +5 steps, +6 current step, +8 steps, +0xC time
// allowed, +0x10 deadline) walks its steps (0x10 bytes: +4 items, +5 forbidden,
// +8 items / +0xC forbidden of 0x10 bytes: +0 binding, +1 threshold, +4 hold
// time, +0xC latched) over the bindings' values and hold times, and writes 0xFF
// into value numActions + k when combo k completes. The heroes' view reads those
// as actions 0x10.. (Club A A X B, Power Hammer A B X, Mole A A Y B, Twister A X
// B Y). Player 2's values never went through it. It runs again here on player
// 2's values with a private copy of its state (steps and items carry state too).
namespace {
struct ComboCopy {
	char*  src = nullptr;          // the game's manager it was copied from
	char*  srcEntries = nullptr;
	char   mgr[0x0C] = {};
	char*  entries = nullptr;
	unsigned count = 0;
	char*  pool = nullptr;         // steps and items, one allocation
} g_combo;

void FreeComboCopy() {
	delete[] g_combo.entries;
	delete[] g_combo.pool;
	g_combo = ComboCopy();
}

bool CopyCombos(char* src) {
	FreeComboCopy();
	unsigned n = *(uint8_t*)(src + 5);
	char* se = *(char**)(src + 8);
	if (!se || n == 0 || n > 32) return false;
	size_t poolSize = 0;
	for (unsigned c = 0; c < n; c++) {
		char* e = se + c * 0x14;
		unsigned ns = *(uint8_t*)(e + 5);
		poolSize += ns * 0x10;
		for (unsigned s = 0; s < ns; s++) {
			char* st = *(char**)(e + 8) + s * 0x10;
			poolSize += (*(uint8_t*)(st + 4) + *(uint8_t*)(st + 5)) * 0x10;
		}
	}
	g_combo.entries = new char[n * 0x14];
	g_combo.pool = new char[poolSize + 16];
	memcpy(g_combo.entries, se, n * 0x14);
	char* p = g_combo.pool;
	for (unsigned c = 0; c < n; c++) {
		char* e = g_combo.entries + c * 0x14;
		unsigned ns = *(uint8_t*)(e + 5);
		char* steps = p;
		memcpy(steps, *(char**)(e + 8), ns * 0x10);
		p += ns * 0x10;
		*(char**)(e + 8) = steps;
		*(uint8_t*)(e + 6) = 0;
		*(float*)(e + 0x10) = 0.0f;
		for (unsigned s = 0; s < ns; s++) {
			char* st = steps + s * 0x10;
			for (int list = 0; list < 2; list++) {
				unsigned cnt = *(uint8_t*)(st + 4 + list);
				char*& items = *(char**)(st + 8 + 4 * list);
				if (!items || !cnt) continue;
				memcpy(p, items, cnt * 0x10);
				items = p;
				for (unsigned i = 0; i < cnt; i++) p[i * 0x10 + 0xC] = 0;
				p += cnt * 0x10;
			}
		}
	}
	memcpy(g_combo.mgr, src, sizeof(g_combo.mgr));
	*(char**)(g_combo.mgr + 8) = g_combo.entries;
	g_combo.src = src;
	g_combo.srcEntries = se;
	g_combo.count = n;
	return true;
}
} // namespace

int P2RecognizeCombos(uint8_t* values, int numActions, char* bindings) {
	CKYellowPages* yp = YellowPages();
	char* input = yp ? *(char**)((char*)yp + 0x14) : nullptr;
	char* src = input ? *(char**)(input + 0x34) : nullptr;
	if (!src || !values || !bindings) return 0;
	if (src != g_combo.src || *(char**)(src + 8) != g_combo.srcEntries || *(uint8_t*)(src + 5) != g_combo.count) {
		if (!CopyCombos(src)) return 0;
	}
	if (numActions + (int)g_combo.count > 256) return 0;
	typedef int(__fastcall* RecognizeFn)(void* mgr, void* edx, uint8_t* values, char* bindings);
	((RecognizeFn)0x4353C0)(g_combo.mgr, nullptr, values, bindings);
	return (int)g_combo.count;
}

// Cheat: every combo learned (the merchant's item bits the heroes' combo
// actions ask for, game manager +0x40 -> +0x10: 1 Club, 2 Power Hammer, 4
// Twister, 8 Twister Fusion, 0x10 Mole) and the combo bar full, as
// FUN_0048C860 fills it (that object +0x10 bit 0x4000; +0xA4 = +0xA0, +0xC4 =
// byte +0xBB).
// The HUD's bar only turns "full" (FUN_00518190 state 2 -> 4, "Combo") when it
// sees the hits (+0xB0) go past the full mark (+0xC8) -- set straight to full,
// the stats were full while the bar went on as before (the HUD looked frozen).
// viaHits: the hits are put one short of the mark instead, and the caller has
// the hero land two hits a moment later (CoopFrame): the game fills the bar.
// Returns true when those hits are wanted.
bool CombosCheat(bool viaHits) {
	CKYellowPages* yp = YellowPages();
	char* gm = yp ? (char*)yp->gameManager : nullptr;
	char* sg = gm ? *(char**)(gm + 0x40) : nullptr;
	if (!sg) return false;
	*(uint32_t*)(sg + 0x10) |= 0x1F;
	if (viaHits) {
		if (*(uint32_t*)(sg + 0x10) & 0x4000) {   // already full: the gauge refilled
			*(uint32_t*)(gm + 0xA4) = *(uint32_t*)(gm + 0xA0);
			return false;
		}
		uint32_t mark = *(uint8_t*)(gm + 0xC8);
		if (mark && *(uint32_t*)(gm + 0xB0) + 1 < mark) *(uint32_t*)(gm + 0xB0) = mark - 1;
		*(float*)(gm + 0xA8) = *(float*)(gm + 0x9C);   // the decay timer from the start
		return true;
	}
	if (!(*(uint32_t*)(sg + 0x10) & 0x4000)) {
		*(uint32_t*)(gm + 0xA4) = *(uint32_t*)(gm + 0xA0);
		*(uint32_t*)(gm + 0xC4) = *(uint8_t*)(gm + 0xBB);
		*(uint32_t*)(sg + 0x10) |= 0x4000;
	}
	return false;
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
