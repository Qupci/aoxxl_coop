// The level's pathfinding graph (CKSrvPathFinding, yellow pages +0x64), read
// to tell whether one hero can walk to the other: the AI sidekick's grid of
// cells per node, the transitions between nodes (walking over, or a jump from
// one box to another) as the level has them switched on now.

#pragma once
#include "kal.h"

enum PfReach { PF_UNKNOWN = -1, PF_NO = 0, PF_YES = 1 };

// Can a hero walk from `from` to `to`? PF_UNKNOWN when either place is in no
// node of the graph (or the level has none).
int PfReachable(const Vector3& from, const Vector3& to);

// A random place a hero at `from` can walk to, `rmin`..`rmax` units away
// (x / z; y is `from`'s). False if `from` is in no node or none is that far.
bool PfSpotNear(const Vector3& from, float rmin, float rmax, Vector3* out);

// The graph in the log: nodes, transitions, doors; with `around`, the cells
// within `radius` units of it as a map.
void PfDump(const Vector3* around, float radius);
