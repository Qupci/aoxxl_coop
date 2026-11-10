// Roman pools sized for squad_romans_at_once ([coop_difficulty]).
//
// A squad takes its Romans from a pool (CKGrpPoolSquad: its member hooks are
// the free Romans, each a whole object -- node tree, collision tests, clone
// manager entries -- loaded from the level file), so no squad can have more
// Romans at once than its pool holds: LV4's fights share 18 basic Romans. The
// level files are rebuilt with more of them (romanforge\, XXL-Editor's level
// code): each pool of basic Romans and leaders filled up to squad_romans_at_once
// times what it holds. The originals stay as they are; the rebuilt ones are
// kept in Game Files\coop_levels\ (made once, again when the factor or the
// game's files change) and the game reads them instead (CreateFileA).
//
// Bonuses (helmets...) and crates are objects handed to the spots in view the
// same way (a bonus pool's chain, a crate group's hooks): two views see more
// spots than one, so the rebuilt levels have twice the level's, whatever the
// factor (romanforge 2; archers and jumping Romans cloned too, not the jetpack
// Romans).
//
// They are in XXL-Editor's moddable format: the original PC levels keep their
// class header and some values in the game module, which is patched in memory
// (the editor's game patcher's patches) to read them from the level file -- so
// every level the game loads is served rebuilt (most just converted).
#pragma once

// From DllMain, after the config: patches the module, hooks the file opens,
// starts building what the cache lacks. Nothing with a game module the
// conversion does not know (Romans cloned only with squad_romans_at_once > 1).
void LevelForgeInstall();
// The level loaded has its Roman pools filled (squad_romans_at_once > 1).
bool LevelForgePoolsFilled(int level);
