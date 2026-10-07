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
// Same, but every action whose button is down reads as held (no edges).
bool BuildP2Held(int padIndex, uint8_t* out, int numActions);

// Same mapping from an explicit pad state (used by autotest to synthesise
// player 1's input). Clears `out` first.
void BuildActionsFromPad(const PadState& p, uint8_t* out, int numActions);

// Runs the game's combo recognizer on player 2's action values (numActions
// bytes, then one byte per combo that must persist from frame to frame) with
// `bindings` carrying player 2's hold times; returns the number of combos.
int P2RecognizeCombos(uint8_t* values, int numActions, char* bindings);
// Cheat: all combos learned, combo bar full.
void CombosCheat();
