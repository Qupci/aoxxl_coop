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
// May Roman `roman` of pool record `rec` guard (a basic Roman serving the empire)?
bool GuardEligible(const char* rec, char* roman);
// The names of the Roman's model's textures, " name" each, in `out` (diagnostics).
void GuardModelTextures(char* roman, char* out, int room);
