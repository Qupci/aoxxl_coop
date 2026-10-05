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
// service whose frame is player 2's camera.

#include "klusters.h"
#include "coop.h"
#include "detour.h"
#include "kal.h"
#include "log.h"
#include "p2cam.h"
#include "splitscreen.h"

#include <cmath>
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

// Stand-in camera service for FUN_00457170: +0x18 the real one's view object
// (near/far planes, view window), +0x1C -> object whose +4 is the frame, planes
// written at +0x4F0..+0x54C.
struct FakeFrameOwner { void* vtbl; char* frame; };
alignas(16) char g_fakeSvc[0x800];
alignas(16) char g_fakeFrame[0xC0];
FakeFrameOwner g_fakeOwner = { nullptr, g_fakeFrame };
bool  g_p2Planes = false;
float g_p2Pos[3];
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

// FUN_0046DED0, with the union frustum (see the top of the file).
Entry* __fastcall QueryHook(char* self, void* edx, int handler, uint32_t flags, Entry** outside,
                            const float* ref) {
	CKYellowPages* yp = YellowPages();
	char* svc = yp ? *(char**)((char*)yp + 0x68) : nullptr;
	if (!(flags & 1) || !g_p2Planes || !svc || !SplitActive())
		return g_origQuery(self, edx, handler, flags, outside, ref);
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
	const float* ref2 = ref ? nullptr : g_p2Pos;
	float scale = *(const float*)kKeyScale;
	Entry* inside = nullptr;
	Entry* out = nullptr;
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
				if (ref2) d = fminf(d, Manhattan(c, ref2));
				key = (uint32_t)(-(int)(d * scale) - 1);
			}
			bool in1 = InFrustum(svc + 0x500, c, r);
			bool in = in1 || InFrustum(g_fakeSvc + 0x500, c, r);
			if (in) {
				if (!in1) InterlockedIncrement(&g_p2Only);
				Insert(inside, e, key, (flags & 2) != 0);
			} else if (flags & 4) {
				Insert(out, e, key, (flags & 8) != 0);
			}
		}
	}
	if (outside) *outside = out;
	return inside;
}

} // namespace

void KlusterSetP2Camera() {
	g_p2Planes = false;
	CKYellowPages* yp = YellowPages();
	char* svc = yp ? *(char**)((char*)yp + 0x68) : nullptr;
	float m[16];
	if (!svc || !*(void**)(svc + 0x18) || !OtherViewMatrix(m)) return;
	memcpy(g_fakeFrame + 0x10, m, sizeof(m));
	*(void**)(g_fakeSvc + 0x18) = *(void**)(svc + 0x18);
	*(void**)(g_fakeSvc + 0x1C) = &g_fakeOwner;
	((BuildPlanesFn)kBuildPlanes)(g_fakeSvc, nullptr);
	g_p2Pos[0] = m[12]; g_p2Pos[1] = m[13]; g_p2Pos[2] = m[14];
	g_p2Planes = true;
}

long KlusterQueries() { return g_queries; }
long KlusterP2Only() { return g_p2Only; }

bool KlusterInstallHooks() {
	return InstallDetour(kKlusterQuery, (void*)QueryHook, (void**)&g_origQuery, kKlusterQueryPrologue,
	                     sizeof(kKlusterQueryPrologue), "beacon kluster query");
}
