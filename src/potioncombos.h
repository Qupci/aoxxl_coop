// Combos under the magic potion ([coop] potion_combos): the Power Hammer's
// shockwave and the Club's six small ones defeat the Romans they reach (as a
// catapult's stone does) instead of disarming them, the Mole takes three times
// as many Romans, and the Rage's Idefix defeats the Romans he bites instead of
// disarming them. The Twister and the Twister Fusion stay the game's. Co-op
// (single player too with [single_player] sp_isolation = off), live.

#pragma once

bool PotionCombosInstallHooks();
// Once a frame: the Mole's cap on Asterix.
void PotionCombosFrame();
