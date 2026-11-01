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

// The shortest walk from `from` to `to` in units (as PfWayTo / PfWayFrom, one
// search that stops there): -1 no way, -2 either place in no node.
float PfWalk(const Vector3& from, const Vector3& to);

// The lengths of the walks to `to` (units, cell centre to cell centre: eight
// ways in a node's grid, over to the next node, the sidekick's jumps), for
// PfWayFrom in the same frame. False: no graph, or `to` in no node.
bool PfWayTo(const Vector3& to);
// The shortest walk from `from` to the last PfWayTo's place, in units: -1 no
// way there, -2 `from` in no node (or no PfWayTo this second).
float PfWayFrom(const Vector3& from);

// Does the straight line from `from` to `to` stay within the graph's node boxes
// (x / z, y within 1 unit)? An enemy stepping out of them is put back where it
// was (its tick, 0x4CEE00). PF_UNKNOWN when the level has no graph.
int PfLineInNodes(const Vector3& from, const Vector3& to);

// The box round the cells a hero at `from` can walk to (as PfReachable) within
// `radius` units of it (x / z), from the lowest of their nodes' boxes to the
// highest; false when `from` is in no node of the graph.
bool PfReachBox(const Vector3& from, float radius, Vector3* mn, Vector3* mx);

// Autotests: the walkable cell nearest `from` (at least `minDist` units off,
// at most `maxDist`) that a hero at `from` can't walk to nor come from; its
// centre on top of its node's box in `out`.
bool PfSpotCutOff(const Vector3& from, float minDist, float maxDist, Vector3* out);

// The graph in the log: nodes, transitions, doors; with `around`, the cells
// within `radius` units of it as a map.
void PfDump(const Vector3* around, float radius);
