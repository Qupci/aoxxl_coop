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
// They are in XXL-Editor's moddable format: the original PC levels keep their
// class header and some values in the game module, which is patched in memory
// (the editor's game patcher's patches) to read them from the level file -- so
// every level the game loads is served rebuilt (most just converted).
#pragma once

// From DllMain, after the config: patches the module, hooks the file opens,
// starts building what the cache lacks. Nothing with squad_romans_at_once <= 1
// or a game module the conversion does not know.
void LevelForgeInstall();
// The level loaded has its Roman pools filled (Romans were added to it).
bool LevelForgePoolsFilled(int level);
