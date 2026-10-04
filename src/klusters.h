// Beacon klusters (crates, placed helmets...) kept alive for both players'
// cameras while the split screen is up. See klusters.cpp.

#pragma once

bool KlusterInstallHooks();
void KlusterSetP2Camera();   // once per frame, after player 2's camera moved

// Diagnostics.
long KlusterQueries();       // queries answered with both frustums
long KlusterP2Only();        // klusters kept only because player 2 sees them
