// Sound heard from both players' cameras while the split screen is up. See
// sound.cpp.

#pragma once

bool SoundInstallHooks();
void SoundSetP2Listener();   // once per frame (no-op / off while the split is down)

// Diagnostics.
long SoundNearP2();          // range checks answered by player 2's camera
long SoundMixedAsP2();       // voices mixed against player 2's camera
long SoundRegionKept();      // region voices kept playing by player 2's camera
long SoundRegionStarted();   // region sounds started for player 2's camera
void SoundDumpRegions();     // diagnostics: the waiting region sounds and their boxes
