// Co-op difficulty ([coop_difficulty]) for the enemies and the heroes' health
// bar: a Roman's wind-up before its blow (roman_charge_speed), a shield for
// every Roman (roman_shield: breakable / infinite), hits to defeat an enemy
// (roman_health), blows a shield takes (roman_shield_hits), the weaponless
// daze after a Power Hammer / club / Idefix (roman_disarmed_time), and the
// shields of the heroes' health bar (hero_shields + hero_extra_shields). All
// live; the enemies' while the squads' rules are on, the heroes' in co-op
// (single player too with difficulty_coop_only off).

#pragma once

bool ToughnessInstallHooks();
// Once a frame (after the squads' rules): the enemy components and the health bar.
void ToughnessFrame();
// Shields a hero may have at most now (6: the game's).
int ToughnessHeroShieldsMax();
