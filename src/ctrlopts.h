// The controls options (the options menu's Controls, page 3/3).
//
// Vibration: the game rumbles through DirectInput force feedback only (pad
// +0xC4, an effect object), which an XInput pad never has through DirectInput,
// so its Vibration row could not be chosen. The row is offered whenever an
// XInput pad is in, and the game's rumble calls (CKInput FUN_00437E60 on,
// FUN_00437EA0 timed, FUN_00437EE0 off) drive the XInput motors of the player
// whose hero they are for: a hero life's own calls that hero's player, the
// rest (the potion, machines) the leader's player.
//
// Invert Camera X / Y: two rows below Vibration, copies of its label and value
// put into the page while it is up (taken out again when the controls close).
// On: the camera's horizontal / vertical actions swapped (player 1's 48/49 and
// 46/47 after each input update; player 2's right stick and the mod's follow
// cameras turned round). Kept in xxl_coop.ini [controls].

#pragma once
#include <cstdint>

bool CtrlOptsInstallHooks();
// Once per frame, right after the game's input update: the camera actions
// swapped, the rumble's motors, the rows taken out when the controls are gone.
void CtrlOptsFrame();
// -1 when that camera axis is inverted, else 1 (the mod's own cameras, player 2's stick).
// The localised string `id` as the controls show it: the page counter's
// ("Page %CtrlPage_Cur%/%CtrlPage_Nb%", 0x390) with 4 pages while the mod's rows are in.
const wchar_t* CtrlOptsLocString(uint32_t id, const wchar_t* s);
float CtrlOptsCamX();
float CtrlOptsCamY();
