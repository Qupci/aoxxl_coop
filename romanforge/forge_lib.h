// romanforge: XXL1 PC levels with more Romans in their squads' pools, built
// with XXL-Editor's level code (vendored in xec\, MIT, AdrienTD). Used by the
// co-op mod (src/levelforge.cpp) inside the game process.
//
// The original PC levels keep their class header and some values ("DRM") in
// the game module; a level is first converted to the moddable format XXL-Editor
// writes (the game module patched to read it: ForgeModulePatches), then each
// Roman pool of basic Romans and their leaders is filled up to `factor` times
// the Romans it holds, by cloning one of them (node tree, collision tests,
// clone manager entries -- the editor's duplicator).
#pragma once
#include <cstddef>

struct ForgePatch { size_t offset; const unsigned char* bytes; size_t size; };

// Module patches as file offsets (mapped at 0x400000 + offset); returns the count.
size_t ForgeModulePatches(ForgePatch* out, size_t max);

// Checks the game module is the build the conversion knows (the Ipatix patch).
bool ForgeModuleKnown(const void* exe, size_t exeSize);

// Builds level `lvl` into outDir (LVLnnn\LVLnn.KWN, and the sectors' STRnn_mm.KWN
// when Romans were added): converted, Romans cloned with factor > 1. Writes a
// short report to `report`. *cloned: Romans added. false: nothing usable written.
bool ForgeLevel(const void* exe, size_t exeSize, const char* gameDir, const char* outDir, int lvl,
                float factor, int* cloned, char* report, size_t reportSize);
