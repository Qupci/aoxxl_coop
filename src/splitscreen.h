// Split-screen rendering (docs/RESEARCH.md, "Rendering").
//
// The per-frame render is CKGraphic vtable slot 17 (0x431A40): it bumps the frame
// id and runs FUN_00413EC0 on the render view (CKGraphic+4). That function syncs
// the camera frames, walks the scene graph (CSGRootNode slot 14) to fill 15 draw
// lists -- culling every geometry against the main RwCamera's frustum -- clears,
// draws the 11 world lists (FUN_00413F40) and the effects list (Render_View),
// then the 3 HUD lists with the 2D camera (FUN_00414070), and presents.
//
// While two players are in co-op that function is replaced by one that runs the
// camera sync / scene walk / world / effects steps once per player -- each with
// that player's camera, a half-screen view window and a half-screen D3D viewport
// -- and then draws the HUD once over the whole screen. Any other time the
// game's own function runs untouched.

#pragma once

bool SplitInstallHooks();

// For the watchdog / autotest.
bool SplitActive();
long SplitFrames();

// The view window a player's half was last drawn with (player 0 = player 1's
// half), false while the split screen is down.
bool SplitHalfWindow(int player, float out[2]);
