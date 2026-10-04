#include "splitscreen.h"
#include "coop.h"
#include "config.h"
#include "log.h"
#include "render.h"
#include "trio.h"
#include "p2cam.h"

#include <cstring>

namespace {

// --- game functions (all __thiscall; called through __fastcall with a dummy edx) ---
typedef void(__fastcall* ThisFn)(void* self, void* edx);
typedef void(__fastcall* ThisArgFn)(void* self, void* edx, void* arg);
typedef void(__fastcall* ThisByteFn)(void* self, void* edx, int arg);
typedef void(__fastcall* CollectFn)(void* self, void* edx, void** lists, void* dt);
typedef void(__fastcall* GraphicTickFn)(void* self, void* edx, void* dt);

const ThisFn     SyncCameras    = (ThisFn)0x413DC0;       // FUN_00413DC0(view)
// Clone visibility: CClone / CAnimatedClone nodes (crates, enemies...) do not go
// into the draw lists during the scene walk; they stamp their instance with the
// frame id, and this then enables every stamped instance and disables the rest.
const ThisArgFn  SyncClones     = (ThisArgFn)0x422D10;    // FUN_00422D10(view+0x58, dt)
const ThisFn     ResetDirty     = (ThisFn)0x413E70;       // FUN_00413E70(view): RW dirty frames
const ThisByteFn ClearCamera    = (ThisByteFn)0x414700;   // FUN_00414700(renderer, zOnly)
const ThisArgFn  DrawWorld      = (ThisArgFn)0x413F40;    // FUN_00413F40(view, dt)
const ThisArgFn  DrawEffects    = (ThisArgFn)0x413FF0;    // Render_View(view, dt)
const ThisArgFn  DrawHud        = (ThisArgFn)0x414070;    // FUN_00414070(view, dt)
const ThisFn     Present        = (ThisFn)0x414780;       // FUN_00414780(renderer)

typedef void*(__cdecl* SetViewWindowFn)(void* camera, float* window);
const SetViewWindowFn RwCameraSetViewWindow = (SetViewWindowFn)0x58FFB0;

const uintptr_t kGraphicVtbl     = 0x623D54;    // CKGraphic
const size_t    kGraphicTickSlot = 17;
const uintptr_t kGraphicTick     = 0x431A40;
const uintptr_t kFrameId         = 0x6464C4;    // u16, bumped once per rendered frame
const uintptr_t kD3DDevicePtr    = 0x668978;    // IDirect3DDevice9* (RenderWare's)

GraphicTickFn g_origGraphicTick = nullptr;

// --- swept collision spheres ---------------------------------------------------
// CKDynamicBoundingSphere (and CKDynBSphereProjectile) record their motion in the
// scene walk: vf14 moves the previous position to +0x30, the motion since then to
// +0x24 and the current one to +0x3C. The collision service sweeps the sphere
// along that motion (OBB test FUN_00440F10), which is how a dash or a punch
// registers with springs, crates, enemies... A second walk in the same frame
// leaves a motion of zero, and every sweep falls back to a static overlap test:
// player 2's Obelix dashing into a spring never touched it. Only the first walk
// of a split frame updates them; the later walks just stamp them visible.
const uintptr_t kDynSphereVtbl     = 0x622DE8;   // CKDynamicBoundingSphere (11,15)
const uintptr_t kDynProjectileVtbl = 0x622E30;   // CKDynBSphereProjectile (11,6)
const size_t    kWalkSlot          = 14;
const uintptr_t kDynSphereWalk     = 0x416440;
const CollectFn LeafWalk           = (CollectFn)0x41EFE0;   // CSGLeaf: stamp only
CollectFn g_origDynSphereWalk = nullptr;
bool      g_repeatWalk = false;

void __fastcall DynSphereWalkHook(void* self, void* edx, void** lists, void* dt) {
	if (g_repeatWalk) LeafWalk(self, edx, lists, dt);
	else              g_origDynSphereWalk(self, edx, lists, dt);
}

// --- viewport override ---------------------------------------------------------
// RenderWare's D3D9 camera begin-update sets the viewport from the camera's
// raster; the main camera's own beginUpdate pointer is wrapped so a half-screen
// viewport can be put in place right after it, for everything the pass draws.
struct Viewport { unsigned x, y, w, h; float minZ, maxZ; };   // D3DVIEWPORT9
typedef void*(__cdecl* BeginUpdateFn)(void* camera);
BeginUpdateFn g_origBeginUpdate = nullptr;
void*         g_wrappedCamera = nullptr;
bool          g_vpActive = false;
Viewport      g_vp = {};

void SetViewport(const Viewport& vp) {
	void* dev = *(void**)kD3DDevicePtr;
	if (!dev) return;
	typedef long(__stdcall* SetViewportFn)(void* dev, const Viewport* vp);
	SetViewportFn fn = (SetViewportFn)(*(void***)dev)[47];
	fn(dev, &vp);
}

void ClearRect(unsigned x, unsigned y, unsigned w, unsigned h, unsigned color) {
	void* dev = *(void**)kD3DDevicePtr;
	if (!dev || !w || !h) return;
	struct { long x1, y1, x2, y2; } rect = { (long)x, (long)y, (long)(x + w), (long)(y + h) };
	typedef long(__stdcall* ClearFn)(void* dev, unsigned long count, const void* rects,
	                                 unsigned long flags, unsigned long color, float z,
	                                 unsigned long stencil);
	ClearFn fn = (ClearFn)(*(void***)dev)[43];
	fn(dev, 1, &rect, 1 /* D3DCLEAR_TARGET */, color, 1.0f, 0);
}

void* __cdecl BeginUpdateWrapper(void* camera) {
	void* r = g_origBeginUpdate(camera);
	if (r && g_vpActive && camera == g_wrappedCamera) SetViewport(g_vp);
	return r;
}

void EnsureBeginUpdateWrapped(RwCamera* cam) {
	if (!cam) return;
	if (cam == g_wrappedCamera && cam->beginUpdate == (void*)BeginUpdateWrapper) return;
	// A new camera (level change, device reset) or someone replaced ours.
	g_origBeginUpdate = (BeginUpdateFn)cam->beginUpdate;
	g_wrappedCamera = cam;
	cam->beginUpdate = (void*)BeginUpdateWrapper;
	Log("split: wrapped beginUpdate of camera %p (was %p)", cam, (void*)g_origBeginUpdate);
}

// --- dummy draw lists ---------------------------------------------------------
// Draw lists are intrusive (item+0x24 = next). The three HUD-layer lists are only
// drawn at the very end, so player 1's walk has already filled them when player
// 2's walk runs; re-adding the same items would link them into cycles and hang
// the HUD pass. Player 2's walk gets these do-nothing lists in those slots.
void __fastcall DummyAdd(void*, void*, void*) {}
void __fastcall DummyNoArg(void*, void*) {}
int  __fastcall DummyIsEmpty(void*, void*) { return 1; }
void* g_dummyVtbl[8] = {
	(void*)DummyAdd, (void*)DummyAdd, (void*)DummyNoArg, (void*)DummyNoArg,
	(void*)DummyNoArg, (void*)DummyNoArg, (void*)DummyIsEmpty, (void*)DummyNoArg,
};
struct DummyList { void** vtbl; void* head; uint32_t flags; } g_dummy = { g_dummyVtbl, nullptr, 0 };

const int kNumLists   = 15;   // 11 world + 1 effects + 3 HUD, at view+0x1C
const int kFirstHud   = 12;

// --- per-player cameras --------------------------------------------------------
// v1: each half gets the game's camera, translated so that it frames that player's
// hero exactly the way the game frames its leader -- same angle, same distance.
// The game's camera service keeps following the trio leader as usual.
struct SavedCamera {
	float modelling[16];
	float ltm[16];
	float window[2];
};

float* FrameModelling(RwCamera* cam) { return (float*)((char*)(*(void**)((char*)cam + 4)) + 0x10); }
float* FrameLtm(RwCamera* cam)       { return (float*)((char*)(*(void**)((char*)cam + 4)) + 0x50); }

void Translate(RwCamera* cam, const SavedCamera& base, const Vector3& d) {
	if (d.len2() == 0.0f) return;
	float* m = FrameModelling(cam);
	float* l = FrameLtm(cam);
	memcpy(m, base.modelling, sizeof(base.modelling));
	memcpy(l, base.ltm, sizeof(base.ltm));
	m[12] += d.x; m[13] += d.y; m[14] += d.z;   // RwMatrix pos at +0x30
	l[12] += d.x; l[13] += d.y; l[14] += d.z;
}

volatile long g_splitFrames = 0;

// One-off diagnostic: does our sync really move the camera's view matrix and
// frustum planes (what the scene walk culls with), or only BeginUpdate does?
int g_frustumLogs = 0;
void LogFrustum(const char* tag, RwCamera* cam) {
	if (g_frustumLogs >= 12) return;
	g_frustumLogs++;
	const float* vm = (const float*)((char*)cam + 0x20);
	const float* fp = (const float*)((char*)cam + 0x94);
	Log("split: %-12s view.pos=(%.2f %.2f %.2f) view.at=(%.2f %.2f %.2f) plane0=(%.3f %.3f %.3f %.2f)",
		tag, vm[12], vm[13], vm[14], vm[8], vm[9], vm[10], fp[0], fp[1], fp[2], fp[3]);
}
bool          g_splitActive = false;
bool          g_loggedLayout = false;

// The render of one frame with two 3D passes. Mirrors FUN_00413EC0 step for step.
void RenderSplitFrame(char* view, void* dt) {
	char* renderer = *(char**)(view + 0x08);
	if (!*(void**)(renderer + 0x04) || !*(void**)(renderer + 0x08)) return;
	RwCamera* cam = *(RwCamera**)(renderer + 0x10);
	void* root = *(void**)(view + 0x0C);
	if (!cam || !root || !cam->frameBuffer) return;
	EnsureBeginUpdateWrapped(cam);

	Trio t = GetTrio();
	Hero leader = t ? t.GetHero(t.Leader()) : Hero();
	Hero h1 = t ? t.GetHero(CoopP1Hero()) : Hero();
	Hero h2 = t ? t.GetHero(CoopP2Hero()) : Hero();

	// Screen halves.
	char* raster = (char*)cam->frameBuffer;
	unsigned w = *(unsigned*)(raster + 0x0C), h = *(unsigned*)(raster + 0x10);
	bool vertical = g_config.splitMode == SPLIT_VERTICAL;
	const unsigned gap = 2;
	Viewport vp1, vp2;
	if (vertical) {
		unsigned half = w / 2;
		vp1 = { 0, 0, half - gap, h, 0.0f, 1.0f };
		vp2 = { half + gap, 0, w - half - gap, h, 0.0f, 1.0f };
	} else {
		unsigned half = h / 2;
		vp1 = { 0, 0, w, half - gap, 0.0f, 1.0f };
		vp2 = { 0, half + gap, w, h - half - gap, 0.0f, 1.0f };
	}

	// 1. Camera sync, exactly as the game does it, then remember the game's camera.
	SyncCameras(view, nullptr);

	SavedCamera p1cam;
	memcpy(p1cam.modelling, FrameModelling(cam), sizeof(p1cam.modelling));
	memcpy(p1cam.ltm, FrameLtm(cam), sizeof(p1cam.ltm));
	p1cam.window[0] = cam->viewWindow[0];
	p1cam.window[1] = cam->viewWindow[1];

	// Half-screen view window: keep the horizontal field of view for a stacked
	// split and the vertical one for a side-by-side split, so nothing is squashed.
	float half[2] = { p1cam.window[0], p1cam.window[1] };
	if (vertical) half[0] *= (float)vp1.w / (float)w;
	else          half[1] *= (float)vp1.h / (float)h;
	if (!g_loggedLayout) {
		g_loggedLayout = true;
		char* frame = *(char**)((char*)cam + 4);
		Log("split: %ux%u %s, view window %.4f %.4f -> %.4f %.4f; camera frame %p root %p",
			w, h, vertical ? "side by side" : "stacked", p1cam.window[0], p1cam.window[1],
			half[0], half[1], frame, frame ? *(void**)(frame + 0xA0) : nullptr);
	}

	// Offsets from the hero the game's camera frames to each player's hero.
	Vector3 d1, d2;
	if (leader && h1) d1 = h1.Pos() - leader.Pos();
	if (leader && h2) d2 = h2.Pos() - leader.Pos();

	// Each pass: view window, camera position, sync (view matrix and the frustum
	// the scene walk culls against), scene walk, draw. RwCameraSetViewWindow only
	// marks the frame dirty, so the sync has to come after it.
	CollectFn collect = (CollectFn)(*(void***)root)[14];
	uint32_t viewFlags = *(uint32_t*)(view + 0x68);

	// Clones (crates, enemies...) are drawn as merged batches that the clone
	// manager rebuilds from the instances stamped visible this frame, and the
	// scene walk culls those batches by their bounds. One clone sync per player
	// leaves each batch covering only the *last* player's instances, so the
	// other player's walk culls it: player 1's camera turning made crates pop in
	// player 2's half. Instead, stamp what player 2 sees first (a walk into
	// lists that ignore everything), then player 1's real walk, then one sync
	// over both views.
	void* stampLists[kNumLists];
	for (int i = 0; i < kNumLists; i++) stampLists[i] = &g_dummy;
	float m2[16];
	bool own2 = P2CamMatrix(m2);
	auto placeP2 = [&] {
		if (own2) {
			memcpy(FrameModelling(cam), m2, sizeof(m2));
			memcpy(FrameLtm(cam), m2, sizeof(m2));
		} else {
			Translate(cam, p1cam, d2);
		}
	};
	auto placeP1 = [&] {
		memcpy(FrameModelling(cam), p1cam.modelling, sizeof(p1cam.modelling));
		memcpy(FrameLtm(cam), p1cam.ltm, sizeof(p1cam.ltm));
		Translate(cam, p1cam, d1);
	};

	// 2. Stamp-only walk for player 2. Being the frame's first walk, it is also
	//    the one that updates the swept collision spheres.
	g_repeatWalk = false;
	RwCameraSetViewWindow(cam, half);
	placeP2();
	SyncCameras(view, nullptr);
	LogFrustum("p2 stamp", cam);
	collect(root, nullptr, stampLists, dt);
	g_repeatWalk = true;

	// 3. Player 1's view.
	placeP1();
	SyncCameras(view, nullptr);
	LogFrustum("p1 walk", cam);
	collect(root, nullptr, (void**)(view + 0x1C), dt);
	SyncClones(*(void**)(view + 0x58), nullptr, dt);
	ResetDirty(view, nullptr);
	if (!(viewFlags & 0x10)) ClearCamera(renderer, nullptr, 0);
	g_vp = vp1; g_vpActive = true;
	DrawWorld(view, nullptr, dt);
	DrawEffects(view, nullptr, dt);
	g_vpActive = false;

	// 4. Player 2's view: fresh world lists (player 1's pass drew and emptied
	//    them); the clone batches already cover both views.
	*(uint32_t*)(view + 0x68) = viewFlags;
	placeP2();
	SyncCameras(view, nullptr);
	LogFrustum("p2 walk", cam);
	void* lists[kNumLists];
	for (int i = 0; i < kNumLists; i++)
		lists[i] = i >= kFirstHud ? (void*)&g_dummy : *(void**)(view + 0x1C + 4 * i);
	collect(root, nullptr, lists, dt);
	g_repeatWalk = false;
	ResetDirty(view, nullptr);
	g_vp = vp2; g_vpActive = true;
	DrawWorld(view, nullptr, dt);
	DrawEffects(view, nullptr, dt);
	g_vpActive = false;

	// 5. Back to the game's own camera, so everything after the render (sound,
	//    game logic, next frame's camera service) sees what it expects.
	RwCameraSetViewWindow(cam, p1cam.window);
	memcpy(FrameModelling(cam), p1cam.modelling, sizeof(p1cam.modelling));
	memcpy(FrameLtm(cam), p1cam.ltm, sizeof(p1cam.ltm));
	SyncCameras(view, nullptr);

	// Divider, then the HUD over the whole screen, then present.
	SetViewport({ 0, 0, w, h, 0.0f, 1.0f });
	if (vertical) ClearRect(vp1.w, 0, vp2.x - vp1.w, h, 0xFF000000);
	else          ClearRect(0, vp1.h, w, vp2.y - vp1.h, 0xFF000000);
	DrawHud(view, nullptr, dt);
	Present(renderer, nullptr);
	*(uint32_t*)(view + 0x68) &= 0xFFFFFFC1;
	InterlockedIncrement(&g_splitFrames);
}

bool WantSplit() {
	return g_config.splitScreen && CoopP2Joined() && !CoopSuspended();
}

void __fastcall GraphicTickHook(void* self, void* edx, void* dt) {
	bool want = WantSplit();
	if (want != g_splitActive) {
		g_splitActive = want;
		Log("split: %s", want ? "on" : "off");
	}
	if (!want) { g_origGraphicTick(self, edx, dt); return; }

	// What CKGraphic::vf17 does before rendering: bump the frame id (0xFFFE wraps
	// to 1, so 0 and 0xFFFF stay free as markers).
	uint16_t& frameId = *(uint16_t*)kFrameId;
	frameId = (frameId == 0xFFFE) ? 1 : (uint16_t)(frameId + 1);
	RenderSplitFrame(*(char**)((char*)self + 4), dt);
}

} // namespace

bool SplitActive() { return g_splitActive; }
long SplitFrames() { return g_splitFrames; }

bool SplitInstallHooks() {
	bool ok = PatchVtableSlot(kGraphicVtbl, kGraphicTickSlot, kGraphicTick,
	                          (void*)GraphicTickHook, (void**)&g_origGraphicTick, "CKGraphic::render");
	ok &= PatchVtableSlot(kDynSphereVtbl, kWalkSlot, kDynSphereWalk, (void*)DynSphereWalkHook,
	                      (void**)&g_origDynSphereWalk, "CKDynamicBoundingSphere::walk");
	CollectFn same = nullptr;
	ok &= PatchVtableSlot(kDynProjectileVtbl, kWalkSlot, kDynSphereWalk, (void*)DynSphereWalkHook,
	                      (void**)&same, "CKDynBSphereProjectile::walk");
	return ok;
}
