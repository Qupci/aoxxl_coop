// Works out which XInput pad is the one the game itself reads -- player 1's.
//
// The game reads one controller through DirectInput, and XInput slot numbers
// follow connection order, so "player 1 is slot 0" is a guess that is wrong on
// plenty of machines. Instead, every pad event (a button press or a sharp stick
// movement) is checked against the game's own action array: if the same physical
// pad drives the game, the game's input shows a matching event within a few
// frames, every time. Player 2's pad never does, except by coincidence.

#pragma once
#include <cstdint>

enum PadRole {
	PAD_ROLE_UNKNOWN = 0,
	PAD_ROLE_GAME    = 1,   // drives the game's own input: player 1's pad
	PAD_ROLE_FREE    = 2,   // the game does not read it: usable by player 2
};

// Once per frame, after PadsPoll(), with the game's freshly updated action array.
void PadIdentFrame(const uint8_t* gameValues, int numActions);

PadRole PadIdentRole(int pad);
const char* PadRoleName(PadRole r);

// True if the game's input changed within `frames` frames of now (button-type
// actions only), i.e. a pad pressed right now may well be the game's pad.
bool PadIdentGameEventRecently(int frames);

// Forces a role (ini override p1_pad).
void PadIdentForce(int pad, PadRole role);
