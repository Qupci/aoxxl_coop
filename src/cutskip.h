// A cutscene is skipped by holding the skip button (pad A, Enter) for 3 s.
//
// The game skips a playing scene with flag 0x1000 the moment its cinematic
// service (CKSrvCinematic::vf17, 0x46AC10) reads its input view's action 0 as
// pressed (FUN_00461A80). That read (InputView_GetAction, hooked in shared.cpp)
// is answered 0 until the button has been held 3 s; a press shows "Hold [A] to
// skip" in the presser's hint box, its plate filling as the hold goes on (the
// hold bar of hints.cpp), gone 5 s after the last press. Level 0's films (no
// in-game HUD for the box) still skip at once.

#pragma once
#include <cstdint>

// Once per frame (after the co-op frame).
void CutSkipFrame();
// The cinematic service's read of its skip action, `v` the game's value (a
// press's edge): what it gets instead.
uint8_t CutSkipRead(uint8_t v);
// The read comes from the cinematic service's tick.
inline bool CutSkipReader(uintptr_t ret) { return ret >= 0x46AC10 && ret < 0x46AD3F; }
