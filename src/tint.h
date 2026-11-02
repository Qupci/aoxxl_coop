// Second chance's two health rows (hud2.cpp): each hero's shields in its own
// colours -- Asterix's red, Obelix's blue. See tint.cpp.

#pragma once

// From now until TintEnd, the shields' textures bound are drawn in hero
// `hero`'s colours. `learn`: every texture bound is a shield's (a pass that
// draws only shields and their chips); otherwise only those learned are swapped.
void TintBegin(int hero, bool learn);
void TintEnd();
// A new level's interface: the copies released, nothing known.
void TintForget();
