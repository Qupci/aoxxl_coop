// Split screen: each player's own copy of the game's camera.
//
// The camera service (CKSrvCamera, yellow pages +0x68, 0x6A0 bytes) keeps all its
// state in itself and in its "tracks" (the follow camera, fixed cameras, path
// cameras... a list at +0x20) and follows one target (+0xE8, a hero's +0x10).
// Camera sectors (list at +0x24: a box, a track) switch the track when the
// target is inside the box -- the level's camera triggers. The game's own state
// is the leader's camera. The other player's camera is a copy of those bytes:
// once a frame, right after the game's tick, the copy is swapped in, the game's
// own tick runs again on it (aimed at the other player's hero, reading that
// player's right stick), the result is kept, and the game's state goes back.
// When the game hands its camera to the other hero (a leader switch), the two
// states change places, so each player keeps their own camera.

#pragma once

bool GameCamInstallHooks();
// The copy's last view for `hero` (the hero that is not leading): the camera
// frame's modelling matrix and the view window the game set for it.
bool GameCamView(int hero, float m[16], float window[2]);
// The hero the game's own camera follows (the real leader, also while the mod
// has the trio's leader byte sliced for the other hero's tick); -1 if the
// copies are off.
int GameCamLiveHero();
// The copy's clip and fog planes for `hero` (near, far, fog), as its tick left them.
bool GameCamPlanes(int hero, float planes[3]);
// Around a hero's Life tick: hero code writes its camera wishes into the camera
// service (in the air: don't follow the jump, +0xC bits 11/12...). For the hero
// with the copy, the copy is the service for the length of its tick.
void GameCamBeginHeroTick(int hero);
void GameCamEndHeroTick();
// The copy's service bytes (camera position +0x5B0, look-at +0x5BC), or nullptr.
const char* GameCamCopyState(int hero);
