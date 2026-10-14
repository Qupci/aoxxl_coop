#include "splitscreen.h"
#include "coop.h"
#include "config.h"
#include "log.h"
#include "render.h"
#include "trio.h"
#include "p2cam.h"
#include "gamecam.h"
#include "hud2.h"

#include <cmath>
#include <intrin.h>
#include <cstdlib>
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

// --- particles ---------------------------------------------------------------
// CParticlesNodeFx::vf14 (0x41A510) steps its particle system by the frame time
// and adds the system's atomics to the draw lists (FUN_0041E390, which also does
// the frustum test) -- once per frame for most systems: it works only while the
// system's +4 bit 1 is set and clears it when done; others (the magic potion's
// aura on Asterix) use up the emission points the game hands them every frame
// (+0x40). A second step in the same frame left player 2's half without the
// helmets' sparkles, the potion's aura and the x10 bonuses' rays. Now the
// frame's first walk -- player 2's stamp walk, whose lists draw nothing -- steps
// every system that half sees, and each half's walk then only adds the same
// atomics to its own lists (the particles are in world space and face whichever
// camera draws them). A system not stepped yet this frame (out of player 2's
// view) is stepped by player 1's walk as usual.
const uintptr_t kParticlesVtbl = 0x622FCC;   // CParticlesNodeFx (11,19)
const uintptr_t kParticlesWalk = 0x41A510;
typedef char(__fastcall* AddToListsFn)(char* node, void* edx, void** lists);
const AddToListsFn AddToLists = (AddToListsFn)0x41E390;
CollectFn g_origParticlesWalk = nullptr;
bool      g_inSplitFrame = false;
bool      g_stampWalk = false;
int       g_walkPlayer = 0;   // 1 / 2: whose walk the split frame is in

// Systems stepped in this split frame. A system with +0x10 bit 9 is stepped
// whether or not it is in view (and stamps itself only when it is): the x10
// bonuses' yellow rays, stepped by player 2's stamp walk out of that view, were
// then skipped by player 1's walk -- only the red ones showed.
char*    g_stepped[2048];
uint32_t g_steppedGen[2048];
uint32_t g_splitGen = 0;

bool Stepped(char* node) {
	for (uint32_t h = ((uintptr_t)node >> 4) & 2047, k = 0; k < 2048; h = (h + 1) & 2047, k++) {
		if (g_steppedGen[h] != g_splitGen) return false;
		if (g_stepped[h] == node) return true;
	}
	return false;
}

void MarkStepped(char* node) {
	for (uint32_t h = ((uintptr_t)node >> 4) & 2047, k = 0; k < 2048; h = (h + 1) & 2047, k++) {
		if (g_steppedGen[h] != g_splitGen) { g_steppedGen[h] = g_splitGen; g_stepped[h] = node; return; }
		if (g_stepped[h] == node) return;
	}
}

void __fastcall ParticlesWalkHook(char* self, void* edx, void** lists, char* dt) {
	char* ps = *(char**)(self + 0x48);
	if (!g_inSplitFrame || !ps || (*(uint16_t*)(self + 0x10) & 2)) {
		g_origParticlesWalk(self, edx, lists, dt);
		return;
	}
	if (!Stepped(self)) {
		bool active = (*(uint32_t*)(ps + 4) & 2) != 0;
		static const bool plog = getenv("XXL_PARTLOG") != nullptr;   // diagnostics: systems stepped twice in a frame
		if (plog && active) {
			static char* who[256]; static uint32_t gen[256]; static int seenTwice[256];
			uint32_t h = ((uintptr_t)self >> 4) & 255;
			if (who[h] == self && gen[h] == g_splitGen && !seenTwice[h]) {
				seenTwice[h] = 1;
				Log("split: particle system %p (flags %04X, stamp %d walk %d) stepped again in the same frame", self,
				    *(uint16_t*)(self + 0x10), g_stampWalk, g_walkPlayer);
			}
			if (who[h] != self) seenTwice[h] = 0;
			who[h] = self; gen[h] = g_splitGen;
		}
		g_origParticlesWalk(self, edx, lists, dt);
		// Stepped: every bit-9 system, the others when their visibility test passed.
		if (active && ((*(uint16_t*)(self + 0x10) & 0x200) || *(uint16_t*)(self + 0x14) == *(uint16_t*)kFrameId))
			MarkStepped(self);
		return;
	}
	// Already stepped this frame: only into this walk's lists (with its frustum test).
	AddToLists(self, nullptr, lists);
	for (char* child = *(char**)(self + 0x18); child; child = *(char**)(child + 8))
		((CollectFn)(*(void***)child)[14])(child, nullptr, lists, dt);
}

// --- animation blends ------------------------------------------------------------
// CAnimatedNode / CAnimatedClone walks (slot 14: 0x41DA30, 0x41D270) end in
// FUN_0041DA70, which cross-fades a model from its previous animation into the
// current one (+0x10 bits 7-8: blend mode; +0x48 the blend's clock, +0x44 its
// length) by the frame time -- once per walk, and the bones' blend on top of the
// last one. Three walks per split frame made every blend three times as fast:
// heroes snapped between standing, walking and running. Only the frame's first
// walk of a node blends; the later ones run with the blend mode off (the pose
// from the first walk stays; bones, visibility and children as usual).
const uintptr_t kAnimNodeVtbl  = 0x623128, kAnimNodeWalk  = 0x41DA30;   // CAnimatedNode (11,21)
const uintptr_t kAnimCloneVtbl = 0x6230D4, kAnimCloneWalk = 0x41D270;   // CAnimatedClone (11,22)
CollectFn g_origAnimNodeWalk = nullptr, g_origAnimCloneWalk = nullptr;

void AnimWalk(CollectFn orig, char* self, void* edx, void** lists, void* dt) {
	static const bool off = getenv("XXL_ANIMFIX") && getenv("XXL_ANIMFIX")[0] == '0';   // diagnostics
	static const bool log = getenv("XXL_ANIMLOG") != nullptr;   // diagnostics: Asterix's blends, every walk
	if (log) {
		Trio t = GetTrio();
		Hero a = t ? t.GetHero(HERO_ASTERIX) : Hero();
		uint16_t f = *(uint16_t*)(self + 0x10);
		if (a && (char*)a.hook->node == self && (f & 0x180))
			Log("split: Asterix blend mode %d clock %.3f of %.3f (walk %d%s, dt %.4f)", (f & 0x180) >> 7, *(float*)(self + 0x48),
			    *(float*)(self + 0x44), g_walkPlayer, g_stampWalk ? " stamp" : "", *(float*)((char*)dt + 0xC));
	}
	if (!g_inSplitFrame || off) { orig(self, edx, lists, dt); return; }
	char* key = self + 1;   // own entries in the stepped table (particle systems use the node pointer)
	if (!Stepped(key)) {
		MarkStepped(key);
		orig(self, edx, lists, dt);
		return;
	}
	uint16_t& flags = *(uint16_t*)(self + 0x10);
	uint16_t blend = flags & 0x180;
	flags &= ~0x180;
	orig(self, edx, lists, dt);
	flags = (uint16_t)((flags & ~0x180) | blend);
}

void __fastcall AnimNodeWalkHook(char* self, void* edx, void** lists, void* dt) { AnimWalk(g_origAnimNodeWalk, self, edx, lists, dt); }
void __fastcall AnimCloneWalkHook(char* self, void* edx, void** lists, void* dt) { AnimWalk(g_origAnimCloneWalk, self, edx, lists, dt); }

// --- fog boxes (snow) ---------------------------------------------------------------
// CFogBoxNodeFx::vf14 (0x416D10) moves its particles by the frame time in its
// walk (snow in level 2 sector 1's camp): each particle's record (+0x360, 7
// floats: target, position, speed) and fade (+0x35C, 8 dwords), and the box's
// fade (+0x38C) -- but only the particle groups its camera sees (the others are
// gathered at their group's centre for that walk's mesh). Three walks per split
// frame moved the snow up to three times as fast. Now the stamp walk leaves it
// alone, player 1's walk moves what player 1 sees, player 2's what player 2
// sees, and whatever player 1's walk moved is put back to how that walk left it
// (each particle moves once a frame; in player 2's half the ones both see are
// drawn a frame ahead).
const uintptr_t kFogVtbl = 0x622F14;
const uintptr_t kFogWalk = 0x416D10;
CollectFn g_origFogWalk = nullptr;

struct FogState {
	char*    node = nullptr;
	uint32_t gen = 0;          // the split frame of s0 / s1
	unsigned n = 0, cap = 0;   // particles
	uint8_t* s0 = nullptr;     // before player 1's walk: records then fades
	uint8_t* s1 = nullptr;     // after it
	float    fade0 = 0, fade1 = 0;
};
FogState g_fog[16];

void __fastcall FogWalkHook(char* self, void* edx, void** lists, void* dt) {
	static const bool off = getenv("XXL_FOGFIX") && getenv("XXL_FOGFIX")[0] == '0';   // diagnostics
	static const bool log = getenv("XXL_FOGLOG") != nullptr;
	if (log && (g_walkPlayer == 1 || !g_inSplitFrame)) {   // once a frame: the box's mean particle speed
		static char* first = nullptr;
		static DWORD seen = 0, lastT = 0;
		static float prev[4096 * 3];
		static unsigned prevN = 0, k = 0;
		if (!first || GetTickCount() - seen > 2000) { first = self; prevN = 0; }
		unsigned n = *(unsigned*)(self + 0x34) * *(unsigned*)(self + 0x44);
		float* rec = *(float**)(self + 0x360);
		if (self == first && rec && n && n <= 4096) {
			seen = GetTickCount();
			if ((k++ % 30) == 0) {
				DWORD t = GetTickCount();
				if (prevN == n && t > lastT) {
					double sum = 0; unsigned moved = 0;
					for (unsigned i = 0; i < n; i++) {
						float dx = rec[i * 7 + 3] - prev[i * 3], dy = rec[i * 7 + 4] - prev[i * 3 + 1], dz = rec[i * 7 + 5] - prev[i * 3 + 2];
						float d = sqrtf(dx * dx + dy * dy + dz * dz);
						if (d > 0.0001f && d < 5.0f) { sum += d; moved++; }
					}
					Log("split: fog box %p: %u of %u particles moved, mean %.2f units/s (split %d)", self, moved, n,
					    moved ? sum / moved / ((t - lastT) / 1000.0) : 0.0, g_inSplitFrame);
				}
				for (unsigned i = 0; i < n; i++) { prev[i * 3] = rec[i * 7 + 3]; prev[i * 3 + 1] = rec[i * 7 + 4]; prev[i * 3 + 2] = rec[i * 7 + 5]; }
				prevN = n; lastT = t;
			}
		}
	}
	if (!g_inSplitFrame || off) { g_origFogWalk(self, edx, lists, dt); return; }
	if (g_stampWalk) return;
	unsigned n = *(unsigned*)(self + 0x34) * *(unsigned*)(self + 0x44);
	uint8_t* recs = *(uint8_t**)(self + 0x360);
	uint8_t* fades = *(uint8_t**)(self + 0x35C);
	FogState* st = nullptr;
	for (FogState& f : g_fog) if (f.node == self) st = &f;
	for (FogState& f : g_fog) if (!st && !f.node) { st = &f; st->node = self; }
	if (!st || !recs || !fades || n == 0 || n > 65536) { g_origFogWalk(self, edx, lists, dt); return; }
	const unsigned rs = 28, fs = 32, per = rs + fs;
	if (st->cap < n) {
		free(st->s0); free(st->s1);
		st->s0 = (uint8_t*)malloc((size_t)n * per);
		st->s1 = (uint8_t*)malloc((size_t)n * per);
		st->cap = st->s0 && st->s1 ? n : 0;
		if (!st->cap) { g_origFogWalk(self, edx, lists, dt); return; }
	}
	float& fade = *(float*)(self + 0x38C);
	if (g_walkPlayer == 1) {
		memcpy(st->s0, recs, (size_t)n * rs);
		memcpy(st->s0 + (size_t)n * rs, fades, (size_t)n * fs);
		st->fade0 = fade;
		g_origFogWalk(self, edx, lists, dt);
		memcpy(st->s1, recs, (size_t)n * rs);
		memcpy(st->s1 + (size_t)n * rs, fades, (size_t)n * fs);
		st->fade1 = fade;
		st->gen = g_splitGen;
		st->n = n;
		return;
	}
	g_origFogWalk(self, edx, lists, dt);
	if (g_walkPlayer != 2 || st->gen != g_splitGen || st->n != n) return;
	for (unsigned i = 0; i < n; i++) {
		uint8_t* r0 = st->s0 + (size_t)i * rs;
		uint8_t* r1 = st->s1 + (size_t)i * rs;
		uint8_t* f0 = st->s0 + (size_t)n * rs + (size_t)i * fs;
		uint8_t* f1 = st->s1 + (size_t)n * rs + (size_t)i * fs;
		if (memcmp(r0, r1, rs) != 0 || memcmp(f0, f1, fs) != 0) {
			memcpy(recs + (size_t)i * rs, r1, rs);
			memcpy(fades + (size_t)i * fs, f1, fs);
		}
	}
	if (st->fade0 != st->fade1) fade = st->fade1;
}

// --- sky -----------------------------------------------------------------------
// CSkyNodeFx::vf14 (0x41AF50) scrolls the sky's textures by how far the camera
// turned since the sky's last walk: it keeps the camera's last forward (x, z at
// +0x54/+0x58), right (+0x64/+0x68, with +0x10 bit 6 telling which axis),
// height (+0x6C) and the four scroll offsets (+0x4C/+0x50/+0x5C/+0x60), and also
// drifts them by the frame time. Walked once per player each split frame, every
// walk saw the turn between the two players' cameras: facing each other, half a
// turn per walk -- the sky spun. Each player now has their own copy of that state
// (the stamp walk leaves the sky alone).
const uintptr_t kSkyVtbl = 0x623020;   // CSkyNodeFx (11,25)
const uintptr_t kSkyWalk = 0x41AF50;
CollectFn g_origSkyWalk = nullptr;

struct SkyState { char* node; bool valid[2]; uint8_t flags[2]; uint8_t data[2][0x24]; };
SkyState g_sky[8];

void __fastcall SkyWalkHook(char* self, void* edx, void** lists, void* dt) {
	if (!g_inSplitFrame) {
		for (SkyState& st : g_sky) if (st.node == self) st.valid[0] = st.valid[1] = false;
		g_origSkyWalk(self, edx, lists, dt);
		return;
	}
	static const bool off = getenv("XXL_SKYFIX") && getenv("XXL_SKYFIX")[0] == '0';   // diagnostics: the game's own sky walks
	static const bool log = getenv("XXL_SKYLOG") != nullptr;
	static unsigned logN = 0;
	bool logThis = log && (logN++ % 90) < 9;
	if (off) {
		g_origSkyWalk(self, edx, lists, dt);
		if (logThis) {
			const float* f = (const float*)(self + 0x4C);
			Log("split: sky %p walk %d scroll %.3f %.3f %.3f %.3f", self, g_stampWalk ? 0 : g_walkPlayer, f[0], f[1], f[4], f[5]);
		}
		return;
	}
	if (g_stampWalk || g_walkPlayer < 1 || g_walkPlayer > 2) return;
	SkyState* st = nullptr;
	for (SkyState& s : g_sky) if (s.node == self) st = &s;
	for (SkyState& s : g_sky) if (!st && !s.node) { st = &s; st->node = self; st->valid[0] = st->valid[1] = false; }
	if (!st) { g_origSkyWalk(self, edx, lists, dt); return; }
	int k = g_walkPlayer - 1;
	if (st->valid[k]) {
		self[0x10] = (char)((self[0x10] & ~0x40) | (st->flags[k] & 0x40));
		memcpy(self + 0x4C, st->data[k], 0x24);
	}
	g_origSkyWalk(self, edx, lists, dt);
	st->flags[k] = (uint8_t)self[0x10];
	memcpy(st->data[k], self + 0x4C, 0x24);
	st->valid[k] = true;
	if (logThis) {
		const float* f = (const float*)(self + 0x4C);
		Log("split: sky %p walk %d scroll %.3f %.3f %.3f %.3f", self, g_walkPlayer, f[0], f[1], f[4], f[5]);
	}
	// The other player's copy starts from here (one jump at most, at split start).
	if (!st->valid[1 - k]) {
		st->flags[1 - k] = st->flags[k];
		memcpy(st->data[1 - k], st->data[k], 0x24);
		st->valid[1 - k] = true;
	}
}

// --- shadows -------------------------------------------------------------------
// The shadow service (CKSrvShadow, vtable 0x62520C, found in the yellow pages)
// builds one mesh of all shadow quads in its tick (slot 17, 0x470A70, ret 4):
// each shadow (slot 26 of its own vtable, 0x46F980, called from 0x470E19 /
// 0x470F0B) is culled and faded by its distance to the game camera (camera
// service +0x5B0) and nudged ~0.3 units towards it along the line of sight --
// which does not move it on screen for that camera. The game camera follows the
// leader: in the other half the non-leader's shadow slid towards the leader; a
// nudge straight up instead showed the shadow above the feet. In split screen the
// tick runs again before each half is drawn, with that half's camera position
// and no frame time (fades and smoothing advance once, in the game's own tick).
const uintptr_t kShadowSrvVtbl = 0x62520C;
typedef void(__fastcall* ShadowTickFn)(char* self, void* edx, void* time);
const ShadowTickFn ShadowTick = (ShadowTickFn)0x470A70;
const uintptr_t kShadowUpdate = 0x46F980;
const size_t    kShadowUpdateSlot = 26;   // of 0x62520C (the shadows' own vtable is 0x625268, slot 3)
typedef int(__fastcall* ShadowUpdateFn)(char* self, void* edx, void* a, void* b, void* c, void* d);
ShadowUpdateFn g_origShadowUpdate = nullptr;

int ShadowMode() {   // XXL_SHADOWFIX (diagnostics): 0 the game's own shadows, 1 (default) each half's
	static const int mode = getenv("XXL_SHADOWFIX") ? getenv("XXL_SHADOWFIX")[0] - '0' : 1;
	return mode;
}

char* ShadowService() {
	static char* cached = nullptr;
	static void* cachedYp = nullptr;
	CKYellowPages* yp = YellowPages();
	if (!yp) return nullptr;
	if (cachedYp == yp && cached && *(uintptr_t*)cached == kShadowSrvVtbl) return cached;
	cached = nullptr;
	cachedYp = yp;
	for (int off = 0x10; off < 0xC0 && !cached; off += 4) {
		char* o = *(char**)((char*)yp + off);
		if (o && !IsBadReadPtr(o, 4) && *(uintptr_t*)o == kShadowSrvVtbl) cached = o;
	}
	return cached;
}

// Rebuilds the shadow mesh for a camera at `eye` (before a half is drawn).
void ShadowsFor(const float eye[3], void* dt) {
	if (ShadowMode() == 0 || !dt) return;
	char* srv = ShadowService();
	CKYellowPages* yp = YellowPages();
	char* svc = yp ? *(char**)((char*)yp + 0x68) : nullptr;
	if (!srv || !svc) return;
	float* cam = (float*)(svc + 0x5B0);
	float saved[3] = { cam[0], cam[1], cam[2] };
	cam[0] = eye[0]; cam[1] = eye[1]; cam[2] = eye[2];
	uint8_t time[0x40];
	memcpy(time, dt, sizeof(time));
	*(float*)(time + 0xC) = 0.0f;
	ShadowTick(srv, nullptr, time);
	cam[0] = saved[0]; cam[1] = saved[1]; cam[2] = saved[2];
}

// Diagnostics (XXL_SHADOWLOG): the heroes' shadow offsets from their ground points.
int __fastcall ShadowUpdateHook(char* self, void* edx, void* a, void* b, void* c, void* d) {
	int r = g_origShadowUpdate(self, edx, a, b, c, d);
	static const bool log = getenv("XXL_SHADOWLOG") != nullptr;
	if (!log) return r;
	static unsigned n[2][3] = {};
	Trio t = GetTrio();
	char* node = *(char**)(self + 0xC);
	for (int i = 0; t && i < 2 && r == 0; i++) {
		Hero h = t.GetHero(i);
		if (!h || (char*)h.hook->node != node || (n[i][g_walkPlayer % 3]++ % 60) != 0) continue;
		const float* g = *(const float**)(self + 0x10);
		const float* o = (const float*)((char*)b + 0x30);
		Hero other = t.GetHero(1 - i);
		Vector3 to = other ? other.Pos() - h.Pos() : Vector3();
		Log("split: %s's shadow (%s, walk %d) offset (%.3f %.3f %.3f) from its ground point; the other hero is (%.1f %.1f) away",
		    HeroName(i), t.Leader() == i ? "leader" : "not leading", g_walkPlayer, o[0] - g[0], o[1] - g[1], o[2] - g[2], to.x, to.z);
	}
	return r;
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

// The HUD's own (2D) camera, renderer +0x14: while one player shops, the HUD --
// the merchant's words and menu with it -- is drawn into that player's half.
BeginUpdateFn g_origHudBegin = nullptr;
void*         g_wrappedHud = nullptr;
bool          g_hudVpActive = false;
Viewport      g_hudVp = {};

void* __cdecl HudBeginUpdateWrapper(void* camera) {
	void* r = g_origHudBegin(camera);
	if (r && g_hudVpActive && camera == g_wrappedHud) SetViewport(g_hudVp);
	return r;
}

void EnsureHudWrapped(RwCamera* cam) {
	if (!cam) return;
	if (cam == g_wrappedHud && cam->beginUpdate == (void*)HudBeginUpdateWrapper) return;
	g_origHudBegin = (BeginUpdateFn)cam->beginUpdate;
	g_wrappedHud = cam;
	cam->beginUpdate = (void*)HudBeginUpdateWrapper;
	Log("split: wrapped beginUpdate of the HUD camera %p (was %p)", cam, (void*)g_origHudBegin);
}

// Diagnostics (XXL_HUDLOG): the merchant's 2D screen -- the screen list (yellow
// pages +0x34 -> +0x3C: +4 first, +0x10 next, +0xC count), index at the shop
// interface (2/144) +0x20 -- and its 2D children, by vtable.
const char* Kind2d(char* p) {
	uintptr_t vt = p && !IsBadReadPtr(p, 4) ? *(uintptr_t*)p : 0;
	switch (vt) {
	case 0x6235C0: return "billboard";
	case 0x623710: return "billboard button";
	case 0x6237E4: return "billboard list";
	case 0x62390C: return "text button";
	case 0x623978: return "container";
	case 0x623A98: return "message box";
	case 0x623B3C: return "scene";
	case 0x623B6C: return "text";
	default: return nullptr;
	}
}
// A 2D element: +5 bit 2 shown; containers (and scenes) keep child lists at
// +0x24 / +0x28 (list +4 first, children linked through +0x10).
void Dump2d(char* p, int depth) {
	const char* kind = Kind2d(p);
	float* f = (float*)p;
	Log("split: shop 2d %*s%s %p vt %p %s | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f", depth * 2, "", kind ? kind : "?", p,
	    *(void**)p, (*(uint8_t*)(p + 5) & 2) ? "shown" : "hidden", f[5], f[6], f[7], f[8], f[13], f[14], f[15], f[16]);
	if (!kind || depth > 5) return;
	if (*(uintptr_t*)p == 0x623B3C) {   // a scene: +4 first child, +0xC count
		int k = 0;
		for (char* c = *(char**)(p + 4); c && k < 80 && Kind2d(c); c = *(char**)(c + 0x10), k++) Dump2d(c, depth + 1);
		return;
	}
	for (int o = 0x24; o <= 0x28; o += 4) {
		char* list = *(char**)(p + o);
		if (!list || IsBadReadPtr(list, 8)) continue;
		int k = 0;
		for (char* c = *(char**)(list + 4); c && k < 60 && Kind2d(c); c = *(char**)(c + 0x10), k++) Dump2d(c, depth + 1);
	}
}
// The shop's HUD at half size in the shopper's half (the HUD camera's vertical
// view window doubled: the HUD fills a band in the middle of the half), pushed
// out to the half's edges: every 2D element of the 2D managers (yellow pages
// +0x34 -> +0x34 / +0x38 / +0x3C) in the HUD's upper half moves up by half its
// height, every one in the lower half down, and a full-screen billboard (the
// merchant's letterbox) gets twice the height -- for the HUD draw only.
struct Moved { float* y; float y0; float* h; float h0; };
Moved g_moved[256];
int   g_nMoved = 0;
void SpreadTree(char* p, int depth) {
	const char* kind = Kind2d(p);
	if (!kind || depth > 6 || g_nMoved >= 256) return;
	uintptr_t vt = *(uintptr_t*)p;
	if (vt == 0x623B3C) {
		int k = 0;
		for (char* c = *(char**)(p + 4); c && k < 80 && Kind2d(c); c = *(char**)(c + 0x10), k++) SpreadTree(c, depth + 1);
		return;
	}
	if (vt == 0x623978) {
		for (int o = 0x24; o <= 0x28; o += 4) {
			char* list = *(char**)(p + o);
			if (!list || IsBadReadPtr(list, 8)) continue;
			int k = 0;
			for (char* c = *(char**)(list + 4); c && k < 60 && Kind2d(c); c = *(char**)(c + 0x10), k++) SpreadTree(c, depth + 1);
		}
		return;
	}
	if (!(*(uint8_t*)(p + 5) & 2)) return;   // not shown (+5 bit 2: shown, set by vf16, cleared by vf17)
	float* f = (float*)p;
	Moved& m = g_moved[g_nMoved++];
	m.y = &f[6]; m.y0 = f[6]; m.h = nullptr;
	// Full-width billboards: a full-screen one twice as high; the letterbox bars
	// (1.0 x 0.2, centred on the HUD's top and bottom edges) twice as high too,
	// so the half of each that stays on screen keeps single player's thickness.
	// 2D elements are placed by the HUD camera's view window only (its frame,
	// moved for the HUD's 3D life shields and helmet, does not move them): in the
	// half-size band (HUD y points up) what sits near the top moves up half a
	// HUD, what sits near the bottom down half a HUD, to the half's edges; the
	// middle -- the item shown on the merchant's board -- stays over the board.
	bool wide = vt == 0x6235C0 && f[13] >= 0.99f;
	if (wide && (f[14] >= 0.99f || f[6] <= 0.01f || f[6] >= 0.99f)) { m.h = &f[14]; m.h0 = f[14]; f[14] *= 2.0f; }
	if (f[6] > 0.75f) f[6] += 0.5f;
	else if (f[6] < 0.25f) f[6] -= 0.5f;
}
void SpreadShopHud() {
	g_nMoved = 0;
	CKYellowPages* yp = YellowPages();
	char* a = yp ? *(char**)((char*)yp + 0x34) : nullptr;
	for (int off = 0x34; a && off <= 0x3C; off += 4) {
		char* m = *(char**)(a + off);
		if (!m || IsBadReadPtr(m, 0x10)) continue;
		int k = 0;
		for (char* c = *(char**)(m + 4); c && k < 64 && Kind2d(c); c = *(char**)(c + 0x10), k++) SpreadTree(c, 0);
	}
}
void UnspreadShopHud() {
	for (int i = g_nMoved - 1; i >= 0; i--) {
		*g_moved[i].y = g_moved[i].y0;
		if (g_moved[i].h) *g_moved[i].h = g_moved[i].h0;
	}
	g_nMoved = 0;
}

int ShopHudMode() {
	static int mode = getenv("XXL_SHOPHUD") ? atoi(getenv("XXL_SHOPHUD")) : 3;   // 1 squeezed, 2 half size, 3 spread
	return mode;
}

void DumpShopScreen() {
	static DWORD last = 0;
	if (GetTickCount() - last < 4000) return;
	last = GetTickCount();
	CKYellowPages* yp = YellowPages();
	char* a = yp ? *(char**)((char*)yp + 0x34) : nullptr;
	char* mgr = a ? *(char**)(a + 0x3C) : nullptr;
	(void)mgr;
	for (int off = 0x04; a && off < 0x80; off += 4) {
		char* m = *(char**)(a + off);
		if (!m || IsBadReadPtr(m, 0x10)) continue;
		char* first = *(char**)(m + 4);
		if (!Kind2d(first)) continue;
		Log("split: shop 2d manager at yellow pages +0x34 -> +%02X: %p vt %p", off, m, *(void**)m);
		int k = 0;
		for (char* scr = first; scr && k < 64 && Kind2d(scr); scr = *(char**)(scr + 0x10), k++) Dump2d(scr, 0);
	}
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
int SplitDebug() {
	static int v = -1;
	if (v < 0) {
		char buf[16] = {};
		v = GetEnvironmentVariableA("XXL_SPLITDBG", buf, sizeof(buf)) ? atoi(buf) : 0;
		if (v) Log("split: debug mode %d", v);
	}
	return v;
}

void RenderSplitFrameImpl(char* view, void* dt);

void RenderSplitFrame(char* view, void* dt) {
	if (++g_splitGen == 0) g_splitGen = 1;
	g_inSplitFrame = true;
	RenderSplitFrameImpl(view, dt);
	g_inSplitFrame = false;
	g_repeatWalk = false;
}

void RenderSplitFrameImpl(char* view, void* dt) {
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

	if (getenv("XXL_PLANELOG")) {   // diagnostics: the game camera's clip and fog planes as they change
		static float ln = -1, lf = -1, lg = -1;
		if (cam->nearPlane != ln || cam->farPlane != lf || cam->fogPlane != lg) {
			ln = cam->nearPlane; lf = cam->farPlane; lg = cam->fogPlane;
			Log("split: game camera planes near %.2f far %.1f fog %.1f", ln, lf, lg);
		}
	}
	// The shop's HUD elements pushed out to the shopper's half's edges before
	// anything is walked (their draw data is built from their places on the way).
	if (CoopShopper() >= 0 && ShopHudMode() == 3) SpreadShopHud();
	// 1. Camera sync, exactly as the game does it, then remember the game's camera.
	SyncCameras(view, nullptr);

	// Clip and fog planes: the game camera's are the leader's; the other player's
	// half gets its own camera copy's (the merchant pushes the game camera's far /
	// fog planes out, which took the fog from the other half).
	const float livePlanes[3] = { cam->nearPlane, cam->farPlane, cam->fogPlane };
	auto usePlanes = [&](const float pl[3]) {
		if (cam->nearPlane != pl[0]) ((void*(__cdecl*)(RwCamera*, float))axr::kRwCameraSetNearClip)(cam, pl[0]);
		if (cam->farPlane != pl[1]) ((void*(__cdecl*)(RwCamera*, float))axr::kRwCameraSetFarClip)(cam, pl[1]);
		cam->fogPlane = pl[2];
	};
	auto planesFor = [&](int hero) {
		float pl[3];
		usePlanes(GameCamPlanes(hero, pl) ? pl : livePlanes);
	};

	SavedCamera p1cam;
	memcpy(p1cam.modelling, FrameModelling(cam), sizeof(p1cam.modelling));
	memcpy(p1cam.ltm, FrameLtm(cam), sizeof(p1cam.ltm));
	p1cam.window[0] = cam->viewWindow[0];
	p1cam.window[1] = cam->viewWindow[1];

	// Half-screen view window: keep the horizontal field of view for a stacked
	// split and the vertical one for a side-by-side split, so nothing is squashed;
	// then widen it by the ini's split_fov degrees (on top of whatever the game,
	// or a wide-FOV patch, set). Each player's from their own camera's window.
	auto halfWindow = [&](const float in[2], float out[2]) {
		out[0] = in[0];
		out[1] = in[1];
		if (vertical) out[0] *= (float)vp1.w / (float)w;
		else          out[1] *= (float)vp1.h / (float)h;
		float v = out[1];
		if (g_config.splitFov != 0.0f && v > 0.0f) {
			float a = atanf(v) + g_config.splitFov * 3.14159265f / 360.0f;
			if (a > 1.4f) a = 1.4f;
			if (a < 0.1f) a = 0.1f;
			float s = tanf(a) / v;
			out[0] *= s;
			out[1] *= s;
		}
	};
	float half[2], half1[2], half2[2], win[2];
	halfWindow(p1cam.window, half);
	if (P1CamWindow(win)) halfWindow(win, half1); else { half1[0] = half[0]; half1[1] = half[1]; }
	if (P2CamWindow(win)) halfWindow(win, half2); else { half2[0] = half[0]; half2[1] = half[1]; }
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
	if (SplitDebug() & 2) { own2 = false; d2 = Vector3(); }   // diagnostics: player 1's camera in both halves
	bool p2Leads = t && t.Leader() == CoopP2Hero() && g_config.gameCamera;
	auto placeP2 = [&] {
		RwCameraSetViewWindow(cam, half2);
		planesFor(CoopP2Hero());
		if (own2) {
			memcpy(FrameModelling(cam), m2, sizeof(m2));
			memcpy(FrameLtm(cam), m2, sizeof(m2));
		} else if (p2Leads) {
			// Player 2's hero leads: the game's camera is player 2's.
			memcpy(FrameModelling(cam), p1cam.modelling, sizeof(p1cam.modelling));
			memcpy(FrameLtm(cam), p1cam.ltm, sizeof(p1cam.ltm));
		} else {
			Translate(cam, p1cam, d2);
		}
	};
	// Player 1's half: the game camera follows the leader. When that is player
	// 2's hero, player 1 gets a follow camera of its own on its hero (also
	// while the game has borrowed it, e.g. Asterix riding the cable car).
	float m1[16];
	bool own1 = P1CamMatrix(m1);
	auto placeP1 = [&] {
		RwCameraSetViewWindow(cam, half1);
		planesFor(CoopP1Hero());
		if (own1) {
			memcpy(FrameModelling(cam), m1, sizeof(m1));
			memcpy(FrameLtm(cam), m1, sizeof(m1));
			return;
		}
		memcpy(FrameModelling(cam), p1cam.modelling, sizeof(p1cam.modelling));
		memcpy(FrameLtm(cam), p1cam.ltm, sizeof(p1cam.ltm));
		(void)d1;   // no shifting the game camera: it bobbed with the leader's jumps
	};

	// 2. Stamp-only walk for player 2. Being the frame's first walk, it is also
	//    the one that updates the swept collision spheres.
	g_repeatWalk = false;
	placeP2();
	SyncCameras(view, nullptr);
	LogFrustum("p2 stamp", cam);
	g_stampWalk = true;
	if (!(SplitDebug() & 1)) collect(root, nullptr, stampLists, dt);
	g_stampWalk = false;
	g_repeatWalk = !(SplitDebug() & 1);

	// 3. Player 1's view.
	placeP1();
	SyncCameras(view, nullptr);
	LogFrustum("p1 walk", cam);
	g_walkPlayer = 1;
	collect(root, nullptr, (void**)(view + 0x1C), dt);
	g_walkPlayer = 0;
	SyncClones(*(void**)(view + 0x58), nullptr, dt);
	ResetDirty(view, nullptr);
	if (!(viewFlags & 0x10)) ClearCamera(renderer, nullptr, 0);
	g_walkPlayer = 1;
	ShadowsFor(FrameLtm(cam) + 12, dt);
	g_walkPlayer = 0;
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
	g_walkPlayer = 2;
	collect(root, nullptr, lists, dt);
	g_walkPlayer = 0;
	g_repeatWalk = false;
	ResetDirty(view, nullptr);
	g_walkPlayer = 2;
	ShadowsFor(FrameLtm(cam) + 12, dt);
	g_walkPlayer = 0;
	g_vp = vp2; g_vpActive = true;
	DrawWorld(view, nullptr, dt);
	DrawEffects(view, nullptr, dt);
	g_vpActive = false;

	// 5. Back to the game's own camera, so everything after the render (sound,
	//    game logic, next frame's camera service) sees what it expects.
	RwCameraSetViewWindow(cam, p1cam.window);
	usePlanes(livePlanes);
	memcpy(FrameModelling(cam), p1cam.modelling, sizeof(p1cam.modelling));
	memcpy(FrameLtm(cam), p1cam.ltm, sizeof(p1cam.ltm));
	SyncCameras(view, nullptr);

	// Divider, then the HUD over the whole screen, then present.
	SetViewport({ 0, 0, w, h, 0.0f, 1.0f });
	if (vertical) ClearRect(vp1.w, 0, vp2.x - vp1.w, h, 0xFF000000);
	else          ClearRect(0, vp1.h, w, vp2.y - vp1.h, 0xFF000000);
	int shopper = CoopShopper();
	RwCamera* hud = *(RwCamera**)(renderer + 0x14);
	float hudWindow[2] = {};
	float hudOffset[3] = {};
	float hudModel[3] = {};
	bool movedOffset = false;
	if (shopper >= 0 && hud) {
		EnsureHudWrapped(hud);
		g_hudVp = shopper == CoopP1Hero() ? vp1 : vp2;
		g_hudVpActive = true;
		const int mode = ShopHudMode();
		hudWindow[0] = hud->viewWindow[0];
		hudWindow[1] = hud->viewWindow[1];
		static bool logged = false;
		if (!logged) {
			logged = true;
			Log("split: shop HUD in %s's half (HUD camera %p, view window %.3f %.3f, mode %d)", HeroName(shopper), hud,
			    hudWindow[0], hudWindow[1], mode);
		}
		if (mode >= 2) {   // half size, same scale both ways
			float wnd[2] = { hudWindow[0], hudWindow[1] * 2.0f };
			RwCameraSetViewWindow(hud, wnd);
		}
		if (mode == 3) {   // ... the HUD's 3D models (life shields, helmet) to the half's top: the camera's frame moved down
			static float dy = getenv("XXL_HUDOFF") ? (float)atof(getenv("XXL_HUDOFF")) : -0.5f;
			float* lt = FrameLtm(hud);
			float* md = FrameModelling(hud);
			memcpy(hudOffset, lt + 12, sizeof(hudOffset));
			memcpy(hudModel, md + 12, sizeof(hudModel));
			for (int i = 0; i < 3; i++) { lt[12 + i] += dy * lt[4 + i]; md[12 + i] += dy * lt[4 + i]; }
			movedOffset = true;
		}
	}
	if (shopper >= 0 && getenv("XXL_HUDLOG")) DumpShopScreen();
	if (false) {
		static DWORD last = 0;
		if (GetTickCount() - last > 3000) {
			last = GetTickCount();
			for (int l = kFirstHud; l < kNumLists; l++) {
				int k = 0;
				for (char* it = *(char**)(view + 0x1C + 4 * l); it && k < 64; it = *(char**)(it + 0x24), k++) {
					float* f = (float*)it;
					Log("split: HUD list %d item %p vt %p: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %08X %08X", l, it,
					    *(void**)it, f[5], f[6], f[7], f[8], f[13], f[14], f[15], f[16], *(uint32_t*)(it + 0x10),
					    *(uint32_t*)(it + 0x3C));
				}
			}
		}
	}
	if (getenv("XXL_HUDPOS") && hud) {   // diagnostics: where the HUD's pieces are
		static DWORD last = 0;
		char* itf = (char*)GetLvlObject(2, 132, 0);
		if (itf && GetTickCount() - last > 2000) {
			last = GetTickCount();
			float* cl = FrameLtm(hud);
			Log("hudpos: camera proj %u window %.3f %.3f offset %.3f %.3f near %.3f far %.3f right %.3f %.3f %.3f up %.3f %.3f %.3f at %.3f %.3f %.3f pos %.3f %.3f %.3f",
			    hud->projectionType, hud->viewWindow[0], hud->viewWindow[1], hud->viewOffset[0], hud->viewOffset[1],
			    hud->nearPlane, hud->farPlane, cl[0], cl[1], cl[2], cl[4], cl[5], cl[6], cl[8], cl[9], cl[10], cl[12], cl[13], cl[14]);
			const int offs[] = { 0x2C, 0x30, 0x34, 0x38, 0x3C, 0x40, 0x54, 0x68, 0x7C, 0x90, 0xA4, 0xA8, 0xAC, 0xB0, 0xB4, 0xB8, 0xBC, 0xC0, 0xC4, 0xCC };
			for (int o : offs) {
				char* n = *(char**)(itf + o);
				char* fr = n ? *(char**)(n + 4) : nullptr;
				if (!fr) continue;
				float* l = (float*)(fr + 0x50);
				float d[3] = { l[12] - cl[12], l[13] - cl[13], l[14] - cl[14] };
				Log("hudpos: +%02X vt %p flags %04X cam xyz %.3f %.3f %.3f (scale %.3f)", o, *(void**)n, *(uint16_t*)(n + 0x10),
				    d[0] * cl[0] + d[1] * cl[1] + d[2] * cl[2], d[0] * cl[4] + d[1] * cl[5] + d[2] * cl[6],
				    d[0] * cl[8] + d[1] * cl[9] + d[2] * cl[10], sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]));
			}
			const int o2[] = { 0x134, 0x150, 0x154, 0x158 };
			for (int o : o2) {
				float* e = *(float**)(itf + o);
				if (e) Log("hudpos: 2d +%03X flags %08X at %.3f %.3f size %.3f %.3f", o, ((uint32_t*)e)[1], e[5], e[6], e[13], e[14]);
			}
		}
	}
	Hud2BeginP1(hud);
	DrawHud(view, nullptr, dt);
	UnspreadShopHud();
	Hud2EndP1();
	for (int part = 0; part < 2; part++) if (Hud2BeginP2(hud, part)) {
		// Individual heroes: player 2's health and combo bar, a HUD pass of its
		// own -- the HUD lists collected again (no stepping: a repeat walk),
		// drawn as FUN_00414070 draws them, without the 2D layer.
		void* hudLists[kNumLists];
		for (int i = 0; i < kNumLists; i++)
			hudLists[i] = i >= kFirstHud ? *(void**)(view + 0x1C + 4 * i) : (void*)&g_dummy;
		g_repeatWalk = true;
		collect(root, nullptr, hudLists, dt);
		g_repeatWalk = false;
		ClearCamera(renderer, nullptr, 1);
		typedef int(__cdecl* CamFn)(void* cam);
		if (((CamFn)0x58FD30)(hud)) {
			((ThisFn)0x414210)(view, nullptr);
			for (int i = 0; i < 3; i++) {
				char* list = *(char**)(view + 0x4C + 4 * i);
				((ThisFn)(*(void***)list)[3])(list, nullptr);
			}
			((CamFn)0x58FD20)(hud);
		}
		Hud2EndP2();
	}
	if (g_hudVpActive) {
		g_hudVpActive = false;
		if (hud->viewWindow[1] != hudWindow[1]) RwCameraSetViewWindow(hud, hudWindow);
		if (movedOffset) {
			memcpy(FrameLtm(hud) + 12, hudOffset, sizeof(hudOffset));
			memcpy(FrameModelling(hud) + 12, hudModel, sizeof(hudModel));
		}
	}
	Present(renderer, nullptr);
	*(uint32_t*)(view + 0x68) &= 0xFFFFFFC1;
	InterlockedIncrement(&g_splitFrames);
}

bool WantSplit() {
	return g_config.splitScreen && CoopP2Joined() && !CoopSuspended() && !CoopOneScreen();
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
	                          (void*)GraphicTickHook, (void**)&g_origGraphicTick, "CKGraphic::render");	ok &= PatchVtableSlot(kDynSphereVtbl, kWalkSlot, kDynSphereWalk, (void*)DynSphereWalkHook,
	                      (void**)&g_origDynSphereWalk, "CKDynamicBoundingSphere::walk");
	ok &= PatchVtableSlot(kParticlesVtbl, kWalkSlot, kParticlesWalk, (void*)ParticlesWalkHook,
	                      (void**)&g_origParticlesWalk, "CParticlesNodeFx::walk");
	ok &= PatchVtableSlot(kSkyVtbl, kWalkSlot, kSkyWalk, (void*)SkyWalkHook, (void**)&g_origSkyWalk, "CSkyNodeFx::walk");
	ok &= PatchVtableSlot(kShadowSrvVtbl, kShadowUpdateSlot, kShadowUpdate, (void*)ShadowUpdateHook,
	                      (void**)&g_origShadowUpdate, "CKSrvShadow::update");
	ok &= PatchVtableSlot(kAnimNodeVtbl, kWalkSlot, kAnimNodeWalk, (void*)AnimNodeWalkHook, (void**)&g_origAnimNodeWalk, "CAnimatedNode::walk");
	ok &= PatchVtableSlot(kAnimCloneVtbl, kWalkSlot, kAnimCloneWalk, (void*)AnimCloneWalkHook, (void**)&g_origAnimCloneWalk, "CAnimatedClone::walk");
	ok &= PatchVtableSlot(kFogVtbl, kWalkSlot, kFogWalk, (void*)FogWalkHook, (void**)&g_origFogWalk, "CFogBoxNodeFx::walk");
	CollectFn same = nullptr;
	ok &= PatchVtableSlot(kDynProjectileVtbl, kWalkSlot, kDynSphereWalk, (void*)DynSphereWalkHook,
	                      (void**)&same, "CKDynBSphereProjectile::walk");
	return ok;
}
