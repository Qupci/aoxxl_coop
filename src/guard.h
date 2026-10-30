// Romans guard a downed hero (individual heroes): the Romans near a hero as it
// falls stay round it until it is revived or they are beaten -- the partner
// fights them to get to it.

#pragma once

bool GuardInstallHooks();
// Once a frame (coop's frame).
void GuardFrame();
// After squad `sq`'s tick: its Romans guarding a downed hero stay round it.
void GuardSteer(char* sq);
// Has squad `sq` Romans guarding a downed hero (not to be put away meanwhile)?
bool GuardKeepsSquad(char* sq);
