// Both players in one scripted mode: the machine gun, the boat, Obelix's belly
// slide, Twister Fusion -- and either player clicking through a dialogue.
//
// Every control the game reads goes through InputView_GetAction (0x4358B0,
// thiscall(view, action), ret 4): value = CKInput.values[view.remap[action]],
// view = one entry (0x10 bytes) of a named input map (CKInput +0x40, names
// hashed at +0x5C, count +0x38): "Astérix Trio" (the heroes), "Astérix
// Baliste" (the machine gun), "Astérix Bateau" (the boat), "Camera",
// "Sekensor" (dialogue text). While a shared mode runs, each (map, action) is
// answered from the pad of the player who owns that part of the mode.

#pragma once
#include <cstdint>

bool SharedInstallHooks();
// Once per frame, right after CoopFrame (no input swap active).
void SharedFrame();

// Which shared mode is on (for hints and the log).
enum SharedMode { SHARED_NONE, SHARED_GUN, SHARED_GUN_SOLO, SHARED_BOAT, SHARED_SLIDE, SHARED_FUSION, SHARED_GUN_OBELIX };
SharedMode SharedCurrentMode();
unsigned SharedModeAgeMs();   // how long the current mode has been on
const char* SharedModeName(SharedMode m);
// Mode `m`'s who-does-what hint: Asterix's part in `ta`, Obelix's in `to`
// (kModeHintLen characters each; empty: none).
const int kModeHintLen = 160;
void SharedModeHintText(SharedMode m, wchar_t* ta, wchar_t* to);
