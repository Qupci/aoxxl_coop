#include "pathfind.h"
#include "log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// CKSrvPathFinding (vt 0x62483C, yellow pages +0x64): +0xC nodes, +0x10 count.
// CKPFGraphNode (vt 0x6247DC): +8 box max, +0x14 box min; the grid at +0x20
// (+0x24 cells, 4 bits each, the even index in the high half; +0x28 / +0x29
// cells across x / z); +0x2C transitions, +0x30 their count (u16).
// A cell's value: 7 wall, 0 enemies only (cyan in XXL-Editor: doors), 4 the
// sidekick only (yellow), 1-6 walkable; bit 3 set by objects standing on it
// (crates, crumbly zones, rolling stones: FUN_0045A880) -- blocked.
// CKPFGraphTransition (vt 0x62480C): +4 0 = walk over to the node, else a jump
// through one of its doors; +8 the node it leads to; +0xC flags: bit 31 on
// (events 0xA00 on / 0xA01 off, bit 30 the level start's), bits 0-1 who may
// take it (1 the sidekick, 2 enemies); +0x10 doors (0x38 each: +4 the box
// jumped from, max then min, +0x1C the box landed in), +0x14 their count.
// The game's own search (FUN_0045D890 over nodes, FUN_0045E1F0 over one node's
// cells) runs a request at a time over frames; this one walks both at once.

namespace {

struct Node {
	char* raw;
	float mn[3], mx[3];
	int nx, nz, base;
	const uint8_t* cells;
	float cw, ch;
};

struct Jump { int from, sx, sz, to, dx, dz; };

struct Graph {
	std::vector<Node> nodes;
	std::vector<Jump> jumps;
	std::vector<std::vector<int>> walkTo;   // per node: nodes walked over to
	std::vector<uint8_t> closedDoor;        // per cell: a closed door stands on it
	std::vector<uint8_t> closedNode;        // per node: shut (PfCloseNodesAt)
	int cells = 0;
};

// PfCloseNodesAt's places, and the nodes they shut last time (for the log).
Vector3 s_closeAt[16];
int s_nCloseAt = 0;
// A node shut by a place in it is a small one (LV2-1's lifter shaft, node 53:
// 9.2 x 12.2 units) -- not a room a lifter stands in.
const float kClosedNodeMax = 16.0f;

const uintptr_t kSrvVt = 0x62483C, kNodeVt = 0x6247DC, kTransVt = 0x62480C;

char* Service() {
	CKYellowPages* yp = YellowPages();
	char* srv = yp ? (char*)yp->p_64 : nullptr;
	if (!srv || IsBadReadPtr(srv, 0x14) || *(uintptr_t*)srv != kSrvVt) return nullptr;
	return srv;
}

int Value(const Node& n, int x, int z) {
	int i = z * n.nx + x;
	uint8_t b = n.cells[i >> 1];
	return (i & 1) ? (b & 0xF) : (b >> 4);
}

bool Inside(const Node& n, const Vector3& p, float slack) {
	return p.x >= n.mn[0] && p.x <= n.mx[0] && p.z >= n.mn[2] && p.z <= n.mx[2] && p.y >= n.mn[1] - slack &&
	       p.y <= n.mx[1] + slack;
}

void CellOf(const Node& n, float x, float z, int* cx, int* cz) {
	int ix = (int)((x - n.mn[0]) / n.cw), iz = (int)((z - n.mn[2]) / n.ch);
	*cx = ix < 0 ? 0 : ix >= n.nx ? n.nx - 1 : ix;
	*cz = iz < 0 ? 0 : iz >= n.nz ? n.nz - 1 : iz;
}

Vector3 Centre(const Node& n, int x, int z) {
	return Vector3(n.mn[0] + (x + 0.5f) * n.cw, n.mn[1], n.mn[2] + (z + 0.5f) * n.ch);
}

// Swing doors (CKHkSwingDoor 2,98): +0x18 how far open (0..1), +0x2C / +0x30
// the leaves' nodes (frame +4: its own rotation at +0x10, cos of the leaf's
// angle -- 1 shut; its world place +0x80). The level has more door objects
// than doors (LV2-3: 8 for 2, the extra ones pointing at the same leaves and
// never moving), so the leaves themselves tell: a leaf turned is a door open.
struct Door { Vector3 at[2]; bool closed; };
const float kDoorReach = 3.0f;

void Doors(std::vector<Door>* out) {
	for (unsigned i = 0, n = GetNumLvlClassInstances(2, 98); i < n; i++) {
		char* o = (char*)GetLvlObject(2, 98, i);
		if (!o || IsBadReadPtr(o, 0x34)) continue;
		Door d;
		d.closed = true;
		bool ok = true;
		for (int k = 0; k < 2 && ok; k++) {
			char* node = *(char**)(o + 0x2C + 4 * k);
			char* fr = node && !IsBadReadPtr(node, 8) ? *(char**)(node + 4) : nullptr;
			if (!fr || IsBadReadPtr(fr, 0x8C)) { ok = false; break; }
			const float* p = (const float*)(fr + 0x80);
			d.at[k] = Vector3(p[0], p[1], p[2]);
			if (*(float*)(fr + 0x10) < 0.97f) d.closed = false;
		}
		bool seen = false;
		for (const Door& e : *out) seen |= (e.at[0] - d.at[0]).len2() < 0.01f && (e.at[1] - d.at[1]).len2() < 0.01f;
		if (ok && !seen) out->push_back(d);
	}
}

bool Load(Graph* g) {
	char* srv = Service();
	if (!srv) return false;
	char** list = *(char***)(srv + 0xC);
	unsigned count = *(uint32_t*)(srv + 0x10);
	if (!list || count == 0 || count > 4096 || IsBadReadPtr(list, count * 4)) return false;
	for (unsigned i = 0; i < count; i++) {
		char* r = list[i];
		Node n;
		n.raw = r;
		if (!r || IsBadReadPtr(r, 0x38) || *(uintptr_t*)r != kNodeVt) return false;
		for (int k = 0; k < 3; k++) {
			n.mx[k] = *(float*)(r + 8 + 4 * k);
			n.mn[k] = *(float*)(r + 0x14 + 4 * k);
		}
		n.nx = *(uint8_t*)(r + 0x28);
		n.nz = *(uint8_t*)(r + 0x29);
		n.cells = *(const uint8_t**)(r + 0x24);
		if (!n.nx || !n.nz || !n.cells || IsBadReadPtr(n.cells, (n.nx * n.nz + 1) / 2)) return false;
		n.cw = (n.mx[0] - n.mn[0]) / n.nx;
		n.ch = (n.mx[2] - n.mn[2]) / n.nz;
		n.base = g->cells;
		g->cells += n.nx * n.nz;
		g->nodes.push_back(n);
	}
	g->walkTo.resize(count);
	for (unsigned i = 0; i < count; i++) {
		char* r = g->nodes[i].raw;
		char** tr = *(char***)(r + 0x2C);
		unsigned nt = *(uint16_t*)(r + 0x30);
		if (!nt || !tr || IsBadReadPtr(tr, nt * 4)) continue;
		for (unsigned k = 0; k < nt; k++) {
			char* t = tr[k];
			if (!t || IsBadReadPtr(t, 0x18) || *(uintptr_t*)t != kTransVt) continue;
			uint32_t flags = *(uint32_t*)(t + 0xC);
			bool on = (flags & 0x80000000u) != 0, sidekicks = (flags & 1) != 0;
			char* to = *(char**)(t + 8);
			int ti = to ? *(uint16_t*)(to + 0x32) : -1;
			if (ti < 0 || ti >= (int)count || g->nodes[ti].raw != to) continue;
			if (*(uint8_t*)(t + 4) == 0) {
				// Walked over: shut only when it is the sidekick's and off (a bridge, a
				// platform not there). The enemies' own (LV2-3's hearth room: the
				// sidekick is never to go in) are a door's: the door's cells tell.
				if (!on && sidekicks) continue;
				g->walkTo[i].push_back(ti);
				continue;
			}
			if (!on || !sidekicks) continue;   // a jump: the sidekick's, and on (LV2-6's bridge, by its lever)
			char* doors = *(char**)(t + 0x10);
			unsigned nd = *(uint32_t*)(t + 0x14);
			if (!doors || nd > 64 || IsBadReadPtr(doors, nd * 0x38)) continue;
			for (unsigned d = 0; d < nd; d++) {
				const float* b = (const float*)(doors + d * 0x38 + 4);   // max xyz, min xyz, then the landing box's
				Jump j;
				j.from = i;
				j.to = ti;
				CellOf(g->nodes[i], (b[0] + b[3]) * 0.5f, (b[2] + b[5]) * 0.5f, &j.sx, &j.sz);
				CellOf(g->nodes[ti], (b[6] + b[9]) * 0.5f, (b[8] + b[11]) * 0.5f, &j.dx, &j.dz);
				g->jumps.push_back(j);
			}
		}
	}
	g->closedNode.assign(count, 0);
	for (int k = 0; k < s_nCloseAt; k++)
		for (unsigned i = 0; i < count; i++) {
			const Node& n = g->nodes[i];
			if (n.mx[0] - n.mn[0] <= kClosedNodeMax && n.mx[2] - n.mn[2] <= kClosedNodeMax && Inside(n, s_closeAt[k], 2.0f))
				g->closedNode[i] = 1;
		}
	// The enemies-only cells (doors) by a closed door are shut.
	g->closedDoor.assign(g->cells, 0);
	std::vector<Door> doors;
	Doors(&doors);
	for (const Door& d : doors) {
		if (!d.closed) continue;
		for (const Node& n : g->nodes)
			for (int k = 0; k < 2; k++) {
				const Vector3& p = d.at[k];
				if (p.x < n.mn[0] - kDoorReach || p.x > n.mx[0] + kDoorReach || p.z < n.mn[2] - kDoorReach ||
				    p.z > n.mx[2] + kDoorReach || p.y < n.mn[1] - 4.0f || p.y > n.mx[1] + 4.0f)
					continue;
				for (int z = 0; z < n.nz; z++)
					for (int x = 0; x < n.nx; x++) {
						if (Value(n, x, z) != 0) continue;
						Vector3 c = Centre(n, x, z);
						float dx = c.x - p.x, dz = c.z - p.z;
						if (dx * dx + dz * dz <= kDoorReach * kDoorReach) g->closedDoor[n.base + z * n.nx + x] = 1;
					}
			}
	}
	return true;
}

bool Walkable(const Graph& g, int ni, int x, int z) {
	if (g.closedNode[ni]) return false;
	const Node& n = g.nodes[ni];
	int v = Value(n, x, z);
	if (v & 8 || v == 7) return false;
	return !g.closedDoor[n.base + z * n.nx + x];
}

// How far inside a node's box a place is (x / z), negative outside it.
float Depth(const Node& n, const Vector3& p) {
	float d = p.x - n.mn[0];
	if (n.mx[0] - p.x < d) d = n.mx[0] - p.x;
	if (p.z - n.mn[2] < d) d = p.z - n.mn[2];
	if (n.mx[2] - p.z < d) d = n.mx[2] - p.z;
	return d;
}

// The cells a place stands for: its own if walkable, else the walkable ones of
// the nearest ring around it (up to 3 cells: a hero on a ledge's edge, in a
// jump). Of the nodes it is in, the one it is deepest in that has any (LV2-6:
// the bridge's node reaches a cell onto the cliff it lands on -- standing
// there is not standing on the bridge).
void Seeds(const Graph& g, const Vector3& p, std::vector<int>* out) {
	std::vector<int> order;
	for (int ni = 0; ni < (int)g.nodes.size(); ni++)
		if (Inside(g.nodes[ni], p, 2.0f)) order.push_back(ni);
	for (size_t i = 1; i < order.size(); i++)
		for (size_t k = i; k > 0 && Depth(g.nodes[order[k]], p) > Depth(g.nodes[order[k - 1]], p); k--)
			std::swap(order[k], order[k - 1]);
	for (int ni : order) {
		const Node& n = g.nodes[ni];
		int cx, cz;
		CellOf(n, p.x, p.z, &cx, &cz);
		for (int r = 0; r <= 3; r++) {
			bool any = false;
			for (int z = cz - r; z <= cz + r; z++)
				for (int x = cx - r; x <= cx + r; x++) {
					if (x < 0 || z < 0 || x >= n.nx || z >= n.nz) continue;
					if (abs(x - cx) != r && abs(z - cz) != r) continue;
					if (!Walkable(g, ni, x, z)) continue;
					out->push_back(n.base + z * n.nx + x);
					any = true;
				}
			if (any) break;
		}
		if (!out->empty()) return;
	}
}

int NodeOfCell(const Graph& g, int c) {
	int lo = 0, hi = (int)g.nodes.size() - 1;
	while (lo < hi) {
		int mid = (lo + hi + 1) / 2;
		if (g.nodes[mid].base <= c) lo = mid; else hi = mid - 1;
	}
	return lo;
}

// Each step a hero can take from cell `c`: fn(cell, its length in units, x / z
// from cell centre to cell centre).
template <class Fn> void Steps(const Graph& g, int c, Fn fn) {
	int ni = NodeOfCell(g, c);
	if (g.closedNode[ni]) return;   // (a shut node: no way out of it either)
	const Node& n = g.nodes[ni];
	int x = (c - n.base) % n.nx, z = (c - n.base) / n.nx;
	for (int dz = -1; dz <= 1; dz++)
		for (int dx = -1; dx <= 1; dx++) {
			if (!dx && !dz) continue;
			int ax = x + dx, az = z + dz;
			if (ax < 0 || az < 0 || ax >= n.nx || az >= n.nz) continue;
			// diagonally only past a walkable corner
			if (dx && dz && !Walkable(g, ni, x + dx, z) && !Walkable(g, ni, x, z + dz)) continue;
			if (!Walkable(g, ni, ax, az)) continue;
			fn(n.base + az * n.nx + ax, sqrtf(dx * dx * n.cw * n.cw + dz * dz * n.ch * n.ch));
		}
	// over to the next node: a cell at (or a cell from) its box
	Vector3 ctr = Centre(n, x, z);
	auto toward = [&](int ti, int tx, int tz) {
		Vector3 d = Centre(g.nodes[ti], tx, tz) - ctr;
		fn(g.nodes[ti].base + tz * g.nodes[ti].nx + tx, sqrtf(d.x * d.x + d.z * d.z));
	};
	for (int ti : g.walkTo[ni]) {
		const Node& t = g.nodes[ti];
		if (ctr.x < t.mn[0] - n.cw || ctr.x > t.mx[0] + n.cw || ctr.z < t.mn[2] - n.ch || ctr.z > t.mx[2] + n.ch)
			continue;
		int tx, tz;
		CellOf(t, ctr.x, ctr.z, &tx, &tz);
		if (Walkable(g, ti, tx, tz)) toward(ti, tx, tz);
	}
	// a jump from within a cell of its take-off box's centre (that one may be a ledge's)
	for (const Jump& j : g.jumps)
		if (j.from == ni && abs(j.sx - x) <= 1 && abs(j.sz - z) <= 1 && !g.closedNode[j.to]) toward(j.to, j.dx, j.dz);
}

// Every cell walked to from `seeds` (marks in `seen`).
void Flood(const Graph& g, const std::vector<int>& seeds, std::vector<uint8_t>* seen) {
	seen->assign(g.cells, 0);
	std::vector<int> queue(seeds);
	for (int c : seeds) (*seen)[c] = 1;
	for (size_t q = 0; q < queue.size(); q++)
		Steps(g, queue[q], [&](int d, float) {
			if ((*seen)[d]) return;
			(*seen)[d] = 1;
			queue.push_back(d);
		});
}

// The walks' lengths to one place (PfWayTo): every cell's shortest way there,
// over the steps turned round (a drop is walked one way only).
struct WayField {
	Graph g;
	std::vector<float> len;
	DWORD at = 0;
	bool ok = false;
} s_way;
const float kNoWay = 1e30f;

} // namespace

int PfReachable(const Vector3& from, const Vector3& to) {
	Graph g;
	if (!Load(&g)) return PF_UNKNOWN;
	std::vector<int> a, b;
	Seeds(g, from, &a);
	Seeds(g, to, &b);
	if (a.empty() || b.empty()) return PF_UNKNOWN;
	std::vector<uint8_t> seen;
	Flood(g, a, &seen);
	for (int c : b)
		if (seen[c]) return PF_YES;
	return PF_NO;
}

namespace {
// Milliseconds since `t0` (QueryPerformanceCounter), for the logs.
double MsSince(const LARGE_INTEGER& t0) {
	LARGE_INTEGER t1, f;
	QueryPerformanceCounter(&t1);
	QueryPerformanceFrequency(&f);
	return (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
}
}

void PfCloseNodesAt(const Vector3* at, int n) {
	s_nCloseAt = 0;
	for (int i = 0; at && i < n && s_nCloseAt < 16; i++) s_closeAt[s_nCloseAt++] = at[i];
}

float PfWalk(const Vector3& from, const Vector3& to) {
	LARGE_INTEGER t0;
	QueryPerformanceCounter(&t0);
	Graph g;
	if (!Load(&g)) return -2.0f;
	std::vector<int> a, b;
	Seeds(g, from, &a);
	Seeds(g, to, &b);
	if (a.empty() || b.empty()) return -2.0f;
	std::vector<uint8_t> target(g.cells, 0);
	for (int c : b) target[c] = 1;
	std::vector<float> len(g.cells, kNoWay);
	typedef std::pair<float, int> Item;
	std::vector<Item> heap;
	auto later = [](const Item& x, const Item& y) { return x.first > y.first; };
	for (int c : a) {
		const Node& n = g.nodes[NodeOfCell(g, c)];
		Vector3 d = Centre(n, (c - n.base) % n.nx, (c - n.base) / n.nx) - from;
		float l = sqrtf(d.x * d.x + d.z * d.z);
		if (l < len[c]) len[c] = l;
		heap.push_back(Item(l, c));
	}
	std::make_heap(heap.begin(), heap.end(), later);
	float found = -1.0f;
	int popped = 0;
	while (!heap.empty()) {
		std::pop_heap(heap.begin(), heap.end(), later);
		Item it = heap.back();
		heap.pop_back();
		if (it.first > len[it.second]) continue;
		popped++;
		if (target[it.second]) { found = it.first; break; }
		Steps(g, it.second, [&](int d, float l) {
			float nl = it.first + l;
			if (nl >= len[d]) return;
			len[d] = nl;
			heap.push_back(Item(nl, d));
			std::push_heap(heap.begin(), heap.end(), later);
		});
	}
	static int s_told = 0;
	if (s_told < 20) {
		s_told++;
		Log("pf: a walk from (%.1f %.1f %.1f) to (%.1f %.1f %.1f): %.1f units (%d cells looked at, %.1f ms)", from.x, from.y,
		    from.z, to.x, to.y, to.z, found, popped, MsSince(t0));
	}
	return found;
}

bool PfWayTo(const Vector3& to) {
	LARGE_INTEGER t0;
	QueryPerformanceCounter(&t0);
	WayField& w = s_way;
	w.ok = false;
	w.g = Graph();
	if (!Load(&w.g)) return false;
	const Graph& g = w.g;
	std::vector<int> targets;
	Seeds(g, to, &targets);
	if (targets.empty()) return false;
	// The steps turned round: for each cell, the cells a step leads to it from.
	std::vector<int> first(g.cells + 1, 0);
	for (int c = 0; c < g.cells; c++) Steps(g, c, [&](int d, float) { first[d + 1]++; });
	for (int c = 0; c < g.cells; c++) first[c + 1] += first[c];
	std::vector<int> from(first[g.cells]);
	std::vector<float> stepLen(first[g.cells]);
	std::vector<int> fill(first.begin(), first.end() - 1);
	for (int c = 0; c < g.cells; c++)
		Steps(g, c, [&](int d, float l) {
			from[fill[d]] = c;
			stepLen[fill[d]++] = l;
		});
	w.len.assign(g.cells, kNoWay);
	typedef std::pair<float, int> Item;
	std::vector<Item> heap;
	auto later = [](const Item& a, const Item& b) { return a.first > b.first; };
	for (int c : targets) {
		w.len[c] = 0.0f;
		heap.push_back(Item(0.0f, c));
	}
	std::make_heap(heap.begin(), heap.end(), later);
	while (!heap.empty()) {
		std::pop_heap(heap.begin(), heap.end(), later);
		Item it = heap.back();
		heap.pop_back();
		if (it.first > w.len[it.second]) continue;
		for (int k = first[it.second]; k < first[it.second + 1]; k++) {
			float l = it.first + stepLen[k];
			if (l >= w.len[from[k]]) continue;
			w.len[from[k]] = l;
			heap.push_back(Item(l, from[k]));
			std::push_heap(heap.begin(), heap.end(), later);
		}
	}
	w.at = GetTickCount();
	w.ok = true;
	static int s_told = 0;
	if (s_told < 20) {
		s_told++;
		Log("pf: the walks to (%.1f %.1f %.1f) over %d cells: %.1f ms", to.x, to.y, to.z, g.cells, MsSince(t0));
	}
	return true;
}

float PfWayFrom(const Vector3& from) {
	const WayField& w = s_way;
	if (!w.ok || GetTickCount() - w.at > 1000) return -2.0f;
	std::vector<int> seeds;
	Seeds(w.g, from, &seeds);
	if (seeds.empty()) return -2.0f;
	float best = kNoWay;
	for (int c : seeds) {
		if (w.len[c] >= kNoWay) continue;
		const Node& n = w.g.nodes[NodeOfCell(w.g, c)];
		Vector3 d = Centre(n, (c - n.base) % n.nx, (c - n.base) / n.nx) - from;
		float l = w.len[c] + sqrtf(d.x * d.x + d.z * d.z);
		if (l < best) best = l;
	}
	return best < kNoWay ? best : -1.0f;
}

int PfLineInNodes(const Vector3& from, const Vector3& to) {
	Graph g;
	if (!Load(&g)) return PF_UNKNOWN;
	float dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
	int steps = (int)(sqrtf(dx * dx + dz * dz) / 0.25f) + 1;
	for (int i = 0; i <= steps; i++) {
		float k = (float)i / steps;
		Vector3 p(from.x + dx * k, from.y + dy * k, from.z + dz * k);
		bool in = false;
		for (const Node& n : g.nodes)
			if (Inside(n, p, 1.0f)) { in = true; break; }
		if (!in) return PF_NO;
	}
	return PF_YES;
}

int PfConnected(const Vector3& a, const Vector3& b) {
	Graph g;
	if (!Load(&g)) return PF_UNKNOWN;
	std::vector<int> sa, sb;
	Seeds(g, a, &sa);
	Seeds(g, b, &sb);
	if (sa.empty() || sb.empty()) return PF_UNKNOWN;
	std::vector<uint8_t> seen;
	Flood(g, sa, &seen);
	for (int c : sb)
		if (seen[c]) return PF_YES;
	Flood(g, sb, &seen);
	for (int c : sa)
		if (seen[c]) return PF_YES;
	return PF_NO;
}

bool PfReachBox(const Vector3& from, float radius, Vector3* mn, Vector3* mx) {
	Graph g;
	if (!Load(&g)) return false;
	std::vector<int> seeds;
	Seeds(g, from, &seeds);
	if (seeds.empty()) return false;
	std::vector<uint8_t> seen;
	Flood(g, seeds, &seen);
	bool any = false;
	for (int ni = 0; ni < (int)g.nodes.size(); ni++) {
		const Node& n = g.nodes[ni];
		for (int z = 0; z < n.nz; z++)
			for (int x = 0; x < n.nx; x++) {
				if (!seen[n.base + z * n.nx + x]) continue;
				Vector3 c = Centre(n, x, z);
				float dx = c.x - from.x, dz = c.z - from.z;
				if (dx * dx + dz * dz > radius * radius) continue;
				Vector3 lo(c.x - n.cw * 0.5f, n.mn[1], c.z - n.ch * 0.5f), hi(c.x + n.cw * 0.5f, n.mx[1], c.z + n.ch * 0.5f);
				if (!any) { *mn = lo; *mx = hi; any = true; continue; }
				if (lo.x < mn->x) mn->x = lo.x;
				if (lo.y < mn->y) mn->y = lo.y;
				if (lo.z < mn->z) mn->z = lo.z;
				if (hi.x > mx->x) mx->x = hi.x;
				if (hi.y > mx->y) mx->y = hi.y;
				if (hi.z > mx->z) mx->z = hi.z;
			}
	}
	return any;
}

bool PfSpotCutOff(const Vector3& from, float minDist, float maxDist, Vector3* out) {
	Graph g;
	if (!Load(&g)) return false;
	std::vector<int> seeds;
	Seeds(g, from, &seeds);
	if (seeds.empty()) return false;
	std::vector<uint8_t> reached, back;
	Flood(g, seeds, &reached);
	struct Cand { float d2; int ni, x, z; };
	std::vector<Cand> cands;
	for (int ni = 0; ni < (int)g.nodes.size(); ni++) {
		const Node& n = g.nodes[ni];
		for (int z = 0; z < n.nz; z++)
			for (int x = 0; x < n.nx; x++) {
				if (reached[n.base + z * n.nx + x] || !Walkable(g, ni, x, z)) continue;
				Vector3 c = Centre(n, x, z);
				float dx = c.x - from.x, dz = c.z - from.z, d2 = dx * dx + dz * dz;
				if (d2 >= minDist * minDist && d2 <= maxDist * maxDist) cands.push_back({ d2, ni, x, z });
			}
	}
	std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
	for (size_t i = 0; i < cands.size() && i < 40; i++) {
		const Node& n = g.nodes[cands[i].ni];
		std::vector<int> s(1, n.base + cands[i].z * n.nx + cands[i].x);
		Flood(g, s, &back);
		bool way = false;
		for (int c : seeds) way |= back[c] != 0;
		if (way) continue;
		*out = Centre(n, cands[i].x, cands[i].z);
		out->y = n.mx[1] + 0.5f;
		return true;
	}
	return false;
}

void PfDump(const Vector3* around, float radius) {
	Graph g;
	if (!Load(&g)) {
		Log("pf: no pathfinding graph");
		return;
	}
	Log("pf: %u nodes, %d cells, %u jumps on", (unsigned)g.nodes.size(), g.cells, (unsigned)g.jumps.size());
	for (int ni = 0; ni < (int)g.nodes.size(); ni++) {
		const Node& n = g.nodes[ni];
		if (around && (around->x < n.mn[0] - radius || around->x > n.mx[0] + radius || around->z < n.mn[2] - radius ||
		               around->z > n.mx[2] + radius))
			continue;
		int counts[16] = {};
		for (int z = 0; z < n.nz; z++)
			for (int x = 0; x < n.nx; x++) counts[Value(n, x, z)]++;
		Log("pf: node %d %p (%.1f %.1f %.1f)..(%.1f %.1f %.1f) %dx%d cell %.2fx%.2f; walls %d, cyan %d, yellow %d, "
		    "taken %d; other node %p",
		    ni, n.raw, n.mn[0], n.mn[1], n.mn[2], n.mx[0], n.mx[1], n.mx[2], n.nx, n.nz, n.cw, n.ch, counts[7], counts[0],
		    counts[4], counts[8] + counts[9] + counts[10] + counts[11] + counts[12] + counts[13] + counts[14] + counts[15],
		    *(void**)(n.raw + 0x34));
		char** tr = *(char***)(n.raw + 0x2C);
		unsigned nt = *(uint16_t*)(n.raw + 0x30);
		for (unsigned k = 0; tr && k < nt; k++) {
			char* t = tr[k];
			char* to = *(char**)(t + 8);
			unsigned nd = *(uint32_t*)(t + 0x14);
			char* doors = *(char**)(t + 0x10);
			Log("pf:   -> node %d kind %u flags %08X doors %u", to ? *(uint16_t*)(to + 0x32) : -1, *(uint8_t*)(t + 4),
			    *(uint32_t*)(t + 0xC), nd);
			for (unsigned d = 0; doors && d < nd && d < 8; d++) {
				const float* b = (const float*)(doors + d * 0x38 + 4);
				Log("pf:      jump (%.1f %.1f %.1f) -> (%.1f %.1f %.1f) +34 %08X", (b[0] + b[3]) * 0.5f,
				    (b[1] + b[4]) * 0.5f, (b[2] + b[5]) * 0.5f, (b[6] + b[9]) * 0.5f, (b[7] + b[10]) * 0.5f,
				    (b[8] + b[11]) * 0.5f, *(uint32_t*)(doors + d * 0x38 + 0x34));
			}
		}
		if (!around) continue;
		// The map: '#' wall, '.' walkable, 'c' enemies only, 'y' sidekick only,
		// 'D' a closed door's, capitals taken by an object, '@' the place.
		int cx = -1, cz = -1;
		if (Inside(n, *around, 2.0f)) CellOf(n, around->x, around->z, &cx, &cz);
		int x0, z0, x1, z1;
		CellOf(n, around->x - radius, around->z - radius, &x0, &z0);
		CellOf(n, around->x + radius, around->z + radius, &x1, &z1);
		for (int z = z1; z >= z0; z--) {
			char row[300];
			int len = 0;
			for (int x = x0; x <= x1 && len < 290; x++) {
				int v = Value(n, x, z);
				char ch = v == 7 ? '#' : v == 0 ? 'c' : v == 4 ? 'y' : v == 1 ? '.' : v < 8 ? (char)('0' + v) : v == 15 ? 'W'
				          : v == 8 ? 'C' : v == 12 ? 'Y' : v == 9 ? 'X' : (char)('0' + v);
				if (g.closedDoor[n.base + z * n.nx + x]) ch = 'D';
				if (x == cx && z == cz) ch = '@';
				row[len++] = ch;
			}
			row[len] = 0;
			Log("pf:   z %7.1f |%s|", n.mn[2] + (z + 0.5f) * n.ch, row);
		}
		Log("pf:   x from %.1f, %.2f a cell", n.mn[0] + (x0 + 0.5f) * n.cw, n.cw);
	}
	static const int kinds[] = { 98, 100 };
	for (int id : kinds)
		for (unsigned i = 0, n = GetNumLvlClassInstances(2, id); i < n; i++) {
			char* o = (char*)GetLvlObject(2, id, i);
			if (!o || IsBadReadPtr(o, 0x34)) continue;
			char* node = *(char**)(o + 0x10);
			float pp[2][3] = {}, cosA[2] = { 9, 9 };
			for (int k = 0; k < 2; k++) {
				char* lf = *(char**)(o + 0x2C + 4 * k);
				char* fr = lf && !IsBadReadPtr(lf, 8) ? *(char**)(lf + 4) : nullptr;
				if (fr && !IsBadReadPtr(fr, 0x8C)) {
					memcpy(pp[k], fr + 0x80, 12);
					cosA[k] = *(float*)(fr + 0x10);
				}
			}
			if (around) {
				float dx = pp[0][0] - around->x, dz = pp[0][2] - around->z;
				if (dx * dx + dz * dz > radius * radius * 4) continue;
			}
			Log("pf: door (2,%d) #%u %p +8 %08X node %p flags %04X leaves %p (%.1f %.1f %.1f) %p (%.1f %.1f %.1f) "
			    "cos %.2f %.2f open %.2f step %u/%u +29 %u +2B %02X events %04X %04X",
			    id, i, o, *(uint32_t*)(o + 8), node, node && !IsBadReadPtr(node, 0x14) ? *(uint16_t*)(node + 0x10) : 0,
			    *(void**)(o + 0x2C), pp[0][0], pp[0][1], pp[0][2], *(void**)(o + 0x30), pp[1][0], pp[1][1], pp[1][2],
			    cosA[0], cosA[1], *(float*)(o + 0x18), *(uint8_t*)(o + 0x2A), *(uint8_t*)(o + 0x28), *(uint8_t*)(o + 0x29),
			    *(uint8_t*)(o + 0x2B), *(uint16_t*)(o + 0x24), *(uint16_t*)(o + 0x26));
		}
}
