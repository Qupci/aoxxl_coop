#include "gamecam.h"
#include "coop.h"
#include "config.h"
#include "detour.h"
#include "inputmgr.h"
#include "log.h"
#include "proximity.h"
#include "render.h"
#include "trio.h"

#include <intrin.h>
#include <cmath>
#include <cstring>

namespace {

const uintptr_t kSrvCameraVtbl = 0x624758;
const size_t    kTickSlot      = 17;
const uintptr_t kTick          = 0x454620;   // CKSrvCamera::vf17(frame), thiscall, ret 4
const size_t    kSrvSize       = 0x6A0;      // allocation size (class registration)

// CKSrvCamera::SetTarget(target, transition seconds, cut) -- thiscall, ret 0xC.
// The trio's switch follow-up (CKGrpTrio_SwitchLeaderFollowup) aims the camera
// at the new leader with it.
const uintptr_t kSetTarget = 0x459050;
const uint8_t   kSetTargetPrologue[7] = { 0xF6, 0x41, 0x0C, 0x40, 0x8B, 0x41, 0x0C };
const uintptr_t kSwitchFollowup    = 0x481880;
const uintptr_t kSwitchFollowupEnd = 0x48194A;

typedef void(__fastcall* TickFn)(char* svc, void* edx, void* frame);
typedef void(__fastcall* SetTargetFn)(char* svc, void* edx, void* target, float time, char cut);
typedef void(__fastcall* ThisFn)(void* self, void* edx);
typedef void*(__cdecl* SetViewWindowFn)(void* camera, float* window);
typedef void*(__cdecl* SetClipFn)(void* camera, float v);
TickFn      g_origTick = nullptr;
SetTargetFn g_origSetTarget = nullptr;

// Camera tracks by vtable, with their allocation sizes.
struct TrackKind { uintptr_t vt; uint32_t size; };
const TrackKind kTrackKinds[] = {
	{ 0x6243B0, 0x88 },  { 0x624418, 0xDC }, { 0x624484, 0x80 }, { 0x6244EC, 0x70 },   // rigid, classic, path, fix
	{ 0x624554, 0x80 },  { 0x6245BC, 0x6C }, { 0x624624, 0x94 }, { 0x624690, 0x11C },  // axis, plain, passive path, spy
};
uint32_t TrackSize(const char* t) {
	if (!t) return 0;
	uintptr_t vt = *(const uintptr_t*)t;
	for (const TrackKind& k : kTrackKinds) if (k.vt == vt) return k.size;
	return 0;
}

const uint32_t kMaxTrackBytes = 0x120;
struct TrackBytes { char* track; uint32_t size; uint8_t b[kMaxTrackBytes]; };

struct CamCopy {
	bool       valid = false;
	bool       cut = false;      // aim it at its hero with a cut on its next tick
	int        hero = -1;
	uint8_t    svc[kSrvSize];
	TrackBytes tracks[3];        // the tracks it uses (+0x34 next, +0x38 current, +0x3C previous)
	int        numTracks = 0;
	bool       haveView = false;
	float      planes[3] = {};   // the copy's near, far and fog planes after its tick
	float      m[16];
	float      window[2];
};
CamCopy g_copy;
CamCopy g_swap;                  // scratch for the exchange
int     g_liveOwner = -1;        // the hero the game's own camera state belongs to
char*   g_svc = nullptr;
bool    g_inCopy = false;
bool    g_active = false;
uint8_t g_liveSvc[kSrvSize];
uint8_t g_zeros[256];

const int  kMaxTracks = 256;
TrackBytes g_snap[kMaxTracks];
int        g_numSnap = 0;

void AddSnap(char* t) {
	uint32_t sz = TrackSize(t);
	if (!sz || g_numSnap >= kMaxTracks) return;
	for (int i = 0; i < g_numSnap; i++) if (g_snap[i].track == t) return;
	TrackBytes& s = g_snap[g_numSnap++];
	s.track = t;
	s.size = sz;
	memcpy(s.b, t, sz);
}

// Every track the service knows (list +0x20, linked at +4) and the ones either
// state points at.
void SnapshotTracks(char* svc) {
	g_numSnap = 0;
	int n = 0;
	for (char* t = *(char**)(svc + 0x20); t && n < kMaxTracks; t = *(char**)(t + 4), n++) AddSnap(t);
	for (int off = 0x34; off <= 0x3C; off += 4) AddSnap(*(char**)(svc + off));
	for (int i = 0; i < g_copy.numTracks; i++) AddSnap(g_copy.tracks[i].track);
	n = 0;
	for (char* sec = *(char**)(svc + 0x24); sec && n < kMaxTracks; sec = *(char**)(sec + 4), n++)
		AddSnap(*(char**)(sec + 0x20));
}
// A track of this service: in its list (+0x20), a camera sector's (list +0x24,
// track +0x20 -- levels made with the XXL Editor have sector tracks missing
// from the list) or its default one (+0x10).
bool TrackKnown(char* svc, const char* t) {
	if (t == *(char**)(svc + 0x10)) return true;
	int n = 0;
	for (char* tr = *(char**)(svc + 0x20); tr && n < kMaxTracks; tr = *(char**)(tr + 4), n++)
		if (tr == t) return true;
	n = 0;
	for (char* sec = *(char**)(svc + 0x24); sec && n < kMaxTracks; sec = *(char**)(sec + 4), n++)
		if (*(char**)(sec + 0x20) == t) return true;
	return false;
}
void RestoreTracks() {
	for (int i = 0; i < g_numSnap; i++) memcpy(g_snap[i].track, g_snap[i].b, g_snap[i].size);
}
void SaveRefs(const char* svc, CamCopy& c) {
	c.numTracks = 0;
	for (int off = 0x34; off <= 0x3C; off += 4) {
		char* t = *(char**)(svc + off);
		uint32_t sz = TrackSize(t);
		if (!sz) continue;
		bool dup = false;
		for (int i = 0; i < c.numTracks; i++) dup |= c.tracks[i].track == t;
		if (dup) continue;
		TrackBytes& tb = c.tracks[c.numTracks++];
		tb.track = t;
		tb.size = sz;
		memcpy(tb.b, t, sz);
	}
}
void LoadRefs(const CamCopy& c) {
	for (int i = 0; i < c.numTracks; i++) memcpy(c.tracks[i].track, c.tracks[i].b, c.tracks[i].size);
}

// What the tick writes outside the service: the camera's frame, its view window
// and clip planes, and two nodes it places (+0x5D0, +0x5D4).
struct Extras {
	float modelling[16];
	float window[2];
	float nearP, farP, fogP;
	float node[2][3];
	float fov;   // the renderer's (svc +0x18) last field of view, +0x28
};
// The service's camera object (+0x1C) has the frame the tick places; the view
// window and clip planes are set on the main RwCamera (CalcProjCamera,
// FUN_00414B10 on the renderer +0x10). CalcProjCamera (0x414B70) keeps the
// field of view it last set (renderer +0x28) and sets nothing when asked for
// the same one: with the other player's tick's value left there, Asterix's
// mole combo (a wider field of view, then back) found "back" already set and
// both halves stayed wide. It goes back with the window.
float* RendererFov(char* svc) {
	char* r = *(char**)(svc + 0x18);
	return r ? (float*)(r + 0x28) : nullptr;
}
float* Modelling(RwCamera* cam) { return (float*)(*(char**)((char*)cam + 4) + 0x10); }
float* NodePos(char* svc, int off) {
	char* node = *(char**)(svc + off);
	char* fr = node ? *(char**)(node + 4) : nullptr;
	return fr ? (float*)(fr + 0x40) : nullptr;
}
void SaveExtras(char* svc, RwCamera* obj, Extras& e) {
	memcpy(e.modelling, Modelling(obj), sizeof(e.modelling));
	RwCamera* cam = MainCamera();
	e.window[0] = cam->viewWindow[0];
	e.window[1] = cam->viewWindow[1];
	e.nearP = cam->nearPlane;
	e.farP = cam->farPlane;
	e.fogP = cam->fogPlane;
	for (int i = 0; i < 2; i++) {
		float* p = NodePos(svc, 0x5D0 + 4 * i);
		if (p) memcpy(e.node[i], p, sizeof(e.node[i]));
	}
	if (float* f = RendererFov(svc)) {
		e.fov = *f;
		static float lastFov = 0.0f;
		static int n = 0;
		if (fabsf(e.fov - lastFov) > 1.0f && n < 40) {   // the game's field of view (e.g. the mole combo's)
			n++;
			Log("gamecam: the game's field of view %.1f -> %.1f (window %.3f %.3f)", lastFov, e.fov, e.window[0], e.window[1]);
			lastFov = e.fov;
		}
	}
}
void RestoreExtras(char* svc, RwCamera* obj, Extras& e) {
	memcpy(Modelling(obj), e.modelling, sizeof(e.modelling));
	RwCamera* cam = MainCamera();
	if (cam->viewWindow[0] != e.window[0] || cam->viewWindow[1] != e.window[1])
		((SetViewWindowFn)axr::kRwCameraSetViewWindow)(cam, e.window);
	if (cam->nearPlane != e.nearP) ((SetClipFn)axr::kRwCameraSetNearClip)(cam, e.nearP);
	if (cam->farPlane != e.farP) ((SetClipFn)axr::kRwCameraSetFarClip)(cam, e.farP);
	cam->fogPlane = e.fogP;
	if (float* f = RendererFov(svc)) *f = e.fov;
	for (int i = 0; i < 2; i++) {
		float* p = NodePos(svc, 0x5D0 + 4 * i);
		if (p) memcpy(p, e.node[i], sizeof(e.node[i]));
	}
}

void* TargetOf(int hero) {
	Trio t = GetTrio();
	Hero h = t ? t.GetHero(hero) : Hero();
	return h ? *(void**)(h.raw() + 0x10) : nullptr;
}
int HeroOfTarget(void* target) {
	for (int h = 0; h < 2; h++) if (target && TargetOf(h) == target) return h;
	return -1;
}

// The camera reads CKInput's values through its own input view (+0x580). For
// player 2's camera those are player 2's.
struct InputSwap { uint8_t** slot = nullptr; uint8_t* saved = nullptr; };
void UseInputOf(int hero, InputSwap& s) {
	if (hero != CoopP2Hero()) return;   // player 1's: the game's own values
	InputManager im = GetInputManager();
	if (!im) return;
	uint8_t* v = CoopP2CameraValues();
	s.slot = (uint8_t**)(im.raw() + 0x1C);
	s.saved = *s.slot;
	*s.slot = v ? v : g_zeros;
}
void Unswap(InputSwap& s) {
	if (s.slot) *s.slot = s.saved;
}

// Fight camera (service +0xC bits 28-31: the follow camera pulls back while a
// fight is on). The game sets it for the trio's one fight; each player's camera
// gets it for its own hero's fight.
void ApplyFight(char* svc, int hero) {
	uint32_t& f = *(uint32_t*)(svc + 0xC);
	bool want = ProximityHeroFighting(hero);
	bool on = (f >> 30) != 0;
	if (want != on) Log("gamecam: fight camera %s for %s's camera", want ? "on" : "off", HeroName(hero));
	if (want && !on) {
		f = (f & 0x0FFFFFFFu) | 0x80000000u;
	} else if (!want && on) {
		f &= 0x0FFFFFFFu;
		char* tr = *(char**)(svc + 0x38);
		if (tr) ((ThisFn)(*(void***)tr)[24])(tr, nullptr);
	}
}

// The game's camera goes to `to`: if the copy is that hero's, the two states
// change places (true); otherwise the game's state is kept as the copy of the
// hero it was following, and the game's own aim at `to` goes ahead (false).
bool HandOver(char* svc, int to, const char* why) {
	int from = g_liveOwner;
	g_liveOwner = to;
	if (from < 0 || from == to) return false;
	if (g_copy.valid && g_copy.hero == to) {
		SnapshotTracks(svc);
		memcpy(g_swap.svc, svc, kSrvSize);
		SaveRefs(svc, g_swap);
		memcpy(svc, g_copy.svc, kSrvSize);
		LoadRefs(g_copy);
		memcpy(g_copy.svc, g_swap.svc, kSrvSize);
		memcpy(g_copy.tracks, g_swap.tracks, sizeof(g_copy.tracks));
		g_copy.numTracks = g_swap.numTracks;
		g_copy.hero = from;
		g_copy.cut = false;
		g_copy.haveView = false;
		Log("gamecam: %s: the game's camera is %s's own again, %s's goes on as the copy", why, HeroName(to),
		    HeroName(from));
		return true;
	}
	memcpy(g_copy.svc, svc, kSrvSize);
	SaveRefs(svc, g_copy);
	g_copy.hero = from;
	g_copy.valid = true;
	g_copy.cut = false;
	g_copy.haveView = false;
	Log("gamecam: %s: the game's camera goes to %s, %s keeps its camera as the copy", why, HeroName(to), HeroName(from));
	return false;
}

bool RunTick(char* svc, void* frame) {
	__try {
		g_origTick(svc, nullptr, frame);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

void TickCopy(char* svc, void* frame, int hero) {
	RwCamera* cam = *(RwCamera**)(svc + 0x1C);
	if (!cam || !*(char**)((char*)cam + 4) || !MainCamera()) return;
	if (!g_copy.valid || g_copy.hero != hero) {
		memcpy(g_copy.svc, svc, kSrvSize);
		SaveRefs(svc, g_copy);
		g_copy.hero = hero;
		g_copy.valid = true;
		g_copy.cut = true;
		g_copy.haveView = false;
		Log("gamecam: %s gets a copy of the game's camera", HeroName(hero));
	}
	// The game's state aside...
	static Extras live;
	memcpy(g_liveSvc, svc, kSrvSize);
	SnapshotTracks(svc);
	SaveExtras(svc, cam, live);
	// ...the copy's in, one tick of the game's own code...
	memcpy(svc, g_copy.svc, kSrvSize);
	LoadRefs(g_copy);
	g_inCopy = true;
	InputSwap s;
	UseInputOf(hero, s);
	ApplyFight(svc, hero);
	if (g_copy.cut) {
		g_copy.cut = false;
		void* target = TargetOf(hero);
		if (target) g_origSetTarget(svc, nullptr, target, 0.0f, 1);
	}
	bool ok = RunTick(svc, frame);
	Unswap(s);
	g_inCopy = false;
	if (ok) {
		static char* lastTrack[2];
		char* tr = *(char**)(svc + 0x38);
		if (tr != lastTrack[1]) {
			lastTrack[1] = tr;
			Log("gamecam: %s's camera (copy) now uses track %p (vt %08X), sector %p", HeroName(hero), tr,
			    tr ? (unsigned)*(uintptr_t*)tr : 0, *(void**)(svc + 0x4EC));
		}
		char* lt = *(char**)(g_liveSvc + 0x38);
		if (lt != lastTrack[0]) {
			lastTrack[0] = lt;
			Log("gamecam: the game's camera (%s) now uses track %p (vt %08X), sector %p", HeroName(g_liveOwner), lt,
			    lt ? (unsigned)*(uintptr_t*)lt : 0, *(void**)(g_liveSvc + 0x4EC));
		}
		memcpy(g_copy.m, Modelling(cam), sizeof(g_copy.m));
		g_copy.window[0] = MainCamera()->viewWindow[0];
		g_copy.window[1] = MainCamera()->viewWindow[1];
		// Its clip and fog planes too: the merchant's camera pushes the game
		// camera's far / fog planes out (60 / 48 -> 100 / 80), which took the
		// fog from the other player's half.
		g_copy.planes[0] = MainCamera()->nearPlane;
		g_copy.planes[1] = MainCamera()->farPlane;
		g_copy.planes[2] = MainCamera()->fogPlane;
		bool finite = true;
		for (int i = 0; i < 16; i++) finite &= i == 3 || std::isfinite(g_copy.m[i]);
		finite &= g_copy.window[0] > 0.01f && g_copy.window[1] > 0.01f && g_copy.window[0] < 100.0f && g_copy.window[1] < 100.0f;
		static int logs = 0;
		if (logs < 6 || !finite) {
			logs++;
			const float* m = g_copy.m;
			Log("gamecam: copy view %s: right (%.2f %.2f %.2f) up (%.2f %.2f %.2f) at (%.2f %.2f %.2f) pos (%.2f %.2f %.2f) "
			    "window %.3f %.3f; svc+1C %p frame %p, main camera %p frame %p",
			    finite ? "ok" : "NOT FINITE", m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14],
			    g_copy.window[0], g_copy.window[1], cam, *(void**)((char*)cam + 4), MainCamera(),
			    *(void**)((char*)MainCamera() + 4));
		}
		g_copy.haveView = finite;
		memcpy(g_copy.svc, svc, kSrvSize);
		SaveRefs(svc, g_copy);
	} else {
		g_copy.valid = false;
		g_copy.haveView = false;
		g_copy.numTracks = 0;
		Log("gamecam: the copy's tick failed (exception); starting over from the game's camera");
	}
	// ...and the game's state back.
	memcpy(svc, g_liveSvc, kSrvSize);
	RestoreTracks();
	RestoreExtras(svc, cam, live);
}

// A level loaded again (the same level too: the camera service stays, its
// tracks and the trio are new): the copy's saved tracks belong to the old one
// -- dropped (true).
void* g_copyTrio = nullptr;
bool CopyStale(char* svc, void* trio) {
	bool stale = svc != g_svc || trio != g_copyTrio;
	if (!stale && g_copy.valid)
		for (int i = 0; i < g_copy.numTracks && !stale; i++) stale = !TrackKnown(svc, g_copy.tracks[i].track);
	if (!stale) return false;
	if (g_copy.valid && svc == g_svc) {
		static int s_logged = 0;
		if (s_logged++ < 20) Log("gamecam: the level was loaded again -- the copy starts over from the game's camera");
	}
	g_copy.valid = false;
	g_copy.haveView = false;
	g_copy.numTracks = 0;
	return true;
}

// Camera shake (the LV3-6 boss rolling on, explosions...): a request sets +0xC
// bit 0x1000 and the shake's length, strength, frequency and fade at
// +0x668..+0x67C, and its tick counter +0x680 to 0 -- the tick counts it up.
// Requests made outside a tick land in the game's state only: a fresh one (bit
// on, count 0) goes to the copy too -- unless the leading hero's own tick made
// it (the Twister): a hero's shake is its player's, as the non-leader's lands
// in its copy only.
bool g_shakeOwn = false;
void ShareShake(char* svc) {
	bool own = g_shakeOwn;
	g_shakeOwn = false;
	static const bool trace = getenv("XXL_SHAKELOG") != nullptr;
	if (trace) {
		static uint32_t s_last[2] = { ~0u, ~0u };
		const char* st[2] = { svc, (const char*)g_copy.svc };
		for (int i = 0; i < 2; i++) {
			uint32_t on = (*(const uint32_t*)(st[i] + 0xC) & 0x1000) ? 1 : 0;
			uint32_t key = on | *(const uint32_t*)(st[i] + 0x680) << 1;
			if (key == s_last[i] || (!on && !(s_last[i] & 1))) { s_last[i] = key; continue; }
			s_last[i] = key;
			Log("gamecam: shake %s: on %u length %.2f elapsed %.2f strength %.2f count %u%s", i ? "copy" : "game", on,
			    *(const float*)(st[i] + 0x668), *(const float*)(st[i] + 0x66C), *(const float*)(st[i] + 0x670),
			    *(const uint32_t*)(st[i] + 0x680), i == 0 && own ? " (the leading hero's own)" : "");
		}
	}
	if (own || !g_copy.valid || !(*(uint32_t*)(svc + 0xC) & 0x1000) || *(uint32_t*)(svc + 0x680)) return;
	*(uint32_t*)(g_copy.svc + 0xC) |= 0x1000;
	memcpy(g_copy.svc + 0x668, svc + 0x668, 0x680 + 4 - 0x668);
	static int s_logged = 0;
	if (s_logged++ < 20)
		Log("gamecam: camera shake for both cameras (%.2f s, strength %.2f)", *(float*)(svc + 0x668),
		    *(float*)(svc + 0x670));
}

bool Wanted() {
	return g_config.gameCamera && g_config.splitScreen && CoopP2Joined() && !CoopSuspended() && !CoopOneScreen();
}

void __fastcall TickHook(char* svc, void* edx, void* frame) {
	Trio t = GetTrio();
	int other = CoopNonLeaderPlayer();
	bool want = Wanted() && t && other >= 0 && t.Leader() != other && t.Leader() < 2;
	if (want != g_active) {
		g_active = want;
		Log("gamecam: %s", want ? "each player has a game camera" : "one game camera");
	}
	if (!want) {
		g_copy.valid = false;
		g_copy.haveView = false;
		g_copy.numTracks = 0;
		g_liveOwner = -1;
		g_origTick(svc, edx, frame);
		return;
	}
	if (CopyStale(svc, t.raw())) {
		g_svc = svc;
		g_copyTrio = t.raw();
		g_liveOwner = -1;
	}
	// The game's camera is the leader's -- or, while the hero not leading is at
	// the merchant (whose camera takes over the game's), that hero's.
	int lead = t.Leader(), own = lead, copyHero = other;
	int shop = CoopShopHero();
	if (shop == other) own = other, copyHero = lead;
	if (g_liveOwner < 0) g_liveOwner = own;
	static bool s_shopOwned = false;   // the game's camera went to the merchant's hero
	const char* why = own != lead ? "the merchant's hero" : s_shopOwned ? "the merchant's visit is over" : "the leader changed";
	s_shopOwned = own != lead;
	if (own != g_liveOwner && !HandOver(svc, own, why)) {
		void* target = TargetOf(own);
		if (target) g_origSetTarget(svc, nullptr, target, 0.0f, 1);
	}
	ShareShake(svc);
	InputSwap s;
	UseInputOf(own, s);
	ApplyFight(svc, own);
	g_origTick(svc, edx, frame);
	Unswap(s);
	TickCopy(svc, frame, copyHero);
}

void __fastcall SetTargetHook(char* svc, void* edx, void* target, float time, char cut) {
	uintptr_t ret = (uintptr_t)_ReturnAddress();
	if (g_active && !g_inCopy && svc == g_svc && g_liveOwner >= 0 && ret >= kSwitchFollowup && ret < kSwitchFollowupEnd) {
		int h = HeroOfTarget(target);
		if (h >= 0 && h != g_liveOwner && HandOver(svc, h, "leader switch")) return;
	}
	// A switch run in the non-leader's own tick (its copy in): the rope cable car
	// boarded by the hero not leading sends "Obelix leads" from Asterix's tick.
	// Aimed at the other hero -- whose the game's camera is already -- it turned
	// Asterix's camera to Obelix for good (his once the car's end freed him).
	if (g_active && g_inCopy && svc == g_svc && ret >= kSwitchFollowup && ret < kSwitchFollowupEnd) {
		int h = HeroOfTarget(target);
		if (h >= 0 && h != g_copy.hero) {
			static int s_told = 0;
			if (s_told++ < 20)
				Log("gamecam: leader switch during %s's tick: its camera stays on it (not aimed at %s)", HeroName(g_copy.hero),
				    HeroName(h));
			return;
		}
	}
	g_origSetTarget(svc, edx, target, time, cut);
}

} // namespace

bool GameCamView(int hero, float m[16], float window[2]) {
	if (!g_active || !g_copy.valid || !g_copy.haveView || g_copy.hero != hero) return false;
	memcpy(m, g_copy.m, sizeof(g_copy.m));
	window[0] = g_copy.window[0];
	window[1] = g_copy.window[1];
	return true;
}

int GameCamLiveHero() { return g_active ? g_liveOwner : -1; }

bool GameCamPlanes(int hero, float planes[3]) {
	if (!g_active || !g_copy.valid || !g_copy.haveView || g_copy.hero != hero || g_copy.planes[1] <= 0.0f) return false;
	memcpy(planes, g_copy.planes, sizeof(g_copy.planes));
	return true;
}

namespace {
bool       g_heroSwapped = false;
uint8_t    g_heroLiveSvc[kSrvSize];
TrackBytes g_heroLiveTracks[3];
int        g_heroNumTracks = 0;
bool       g_heroShakeWatch = false;   // the leading hero's tick: did it ask for a shake?
uint8_t    g_heroShake[0x680 + 4 - 0x668];
bool       g_heroShakeOn = false;
}

void GameCamBeginHeroTick(int hero) {
	g_heroSwapped = false;
	char* svc = g_svc;
	g_heroShakeWatch = g_active && !g_inCopy && svc && hero == g_liveOwner;
	if (g_heroShakeWatch) {
		g_heroShakeOn = (*(uint32_t*)(svc + 0xC) & 0x1000) != 0;
		memcpy(g_heroShake, svc + 0x668, sizeof(g_heroShake));
	}
	if (!g_active || g_inCopy || !svc || !g_copy.valid || g_copy.hero != hero || hero == g_liveOwner) return;
	{
		Trio t = GetTrio();
		if (!t || CopyStale(svc, t.raw())) return;
	}
	CKYellowPages* yp = YellowPages();
	if (!yp || *(char**)((char*)yp + 0x68) != svc) return;
	memcpy(g_heroLiveSvc, svc, kSrvSize);
	g_heroNumTracks = 0;
	for (int i = 0; i < g_copy.numTracks; i++) {
		TrackBytes& tb = g_heroLiveTracks[g_heroNumTracks++];
		tb = g_copy.tracks[i];
		memcpy(tb.b, tb.track, tb.size);   // the game's bytes of the copy's tracks
	}
	memcpy(svc, g_copy.svc, kSrvSize);
	LoadRefs(g_copy);
	g_inCopy = true;
	g_heroSwapped = true;
}

void GameCamEndHeroTick() {
	if (g_heroShakeWatch) {
		g_heroShakeWatch = false;
		char* svc = g_svc;
		if ((*(uint32_t*)(svc + 0xC) & 0x1000) && !*(uint32_t*)(svc + 0x680) &&
		    (!g_heroShakeOn || memcmp(g_heroShake, svc + 0x668, sizeof(g_heroShake))))
			g_shakeOwn = true;
	}
	if (!g_heroSwapped) return;
	g_heroSwapped = false;
	g_inCopy = false;
	char* svc = g_svc;
	memcpy(g_copy.svc, svc, kSrvSize);
	SaveRefs(svc, g_copy);
	memcpy(svc, g_heroLiveSvc, kSrvSize);
	for (int i = 0; i < g_heroNumTracks; i++)
		memcpy(g_heroLiveTracks[i].track, g_heroLiveTracks[i].b, g_heroLiveTracks[i].size);
}

const char* GameCamCopyState(int hero) {
	return g_active && g_copy.valid && g_copy.hero == hero ? (const char*)g_copy.svc : nullptr;
}

void GameCamAimAtHero(int hero) {
	void* target = TargetOf(hero);
	if (!g_active || !target) return;
	if (g_copy.valid && g_copy.hero == hero) {
		Log("gamecam: %s's camera (copy) aimed at its hero again (it followed %p)", HeroName(hero),
		    *(void**)(g_copy.svc + 0xE8));
		g_copy.cut = true;   // on the copy's next tick
	} else if (g_liveOwner == hero && g_svc) {
		Log("gamecam: the game's camera (%s) aimed at its hero again (it followed %p)", HeroName(hero),
		    *(void**)(g_svc + 0xE8));
		g_origSetTarget(g_svc, nullptr, target, 0.0f, 1);
	}
}

void GameCamHandTo(int hero, const char* why) {
	if (!g_active || g_inCopy || !g_svc || g_liveOwner < 0 || hero == g_liveOwner) return;
	if (!HandOver(g_svc, hero, why)) {
		void* target = TargetOf(hero);
		if (target) g_origSetTarget(g_svc, nullptr, target, 0.0f, 1);
	}
}

void GameCamDropCopy(int hero) {
	if (!g_copy.valid || g_copy.hero != hero) return;
	g_copy.valid = false;
	g_copy.haveView = false;
	g_copy.numTracks = 0;
}

bool GameCamInstallHooks() {
	bool ok = PatchVtableSlot(kSrvCameraVtbl, kTickSlot, kTick, (void*)TickHook, (void**)&g_origTick,
	                          "CKSrvCamera::tick");
	ok &= InstallDetour(kSetTarget, (void*)SetTargetHook, (void**)&g_origSetTarget, kSetTargetPrologue,
	                    sizeof(kSetTargetPrologue), "CKSrvCamera::SetTarget");
	return ok;
}
