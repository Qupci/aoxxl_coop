#pragma once
#include <cstdint>

// Minimal XInput wrapper, loaded at runtime so the mod does not add a hard
// dependency on a particular xinput DLL version (the game itself uses
// DirectInput 8 and links none of these).

enum {
	PAD_DPAD_UP    = 0x0001,
	PAD_DPAD_DOWN  = 0x0002,
	PAD_DPAD_LEFT  = 0x0004,
	PAD_DPAD_RIGHT = 0x0008,
	PAD_START      = 0x0010,
	PAD_BACK       = 0x0020,
	PAD_LTHUMB     = 0x0040,
	PAD_RTHUMB     = 0x0080,
	PAD_LB         = 0x0100,
	PAD_RB         = 0x0200,
	PAD_A          = 0x1000,
	PAD_B          = 0x2000,
	PAD_X          = 0x4000,
	PAD_Y          = 0x8000,
};

struct PadState {
	bool     connected = false;
	uint16_t buttons   = 0;   // current frame
	uint16_t pressed   = 0;   // rising edge since last poll
	uint16_t released  = 0;
	float    lx = 0, ly = 0;  // left stick, deadzoned, -1..1
	float    rx = 0, ry = 0;  // right stick
	float    lt = 0, rt = 0;  // triggers, 0..1
};

const int kMaxPads = 4;

bool PadsInit();               // false if no XInput DLL could be loaded
void PadsShutdown();
void PadsPoll();               // call once per frame
const PadState& PadGet(int i); // i in [0, kMaxPads)

// Index of the lowest-numbered connected pad that just pressed `button`,
// skipping `excludeIndex` (pass -1 to skip nothing). Returns -1 if none.
int PadFindPressed(uint16_t button, int excludeIndex);

// Test hook for autotest: replaces XInput slot `i` with a scripted pad. Only
// `buttons` and the stick/trigger values of `st` are used; edges and the
// connected flag are derived by PadsPoll() exactly as for a real pad. Pass
// nullptr to hand the slot back to the real device.
void PadsSetVirtual(int i, const PadState* st);
