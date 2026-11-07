// Beacon klusters seen from both players' cameras.
//
// Crates and placed helmets are not permanent objects: the level stores them as
// beacons, grouped into klusters (bounding spheres) per sector, and every frame
// their managers (CKCrateCpnt::vf13, CKGrpBonusPool::vf27...) ask the beacon
// service (yellow pages +0x78) for the klusters worth a live object --
// FUN_0046DED0(handler, flags, &outside, ref). With flag 1 that query keeps only
// klusters inside the camera service's frustum (planes at +0x500..+0x54C:
// far and four sides, normals outward), and sorts them (flag 2) by Manhattan
// distance to `ref` or the camera (+0x5B0). That frustum is player 1's camera:
// turning it made crates vanish from player 2's half, and the placeholder
// geometry of crates without a live object showed instead.
//
// While the split screen is up the query is replaced by the same walk with a
// union test -- a kluster counts if it is in either player's frustum -- and the
// distance to the nearer of the two cameras as its sort key. Player 2's planes
// come from the game's own builder (FUN_00457170) run on a stand-in camera
// service whose frame is player 2's camera and whose view window is its half's;
// the game camera gets a second frustum with the window of the half showing it.

#include "klusters.h"
#include "config.h"
#include "coop.h"
#include "detour.h"
#include "kal.h"
#include "log.h"
#include "p2cam.h"
#include "splitscreen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

const uintptr_t kKlusterQuery   = 0x46DED0;
const uint8_t   kKlusterQueryPrologue[7] = { 0x6A, 0xFF, 0x68, 0xE8, 0xCF, 0x61, 0x00 };
const uintptr_t kNextSector     = 0x46E240;   // thiscall(sectorManager, sector)
const uintptr_t kBuildPlanes    = 0x457170;   // thiscall(cameraService)
const uintptr_t kKeyScale       = 0x6222E0;   // float

struct Entry { Entry* next; uint32_t key; };

typedef Entry*(__fastcall* QueryFn)(void* self, void* edx, int handler, uint32_t flags, Entry** outside,
                                     const float* ref);
typedef char*(__fastcall* NextSectorFn)(void* mgr, void* edx, char* sector);
typedef void(__fastcall* BuildPlanesFn)(void* svc, void* edx);

QueryFn g_origQuery = nullptr;

// Stand-in camera service for FUN_00457170: +0x18 a view object (near/far planes,
// view window: the half's), +0x1C -> object whose +4 is the frame, planes
// written at +0x4F0..+0x54C.
struct FakeFrameOwner { void* vtbl; char* frame; };
// Up to two extra views: player 2's camera, and player 1's own camera while the
// game camera follows player 2's leading hero (then neither half shows the game
// camera exactly, and each player's view needs its own frustum).
const int kViews = 2;
alignas(16) char g_fakeSvc[kViews][0x800];
alignas(16) char g_fakeFrame[kViews][0xC0];
FakeFrameOwner g_fakeOwner[kViews] = { { nullptr, g_fakeFrame[0] }, { nullptr, g_fakeFrame[1] } };
bool  g_viewOn[kViews] = {};
float g_viewPos[kViews][3];
bool  g_p2Planes = false;
// The game camera again, with the view window of the half that shows it. A half
// is narrower than the screen but, with split_fov, taller: the camera service's
// full-screen frustum (+0x500) left out the top and bottom of each half, and
// crates and Romans there came in only when the camera turned to them.
alignas(16) char g_gameHalfSvc[0x800];
bool  g_gameHalfOn = false;
// Stand-in view objects (svc +0x18) for the builder: +0x10 -> a copy of the main
// camera (view window +0x68/+0x6C, near / far +0x80/+0x84) with a half's window.
alignas(16) char g_fakeCam[kViews + 1][0x90];
char* g_fakeRend[kViews + 1][5];
volatile long g_queries = 0, g_p2Only = 0;

bool InFrustum(const char* planes, const float* c, float r) {
	for (int i = 0; i < 5; i++) {
		const float* p = (const float*)(planes + 0x10 * i);
		float d = c[0] * p[0] + c[1] * p[1] + c[2] * p[2] + p[3];
		if (d > r) return false;
	}
	return true;
}

float Manhattan(const float* a, const float* b) {
	return fabsf(a[0] - b[0]) + fabsf(a[1] - b[1]) + fabsf(a[2] - b[2]);
}

void Insert(Entry*& head, Entry* e, uint32_t key, bool sorted) {
	if (!sorted) { e->next = head; head = e; return; }
	e->key = key;
	Entry* prev = nullptr;
	Entry* cur = head;
	while (cur && cur->key > key) { prev = cur; cur = cur->next; }
	e->next = cur;
	if (prev) prev->next = e;
	else head = e;
}

// Crates. CKCrateCpnt::vf13 (0x542AB0, thiscall(cpnt, handler, ?), ret 8) asks
// for its klusters with flags 7: the beacons of those outside every view lose
// their crate objects (CKHkCrate: list (cpnt +4)->vf12, next +4, beacon +0x58,
// -1 free), then those inside get free objects, nearest first, until none is
// left. An object stays with its crate while the kluster is in a view, however
// far: with two views the far crates held them while nearer ones on the other
// half waited (Caesar's Challenge level 1: they came in as Asterix walked up, or
// as player 2's camera turned away). The klusters past the objects there are
// are now passed as outside, so the game's own pass frees theirs.
const uintptr_t kCrateVtbl       = 0x6291E0;
const size_t    kCrateUpdateSlot = 13;
const uintptr_t kCrateUpdate     = 0x542AB0;
typedef void(__fastcall* CrateUpdateFn)(void* self, void* edx, int handler, int arg);
CrateUpdateFn g_origCrateUpdate = nullptr;
char*  g_crateCpnt = nullptr;     // during its vf13
Entry* g_crateInside = nullptr;   // the crate query's answer, for the log after vf13
// Two klusters at the cut traded objects every frame while a camera swung (76
// hand-overs in the second after a warp, 4 without the cut): a kluster with
// crates standing ranks as if a quarter nearer, and one handed objects keeps
// them for kCrateHoldMs.
const float kCrateLiveRank = 0.75f;
const DWORD kCrateHoldMs   = 1500;
struct CrateGiven { Entry* e; DWORD at; };
CrateGiven g_crateGiven[256];
DWORD* CrateGivenAt(Entry* e, bool add) {
	CrateGiven* oldest = &g_crateGiven[0];
	for (CrateGiven& g : g_crateGiven) {
		if (g.e == e) return &g.at;
		if (!g.e || g.at < oldest->at) oldest = &g;
	}
	if (!add) return nullptr;
	oldest->e = e;
	oldest->at = 0;
	return &oldest->at;
}

// A beacon's bits: the beacon service's sector record (svc +0x18, 0x40 bytes,
// the entry's sector at +0x13) has a bit array at +0x28; beacon i's bits start
// at entry +0x11 (bits per beacon) * i + entry +0x16: three of pieces left, and
// +6 set while a crate object stands for it.
bool Bit(const uint8_t* a, unsigned i) { return (a[i >> 3] >> (i & 7)) & 1; }
// Beacons of `e` holding or wanting a crate object; *waiting counts the wanting.
unsigned CrateNeeds(char* svc, Entry* e, unsigned* waiting) {
	char* recs = *(char**)(svc + 0x18);
	char* rec = recs ? recs + *((uint8_t*)e + 0x13) * 0x40 : nullptr;
	const uint8_t* a = rec && *(void**)rec ? *(const uint8_t**)(rec + 0x28) : nullptr;
	if (!a) return 0;
	unsigned n = *(unsigned*)((char*)e + 0xC), per = *((uint8_t*)e + 0x11), base = *(uint16_t*)((char*)e + 0x16);
	unsigned need = 0;
	for (unsigned i = 0; i < n; i++) {
		unsigned b = (per * i + base) & 0xFFFF;
		bool live = Bit(a, b + 6);
		bool pieces = Bit(a, b) || Bit(a, b + 1) || Bit(a, b + 2);
		if (live || pieces) need++;
		if (!live && pieces && waiting) ++*waiting;
	}
	return need;
}

unsigned CrateObjects(char* cpnt, unsigned* freeN) {
	typedef char**(__fastcall* ListFn)(void* self, void* edx);
	char* owner = *(char**)(cpnt + 4);
	char** head = owner ? ((ListFn)(*(void***)owner)[12])(owner, nullptr) : nullptr;
	unsigned n = 0;
	for (char* o = head ? *head : nullptr; o && n < 4096; o = *(char**)(o + 4)) {
		n++;
		if (*(int*)(o + 0x58) == -1) ++*freeN;
	}
	return n;
}

void __fastcall CrateUpdateHook(char* self, void* edx, int handler, int arg) {
	// Objects handed to another crate this update (a crate gone here, one come there).
	typedef char**(__fastcall* ListFn)(void* self, void* edx);
	const int kMaxObj = 128;
	int before[kMaxObj];
	char* owner = *(char**)(self + 4);
	char** head = owner ? ((ListFn)(*(void***)owner)[12])(owner, nullptr) : nullptr;
	int nObj = 0;
	for (char* o = head ? *head : nullptr; o && nObj < kMaxObj; o = *(char**)(o + 4)) before[nObj++] = *(int*)(o + 0x58);
	g_crateCpnt = self;
	g_crateInside = nullptr;
	g_origCrateUpdate(self, edx, handler, arg);
	g_crateCpnt = nullptr;
	if (!g_crateInside) return;
	static unsigned moves[256];
	static DWORD since[256];
	static long movesLogged = 0;
	int i = 0;
	for (char* o = head ? *head : nullptr; o && i < nObj; o = *(char**)(o + 4), i++)
		if (*(int*)(o + 0x58) != before[i] && *(int*)(o + 0x58) != -1) moves[handler & 0xFF]++;
	DWORD now = GetTickCount();
	if (now - since[handler & 0xFF] >= 1000) {
		if (moves[handler & 0xFF] && movesLogged < 60) {
			movesLogged++;
			Log("klusters: crates (handler %d): %u objects handed to another crate in the last second", handler & 0xFF,
			    moves[handler & 0xFF]);
		}
		moves[handler & 0xFF] = 0;
		since[handler & 0xFF] = now;
	}
	// Crates in view still without an object after the update: they show late.
	CKYellowPages* yp = YellowPages();
	char* bsvc = yp ? *(char**)((char*)yp + 0x78) : nullptr;
	unsigned waiting = 0, freeN = 0, n = 0;
	for (Entry* e = g_crateInside; bsvc && e && n < 1024; e = e->next, n++) CrateNeeds(bsvc, e, &waiting);
	unsigned objects = CrateObjects(self, &freeN);
	static int lastWaiting[256];
	static bool primed = false;
	static long logged = 0;
	if (!primed) { primed = true; memset(lastWaiting, 0xFF, sizeof(lastWaiting)); }
	int& last = lastWaiting[handler & 0xFF];
	if ((int)(waiting > 0) != last && logged < 40) {
		logged++;
		Log("klusters: crates (handler %d): %u in view without an object, %u of %u objects free", handler & 0xFF, waiting,
		    freeN, objects);
	}
	last = waiting > 0;
}

// Crates: the klusters past the objects there are go outside (CrateUpdateHook),
// but not one handed its objects less than kCrateHoldMs ago. `inside` comes
// nearest first (flag 2).
void CutCrates(char* self, int handler, Entry*& inside, Entry*& out) {
	DWORD now = GetTickCount();
	unsigned freeN = 0, cap = CrateObjects(g_crateCpnt, &freeN), sum = 0, moved = 0;
	Entry** link = &inside;
	while (*link && sum < cap) {
		unsigned waiting = 0;
		sum += CrateNeeds(self, *link, &waiting);
		if (waiting) *CrateGivenAt(*link, true) = now;
		link = &(*link)->next;
	}
	while (Entry* e = *link) {
		unsigned waiting = 0;
		DWORD* at = CrateNeeds(self, e, &waiting) > waiting ? CrateGivenAt(e, false) : nullptr;
		if (at && now - *at < kCrateHoldMs) {
			link = &e->next;
			continue;
		}
		*link = e->next;
		Insert(out, e, 0, false);
		moved++;
	}
	static int lastMoved[256];
	static bool primed = false;
	static long logged = 0;
	if (!primed) { primed = true; memset(lastMoved, 0xFF, sizeof(lastMoved)); }
	int& last = lastMoved[handler & 0xFF];
	if ((int)(moved > 0) != last && logged < 40) {
		logged++;
		Log("klusters: crates (handler %d): %u klusters past the %u objects passed as outside", handler & 0xFF, moved, cap);
	}
	last = moved > 0;
}

// FUN_0046DED0, with the union frustum (see the top of the file).
Entry* __fastcall QueryHook(char* self, void* edx, int handler, uint32_t flags, Entry** outside,
                            const float* ref) {
	CKYellowPages* yp = YellowPages();
	char* svc = yp ? *(char**)((char*)yp + 0x68) : nullptr;
	if (!(flags & 1) || !g_p2Planes || !svc || !SplitActive()) {
		Entry* r = g_origQuery(self, edx, handler, flags, outside, ref);
		if (g_crateCpnt && flags == 7) {
			// Single player with sp_isolation off: the nearest crates get the objects
			// here too (the game keeps one with a crate anywhere in its view -- LV5-3's
			// fight: 26 objects, up to 7 crates in view without one).
			if (g_config.SoloMod() && !CoopP2Joined() && outside && (flags & 1)) CutCrates(self, handler, r, *outside);
			g_crateInside = r;   // (the crates' log in single player too)
		}
		return r;
	}
	InterlockedIncrement(&g_queries);

	if (outside) *outside = nullptr;
	unsigned count = *(unsigned*)(self + 0x14);
	const uint8_t* tab = *(const uint8_t**)(self + 0xC);
	int slot = -1;
	for (int i = (int)count - 1; i >= 0; i--)
		if (tab[i * 12 + 3] == (uint8_t)handler) { slot = tab[i * 12 + 2]; break; }
	if (slot < 0 || slot == 0xFF) return nullptr;

	char* mgr = *(char**)((char*)yp + 0x90);
	NextSectorFn next = (NextSectorFn)kNextSector;
	const float* ref1 = ref ? ref : (const float*)(svc + 0x5B0);
	const float* ref2 = ref ? nullptr : g_viewPos[0];
	float scale = *(const float*)kKeyScale;
	Entry* inside = nullptr;
	Entry* out = nullptr;
	// Sorted by camera distance (no explicit reference point): the managers fill
	// a limited pool of live objects from the front of the list (helmets), so
	// sorting by the distance to the nearest camera gave everything to the player
	// standing close to beacons -- player 1 turning the camera made player 2's
	// helmets 60 units away come and go. Each camera's visible klusters are
	// ranked by distance to it, and the two rankings are interleaved.
	const int kMaxFair = 1024;
	static Entry* fe[kMaxFair];
	static float fd[kMaxFair][kViews + 1];
	static uint32_t fr[kMaxFair];
	static unsigned fn[kMaxFair];   // objects a kluster counts for in the merge
	int nf = 0;
	bool fair = !ref && (flags & 2);
	for (char* sector = next(mgr, nullptr, nullptr); sector; sector = next(mgr, nullptr, sector)) {
		for (char* k = *(char**)(sector + 0x34); k; k = *(char**)(k + 4)) {
			signed char idx = *(signed char*)(*(char**)(k + 0x20) + slot);
			if (idx == -1) continue;
			Entry* e = (Entry*)(*(char**)(k + 0x1C) + idx * 0x18);
			if (!e) continue;
			e->next = nullptr;
			const float* c = (const float*)(k + 8);
			float r = *(const float*)(k + 0x14);
			uint32_t key = 0;
			if (flags & 6) {
				float d = Manhattan(c, ref1);
				for (int v = 0; ref2 && v < kViews; v++)
					if (g_viewOn[v]) d = fminf(d, Manhattan(c, g_viewPos[v]));
				key = (uint32_t)(-(int)(d * scale) - 1);
			}
			bool in1 = InFrustum(svc + 0x500, c, r);
			if (!in1 && g_gameHalfOn && InFrustum(g_gameHalfSvc + 0x500, c, r)) {
				in1 = true;
				static long halfOnly = 0;
				long n = InterlockedIncrement(&halfOnly);
				if (n <= 8 || n % 1000 == 0)
					Log("klusters: handler %d (flags %u): kluster (%.0f %.0f %.0f r%.0f) in the game camera's half only, not its full-screen frustum (%ld)",
					    handler & 0xFF, flags, c[0], c[1], c[2], r, n);
			}
			bool inV[kViews] = {};
			bool in = in1;
			for (int v = 0; v < kViews; v++)
				if (g_viewOn[v]) { inV[v] = InFrustum(g_fakeSvc[v] + 0x500, c, r); in = in || inV[v]; }
			if (in) {
				if (!in1) InterlockedIncrement(&g_p2Only);
				if (fair && nf < kMaxFair) {
					fe[nf] = e;
					float rank = 1.0f;
					unsigned waiting = 0;
					fn[nf] = *(unsigned*)((char*)e + 0xC);
					if (g_crateCpnt && flags == 7) {   // crates: the ones holding or wanting an object
						fn[nf] = CrateNeeds(self, e, &waiting);
						if (fn[nf] > waiting) rank = kCrateLiveRank;
					}
					fd[nf][0] = in1 ? Manhattan(c, ref1) * rank : 1e30f;
					for (int v = 0; v < kViews; v++) fd[nf][v + 1] = inV[v] ? Manhattan(c, g_viewPos[v]) * rank : 1e30f;
					nf++;
				} else {
					Insert(inside, e, key, (flags & 2) != 0);
				}
			} else if (flags & 4) {
				Insert(out, e, key, (flags & 8) != 0);
			}
		}
	}
	if (nf) {
		// Rank per camera, then merge the rankings so every camera gets the same
		// number of objects (a kluster entry counts its beacons at +0xC, crates the
		// ones holding or wanting an object): the next place goes to the camera that
		// has had the fewest so far.
		const int cams = kViews + 1;
		for (int i = 0; i < nf; i++) fr[i] = 0xFFFFFFFFu;
		static int order[kViews + 1][kMaxFair];
		int m[kViews + 1] = {}, at[kViews + 1] = {};
		unsigned given[kViews + 1] = {};
		for (int cam = 0; cam < cams; cam++) {
			for (int i = 0; i < nf; i++) if (fd[i][cam] < 1e29f) order[cam][m[cam]++] = i;
			std::sort(order[cam], order[cam] + m[cam], [cam](int a, int b) { return fd[a][cam] < fd[b][cam]; });
		}
		for (uint32_t pos = 0;; pos++) {
			int best = -1;
			for (int cam = 0; cam < cams; cam++) {
				while (at[cam] < m[cam] && fr[order[cam][at[cam]]] != 0xFFFFFFFFu) at[cam]++;
				if (at[cam] < m[cam] && (best < 0 || given[cam] < given[best])) best = cam;
			}
			if (best < 0) break;
			int i = order[best][at[best]++];
			fr[i] = pos;
			given[best] += fn[i] ? fn[i] : 1;
		}
		for (int i = 0; i < nf; i++) Insert(inside, fe[i], 0xFFFFFFFFu - fr[i], true);
	}
	if (g_crateCpnt && flags == 7) {
		CutCrates(self, handler, inside, out);
		g_crateInside = inside;
	}
	static const char* klog = getenv("XXL_KLOG");   // diagnostics: "x,z" of a point to follow
	if (klog && (flags & 3) == 3) {
		static float wx = 0, wz = 0;
		static bool parsed = false;
		if (!parsed) { parsed = true; sscanf(klog, "%f,%f", &wx, &wz); }
		static DWORD last[256] = {};
		if (GetTickCount() - last[handler & 0xFF] > 500) {
			last[handler & 0xFF] = GetTickCount();
			int rank = 0, found = -1, total = 0;
			for (Entry* e = inside; e; e = e->next, rank++) total++;
			// find the kluster around the point again and report its frustum tests and rank
			for (char* sector = next(mgr, nullptr, nullptr); sector; sector = next(mgr, nullptr, sector))
				for (char* k = *(char**)(sector + 0x34); k; k = *(char**)(k + 4)) {
					const float* c = (const float*)(k + 8);
					float r = *(const float*)(k + 0x14);
					float dx = c[0] - wx, dz = c[2] - wz;
					if (dx * dx + dz * dz > (r + 8) * (r + 8)) continue;
					signed char idx = *(signed char*)(*(char**)(k + 0x20) + slot);
					Entry* e = idx == -1 ? nullptr : (Entry*)(*(char**)(k + 0x1C) + idx * 0x18);
					if (!e) continue;
					int pos = -1, i = 0;
					for (Entry* x = inside; x; x = x->next, i++) if (x == e) pos = i;
					found = pos;
					Log("klusters: handler %d: kluster (%.0f %.0f %.0f r%.0f) entry %p in game view %d, other %d/%d, rank %d of %d",
					    handler & 0xFF, c[0], c[1], c[2], r, e, InFrustum(svc + 0x500, c, r), g_viewOn[0] && InFrustum(g_fakeSvc[0] + 0x500, c, r),
					    g_viewOn[1] && InFrustum(g_fakeSvc[1] + 0x500, c, r), pos, total);
				}
			(void)found;
			char buf[512];
			int k2 = sprintf(buf, "klusters: handler %d list:", handler & 0xFF);
			for (Entry* x = inside; x && k2 < 470; x = x->next) {
				int fi = -1;
				for (int i = 0; i < nf; i++) if (fe[i] == x) fi = i;
				k2 += sprintf(buf + k2, " %u", *(unsigned*)((char*)x + 0xC));
				if (fi >= 0)
					for (int cam = 0; cam < kViews + 1; cam++)
						if (fd[fi][cam] < 1e29f) k2 += sprintf(buf + k2, "%c%.0f", "GPQ"[cam], fd[fi][cam]);
			}
			Log("%s", buf);
		}
	}
	if (outside) *outside = out;
	return inside;
}

} // namespace

void KlusterSetP2Camera() {
	g_p2Planes = false;
	g_gameHalfOn = false;
	CKYellowPages* yp = YellowPages();
	char* svc = yp ? *(char**)((char*)yp + 0x68) : nullptr;
	char* rend = svc ? *(char**)(svc + 0x18) : nullptr;
	if (!rend) return;
	char* cam = *(char**)(rend + 0x10);
	// The view object to build view i's planes with: player `player`'s half window.
	auto halfView = [&](int i, int player) -> void* {
		float w[2];
		if (!cam || !SplitHalfWindow(player, w)) return nullptr;
		memcpy(g_fakeCam[i], cam, sizeof(g_fakeCam[i]));
		*(float*)(g_fakeCam[i] + 0x68) = w[0];
		*(float*)(g_fakeCam[i] + 0x6C) = w[1];
		g_fakeRend[i][4] = g_fakeCam[i];
		return g_fakeRend[i];
	};
	for (int v = 0; v < kViews; v++) {
		float m[16];
		g_viewOn[v] = v == 0 ? P2CamMatrix(m) : P1CamMatrix(m);
		if (!g_viewOn[v]) continue;
		void* view = halfView(v, v == 0 ? 1 : 0);
		memcpy(g_fakeFrame[v] + 0x10, m, sizeof(m));
		*(void**)(g_fakeSvc[v] + 0x18) = view ? view : rend;
		*(void**)(g_fakeSvc[v] + 0x1C) = &g_fakeOwner[v];
		((BuildPlanesFn)kBuildPlanes)(g_fakeSvc[v], nullptr);
		g_viewPos[v][0] = m[12]; g_viewPos[v][1] = m[13]; g_viewPos[v][2] = m[14];
	}
	// The half showing the game camera: player 1's unless it has its own camera,
	// else player 2's when its hero leads and it has none.
	int gameHalf = !g_viewOn[1] ? 0 : !g_viewOn[0] ? 1 : -1;
	void* view = gameHalf >= 0 ? halfView(kViews, gameHalf) : nullptr;
	if (view && *(void**)(svc + 0x1C)) {
		*(void**)(g_gameHalfSvc + 0x18) = view;
		*(void**)(g_gameHalfSvc + 0x1C) = *(void**)(svc + 0x1C);
		((BuildPlanesFn)kBuildPlanes)(g_gameHalfSvc, nullptr);
		g_gameHalfOn = true;
	}
	g_p2Planes = g_viewOn[0] || g_viewOn[1] || g_gameHalfOn;
}

bool KlusterSphereInOtherViews(const float* c, float r) {
	if (!g_p2Planes || !SplitActive()) return false;
	if (g_gameHalfOn && InFrustum(g_gameHalfSvc + 0x500, c, r)) return true;
	for (int v = 0; v < kViews; v++)
		if (g_viewOn[v] && InFrustum(g_fakeSvc[v] + 0x500, c, r)) return true;
	return false;
}

bool KlusterSplitUp() { return g_p2Planes && SplitActive(); }

bool KlusterSphereInAnyView(const float* c, float r, float maxDist) {
	const float* eyes[4];
	return KlusterViewsSeeing(c, r, maxDist, eyes) > 0;
}

int KlusterViewsSeeing(const float* c, float r, float maxDist, const float* eyes[4]) {
	CKYellowPages* yp = YellowPages();
	char* svc = yp ? *(char**)((char*)yp + 0x68) : nullptr;
	auto closeTo = [&](const float* cam) {
		float dx = c[0] - cam[0], dy = c[1] - cam[1], dz = c[2] - cam[2];
		return dx * dx + dy * dy + dz * dz < maxDist * maxDist;
	};
	int n = 0;
	const float* gameEye = svc ? (const float*)(svc + 0x5B0) : nullptr;
	bool game = svc && closeTo(gameEye) && InFrustum(svc + 0x500, c, r);
	if (!game && svc && g_p2Planes && SplitActive() && g_gameHalfOn && closeTo(gameEye))
		game = InFrustum(g_gameHalfSvc + 0x500, c, r);
	if (game) eyes[n++] = gameEye;
	if (!g_p2Planes || !SplitActive()) return n;
	for (int v = 0; v < kViews; v++)
		if (g_viewOn[v] && closeTo(g_viewPos[v]) && InFrustum(g_fakeSvc[v] + 0x500, c, r)) eyes[n++] = g_viewPos[v];
	return n;
}

long KlusterQueries() { return g_queries; }
long KlusterP2Only() { return g_p2Only; }

bool KlusterInstallHooks() {
	bool ok = InstallDetour(kKlusterQuery, (void*)QueryHook, (void**)&g_origQuery, kKlusterQueryPrologue,
	                        sizeof(kKlusterQueryPrologue), "beacon kluster query");
	ok &= PatchVtableSlot(kCrateVtbl, kCrateUpdateSlot, kCrateUpdate, (void*)CrateUpdateHook, (void**)&g_origCrateUpdate,
	                      "CKCrateCpnt::update");
	return ok;
}
