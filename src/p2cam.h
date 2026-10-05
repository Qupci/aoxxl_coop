// Player 2's own camera for split-screen.
//
// The game's camera service (CKSrvCamera::vf17, 11 KB) follows the trio leader and
// keeps per-zone "track" objects with their own state, so running it a second
// time for player 2 would mean double-buffering a lot of hidden state. Instead
// player 2 gets a follow camera of the same kind: it starts with the game
// camera's distance, angle and focus height, orbits player 2's hero on pad 2's
// right stick, damps vertical motion (jumps don't bob the view), and keeps out
// of walls with the engine's own collision test (CKSrvCollision, 0x44A210, the
// cylinder test the game camera uses). Player 2's movement basis is taken from
// this camera, so "forward" is always away from player 2's view.

#pragma once

void P2CamFrame();                 // once per frame (CoopFrame)
void P2CamReset();                 // next frame re-seeds from the game camera
bool P2CamMatrix(float out[16]);   // RwMatrix for player 2's pass; false if none yet
// The view the game camera is not covering: player 2's camera, or player 1's own
// camera while the game camera follows player 2's leading hero.
bool OtherViewMatrix(float out[16]);

// Around player 2's hero Life tick: swap the trio's movement basis for one
// built from player 2's camera (and back).
void P2CamBeginHeroTick();
void P2CamEndHeroTick();

// Player 1's follow camera: only while player 2's hero is the trio's leader (the
// game camera follows it).
bool P1CamMatrix(float out[16]);
int  Player1Pad();                 // the pad the game reads (pad identification), -1 if unknown
void P1CamBeginHeroTick();
void P1CamEndHeroTick();
