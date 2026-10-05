// Every interactive object sees the player hero nearest to it as "the hero".
//
// The game was written for one player: enemies, squads, bonuses (helmets),
// crates, bumpers (springs), push/pull objects, pressure stones, cable cars...
// all ask the trio for its leader (gameManager+0x14 -> CKGrpTrio vtable
// +0x64..+0x94, or trio+0x2C directly) and get player 1's hero. Player 2's hero
// was invisible to them: Romans would not attack him, helmets would not come to
// him, iron crates did not know Obelix hit them.
//
// Every hook and group is ticked by CKBundle vtable slot 17 (0x43DFE0): the
// bundle's group Life, then each active hook Life. That loop is re-implemented
// so that, while two players are in, the trio's leader is set to the player hero
// nearest the object for the length of that object's tick, then put back --
// unless the object itself changed it (a blue-circle switch).

#pragma once

bool ProximityInstallHooks();

// The hero a lever's (CKHkActivator) events switch the trio to, or -1.
int ProximityLeverSwitchTarget(void* lever);

// Diagnostics.
long ProximitySlices();
long ProximityP2Bonuses();   // bonuses sent flying to player 2's hero
long ProximityP2Contacts();  // collision contacts delivered with player 2's hero as leader
long ProximityP2Beacons();   // placed bonuses (beacons) sent flying to player 2's hero
long ProximitySwappedZones();   // zone events fired for the other hero (co-op second pass)
long ProximityP2Squads();       // Roman squad ticks run for the non-leading player
