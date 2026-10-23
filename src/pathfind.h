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
// Can a hero walk from either place to the other (a drop one way is enough)?
// One read of the graph for both ways.
int PfConnected(const Vector3& a, const Vector3& b);

// Autotests: the walkable cell nearest `from` (at least `minDist` units off,
// at most `maxDist`) that a hero at `from` can't walk to nor come from; its
// centre on top of its node's box in `out`.
bool PfSpotCutOff(const Vector3& from, float minDist, float maxDist, Vector3* out);

// The graph in the log: nodes, transitions, doors; with `around`, the cells
// within `radius` units of it as a map.
void PfDump(const Vector3* around, float radius);
