// Beacon klusters (crates, placed helmets...) kept alive for both players'
// cameras while the split screen is up. See klusters.cpp.

#pragma once

bool KlusterInstallHooks();
void KlusterSetP2Camera();   // once per frame, after player 2's camera moved
// Is the sphere in a view the game camera does not cover (player 2's, or player
// 1's own while player 2's hero leads)? Same test as the camera service's planes.
bool KlusterSphereInOtherViews(const float* c, float r);
// In any player's view (the game camera's too) within maxDist of that camera?
bool KlusterSphereInAnyView(const float* c, float r, float maxDist);
bool KlusterSplitUp();   // two views are being culled for

// Diagnostics.
long KlusterQueries();       // queries answered with both frustums
long KlusterP2Only();        // klusters kept only because player 2 sees them
