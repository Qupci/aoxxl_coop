// Scripted test runs, so the mod can be exercised without a person at the pads.
//
// Enabled only when the game is started with the environment variable
// XXL_AUTOTEST=<scenario> (coop/tools/run_test.ps1 does that), so a normal launch
// can never trigger it. A scenario waits for gameplay, then plays a timeline:
// player 1 input is written into CKInput's values right after its update, player
// 2 drives a virtual XInput pad, both heroes' state is logged at 10 Hz,
// screenshots of the game window go to Game Files\shots, and the game exits when
// the timeline ends.

#pragma once
#include "trio.h"

void AutotestInit();      // DllMain: reads XXL_AUTOTEST
bool AutotestActive();
void AutotestFrame();     // game thread, right after CKInput::update, before CoopFrame
void AutotestBeforeLifeTick(int heroIndex, Hero hero);
void AutotestAfterLifeTick(int heroIndex, Hero hero);

// Window capture (shot.cpp). `scaleDiv` 1 = full size, 2 = half.
bool CaptureWindowPng(void* hwnd, const char* path, int scaleDiv);
