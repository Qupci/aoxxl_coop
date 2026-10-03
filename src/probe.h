// Input probe: works out the layout of the shared hero command block by
// correlating byte changes in it against XInput button edges on a pad the
// player is holding.
//
// The command block at gameManager+0x40 is what both hero update functions read
// (docs/RESEARCH.md section 6). Four of its fields are known; the buttons are
// not. Rather than diff hex dumps by hand, this snapshots the block every frame
// and, on each button press/release, logs exactly which bytes moved on that
// frame -- which makes the mapping fall out in a couple of minutes of play.

#pragma once
#include <cstdint>

const size_t kProbeBytes = 0x200;  // dumped with SEH guards; true size unknown

void ProbeFrame();       // called once per frame, before anything else reads input
void ProbeReport();      // dump the accumulated correlation table

// True while the probe wants exclusive use of the pad (co-op join is suppressed
// so Start/Back presses show up as data instead of changing game state).
bool ProbeActive();
