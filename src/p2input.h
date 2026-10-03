// Synthesises player 2's CKInput action array from an XInput pad.
//
// The engine funnels all mapped input into one byte-per-action array
// (docs/RESEARCH.md 6b). Player 2 does not go through the game's binding table
// at all -- instead the array is rebuilt from pad 2 and swapped in for the
// duration of that hero's state tick, then swapped back out. So player 1 keeps
// whatever the in-game control menu is configured to do, including mixed
// keyboard/controller presets, and the two never interfere.
//
// The index mapping below was measured, not guessed: button rows come from
// aggregating isolated pad edges over a 10k-frame session, axis rows from
// pairing stick samples against live action values across 21 samples.

#pragma once
#include "pads.h"
#include <cstdint>

// Fill `out[0..numActions)` with the action bytes pad `padIndex` should produce.
// Returns false if the pad is not connected, in which case `out` is untouched.
bool BuildP2Actions(int padIndex, uint8_t* out, int numActions);
