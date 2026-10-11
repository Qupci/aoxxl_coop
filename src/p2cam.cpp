#include "p2cam.h"
#include "coop.h"
#include "gamecam.h"
#include "config.h"
#include "log.h"
#include "pads.h"
#include "padident.h"
#include "render.h"
#include "trio.h"

#include <cmath>
#include <cstring>

namespace {

const float kYawSpeed    = 2.6f;    // rad/s at full right-stick deflection
const float kPitchSpeed  = 1.6f;
const float kPitchMin    = -0.10f;  // just below the horizon
const float kPitchMax    = 1.25f;   // almost straight down
const float kFollowXZ    = 14.0f;   // focus follow rates (1/s)
const float kFollowY     = 5.0f;    // slower vertically: jumps don't bob the view
const int   kSteps       = 16;      // wall test samples between focus and camera
const float kAutoFollow  = 1.8f;    // rad/s swing behind a moving hero (stick idle)
const float kIdleBefore  = 0.4f;    // s of untouched right stick before auto-follow
const float kEaseOut     = 2.5f;    // 1/s: back out to full distance after a wall

struct Cam {
	bool    init = false;
	float   yaw = 0, pitch = 0.4f, dist = 5.0f, focusH = 1.5f;
	Vector3 focus, pos;
	float   m[16] = {};
	bool    haveMatrix = false;
	DWORD   lastTick = 0;
	float   reach = 1.0f;       // smoothed fraction of the full distance
	float   idle = 0.0f;        // time since the right stick was last used
	Vector3 prevHero;
	bool    havePrev = false;
	float   handed = 1.0f;    // sign of right . (up x at) in the game's camera matrix
	uint32_t matFlags = 0;
};
Cam g_cams[2];      // [0]: player 1's (only while player 2's hero leads), [1]: player 2's
Cam* g_cur = &g_cams[1];
#define cam (*g_cur)

// The trio's movement basis around a player's hero tick.
struct SavedBasis { bool active = false; float a37C, a384, b[3]; } basis;

char* CameraService() {
	CKYellowPages* yp = YellowPages();
	return yp ? *(char**)((char*)yp + 0x68) : nullptr;
}

Vector3 Cross(const Vector3& a, const Vector3& b) { return a.cross(b); }
Vector3 Normalize(const Vector3& v) {
	float l = sqrtf(v.len2());
	return l > 1e-6f ? v * (1.0f / l) : Vector3(0, 0, 1);
}

// CKSrvCollision cylinder test (FUN_0044A210), used exactly like the game camera
// uses it. Returns 2 or 3 when the shape intersects the world.
int CollideAt(const Vector3& p) {
	CKYellowPages* yp = YellowPages();
	char* srv = CameraService();
	void* coll = yp ? *(void**)((char*)yp + 0x50) : nullptr;
	if (!coll || !srv) return 0;
	typedef unsigned char(__fastcall* CylFn)(void* self, void* edx, float* pos, float radius,
	                                         float height, float* push, char flag);
	float half = *(float*)(srv + 0x660) + *(float*)(srv + 0x640) - 0.09f;
	if (!(half > 0.01f && half < 5.0f)) half = 0.3f;
	float radius = *(float*)(srv + 0x630);
	if (!(radius > 0.01f && radius < 5.0f)) radius = 0.3f;
	float pos[3] = { p.x, p.y - half, p.z };
	// The fourth argument is a whole collision result (push vector, normal,
	// extents and flags -- at least 9 fields are written on a hit), not just a
	// push vector: a 3-float buffer here overran the stack.
	float result[64] = {};
	return ((CylFn)0x44A210)(coll, nullptr, pos, radius, half + half, result, 1);
}

void Seed(Hero leader, const char* state = nullptr) {
	char* srv = CameraService();
	RwCamera* rw = MainCamera();
	if (!srv || !rw || !leader) return;
	// The service keeps last frame's camera position at +0x5B0 and look-at at +0x5BC
	// (`state`: the copy of it that follows the other player's hero).
	const char* st = state ? state : srv;
	Vector3 gpos = *(const Vector3*)(st + 0x5B0), glook = *(const Vector3*)(st + 0x5BC);
	Vector3 off = gpos - glook;
	float d = sqrtf(off.len2());
	// Seeded right after a scripted shot (a launcher's flight camera keeps 9.7
	// units) the view started far out; a follow camera stays near the hero.
	cam.dist = (d > 1.5f && d < 15.0f) ? d : 5.0f;
	if (cam.dist > 6.0f) cam.dist = 6.0f;
	// ...and one seeded where the game camera had pulled in (a wall, the end of
	// a dialogue's close-up) stayed zoomed in on the hero; walls pull it in anyway.
	if (cam.dist < 4.0f) cam.dist = 4.0f;
	cam.pitch = d > 0.01f ? asinf(off.y / d) : 0.4f;
	// The level-start camera is often almost level; start a bit higher so the
	// ground around player 2 is visible.
	if (cam.pitch < 0.25f) cam.pitch = 0.25f;
	if (cam.pitch < kPitchMin) cam.pitch = kPitchMin;
	if (cam.pitch > kPitchMax) cam.pitch = kPitchMax;
	cam.yaw = atan2f(off.z, off.x);
	float h = glook.y - leader.Pos().y;
	cam.focusH = (h > 1.0f && h < 4.0f) ? h : 1.5f;

	// Matrix handedness and flags, copied from the game's own camera.
	const float* gm = (const float*)((char*)(*(void**)((char*)rw + 4)) + 0x10);
	Vector3 r(gm[0], gm[1], gm[2]), u(gm[4], gm[5], gm[6]), a(gm[8], gm[9], gm[10]);
	cam.handed = r.dot(Cross(u, a)) >= 0.0f ? 1.0f : -1.0f;
	cam.matFlags = *(const uint32_t*)&gm[3];

	cam.init = true;
	Log("p2cam: seeded from the game camera: dist %.2f pitch %.2f yaw %.2f focus +%.2f "
		"(collision r=%.2f half=%.2f, handed %+.0f)", cam.dist, cam.pitch, cam.yaw, cam.focusH,
		*(float*)(srv + 0x630), *(float*)(srv + 0x660) + *(float*)(srv + 0x640), cam.handed);
}

} // namespace

void ResetCam(Cam& c) { c.init = false; c.haveMatrix = false; c.havePrev = false; c.reach = 1.0f; }
void P2CamReset() { ResetCam(g_cams[0]); ResetCam(g_cams[1]); }

// Player 1's pad: the one the game reads (pad identification), if known.
int Player1Pad() {
	for (int i = 0; i < kMaxPads; i++)
		if (i != CoopP2Pad() && PadGet(i).connected && PadIdentRole(i) == PAD_ROLE_GAME) return i;
	return -1;
}

namespace {
// Where the hero is drawn: its node's world position. A hero carried by
// something (Asterix seated in the cable car) keeps its own position (+kHero_pos)
// where it got on, while the node moves with the ride.
Vector3 WorldPos(Hero h) {
	CNode* node = h.hook->node;
	const float* p = node && node->matrix ? (const float*)((char*)node->matrix + 0x80) : nullptr;
	if (p && std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) &&
	    (p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f))
		return Vector3(p[0], p[1], p[2]);
	return h.Pos();
}
void UpdateCam(Hero hero, Hero leader, int pad, const char* seedState = nullptr);
}

void P2CamFrame() {
	if (!CoopP2Joined() || !g_config.splitScreen) { P2CamReset(); return; }
	Trio t = GetTrio();
	if (!t) return;
	Hero leader = t.GetHero(t.Leader());
	if (g_config.gameCamera) {
		// Each player not leading has a copy of the game's camera (gamecam.cpp),
		// except while the game holds their hero for a scripted sequence (riding
		// the cable car, waiting on the spinning platform): the other player's
		// action drives the game's camera then, and the held player looks around
		// with this follow camera.
		const int heroes[2] = { CoopP1Hero(), CoopP2Hero() };
		const int pads[2] = { Player1Pad(), CoopP2Pad() };
		for (int p = 0; p < 2; p++) {
			g_cur = &g_cams[p];
			int idx = heroes[p];
			if (idx >= 0 && idx != t.Leader() && CoopHeroHeld(idx))
				UpdateCam(t.GetHero(idx), leader, pads[p], GameCamCopyState(idx));
			else
				ResetCam(g_cams[p]);
		}
		g_cur = &g_cams[1];
		return;
	}
	g_cur = &g_cams[1];
	UpdateCam(t.GetHero(CoopP2Hero()), leader, CoopP2Pad());
	// Player 1 needs a camera of its own only while the game's camera follows
	// player 2's hero and player 1's hero is free to move.
	int p1 = CoopP1Hero();
	g_cur = &g_cams[0];
	// ...except while player 1's hero is the game's for a scripted shot of its
	// own (the cable car ride: the game camera leaves its classic follow track
	// for a path track that frames the rider) -- then the game camera is right.
	char* srv = CameraService();
	char* track = srv ? *(char**)(srv + 0x38) : nullptr;
	bool scriptedShot = CoopHeroHeld(p1) && track && *(uintptr_t*)track != 0x624418;   // CKCameraClassicTrack
	if (t.Leader() == CoopP2Hero() && !scriptedShot) UpdateCam(t.GetHero(p1), leader, Player1Pad());
	else ResetCam(g_cams[0]);
	g_cur = &g_cams[1];
}

namespace {
void UpdateCam(Hero hero, Hero leader, int pad, const char* seedState) {
	if (!hero) return;

	DWORD now = GetTickCount();
	float dt = cam.lastTick ? (now - cam.lastTick) / 1000.0f : 0.016f;
	cam.lastTick = now;
	if (dt < 0.0f || dt > 0.1f) dt = 0.016f;

	Vector3 heroAt = WorldPos(hero);
	Vector3 target = heroAt + Vector3(0, cam.focusH, 0);
	if (!cam.init) {
		// Not from a scripted shot: during a dialogue or cutscene the game camera
		// is the scene's (a Roman agent's close-up seeded player 1's own camera at
		// 3 units, focus at the hero's knees, after the dialogue in level 5).
		if (CoopSuspended()) return;
		Seed(leader, seedState);
		if (!cam.init) return;
		target = heroAt + Vector3(0, cam.focusH, 0);
		cam.focus = target;
	}

	PadState none = {};
	const PadState& p = pad >= 0 ? PadGet(pad) : none;
	cam.yaw   += p.rx * kYawSpeed * dt;
	cam.pitch -= p.ry * kPitchSpeed * dt;

	// Lazy follow, like the game's own camera: with the right stick left alone,
	// swing round behind a moving hero. The turn rate goes with sin(angle off
	// behind), so walking sideways turns the view most, walking away needs no
	// turn, and walking towards the camera does not spin it round.
	Vector3 hp = heroAt;
	bool stick = fabsf(p.rx) > 0.1f || fabsf(p.ry) > 0.1f;
	cam.idle = stick ? 0.0f : cam.idle + dt;
	if (cam.havePrev && dt > 0.0f) {
		float vx = (hp.x - cam.prevHero.x) / dt, vz = (hp.z - cam.prevHero.z) / dt;
		float speed = sqrtf(vx * vx + vz * vz);
		if (!stick && cam.idle > kIdleBefore && speed > 1.0f && speed < 40.0f) {
			float behind = atan2f(-vz, -vx);
			float diff = behind - cam.yaw;
			while (diff >  3.14159265f) diff -= 6.2831853f;
			while (diff < -3.14159265f) diff += 6.2831853f;
			float rate = kAutoFollow * (speed > 6.0f ? 1.0f : speed / 6.0f);
			cam.yaw += sinf(diff) * rate * dt;
		}
	}
	cam.prevHero = hp;
	cam.havePrev = true;
	if (cam.pitch < kPitchMin) cam.pitch = kPitchMin;
	if (cam.pitch > kPitchMax) cam.pitch = kPitchMax;

	float kx = 1.0f - expf(-kFollowXZ * dt), ky = 1.0f - expf(-kFollowY * dt);
	cam.focus.x += (target.x - cam.focus.x) * kx;
	cam.focus.z += (target.z - cam.focus.z) * kx;
	cam.focus.y += (target.y - cam.focus.y) * ky;

	Vector3 dir(cosf(cam.pitch) * cosf(cam.yaw), sinf(cam.pitch), cosf(cam.pitch) * sinf(cam.yaw));
	Vector3 want = cam.focus + dir * cam.dist;

	// Walk from the hero out to where the camera wants to be and stop short of the
	// first wall: no clipping into geometry, no wall between player 2 and Obelix.
	float reach = 1.0f;
	for (int i = 3; i <= kSteps; i++) {
		float f = (float)i / kSteps;
		if (CollideAt(cam.focus + dir * (cam.dist * f)) >= 2) { reach = (float)(i - 1) / kSteps; break; }
	}
	// Pull in at once when a wall comes between (never look through it), ease
	// back out when it clears -- no jumping between distances.
	if (reach < cam.reach) cam.reach = reach;
	else cam.reach += (reach - cam.reach) * (1.0f - expf(-kEaseOut * dt));
	cam.pos = cam.focus + dir * (cam.dist * cam.reach);
	(void)want;
	// Never below the hero's feet: water is not solid to the collision test, and
	// a camera under the surface looking up at a swimming Obelix is useless.
	float floorY = heroAt.y + 0.5f;
	if (cam.pos.y < floorY) cam.pos.y = floorY;

	// Look-at matrix in the game camera's own convention.
	Vector3 at = Normalize(cam.focus - cam.pos);
	Vector3 worldUp(0, 1, 0);
	Vector3 right, up;
	if (cam.handed > 0) { right = Normalize(Cross(worldUp, at)); up = Cross(at, right); }
	else                { right = Normalize(Cross(at, worldUp)); up = Cross(right, at); }
	float* m = cam.m;
	m[0] = right.x; m[1] = right.y; m[2] = right.z;  memcpy(&m[3], &cam.matFlags, 4);
	m[4] = up.x;    m[5] = up.y;    m[6] = up.z;     m[7] = 0;
	m[8] = at.x;    m[9] = at.y;    m[10] = at.z;    m[11] = 0;
	m[12] = cam.pos.x; m[13] = cam.pos.y; m[14] = cam.pos.z; m[15] = 1.0f;
	cam.haveMatrix = true;
}

} // namespace

// Autotest: player 2's camera behind player 2's hero, looking towards `target`.
void P2CamLookToward(const Vector3& target) {
	Trio t = GetTrio();
	Hero h = t ? t.GetHero(CoopP2Hero()) : Hero();
	if (!h || !g_cams[1].init) return;
	Vector3 p = h.Pos();
	g_cams[1].yaw = atan2f(p.z - target.z, p.x - target.x);
	g_cams[1].pitch = 0.3f;
	g_cams[1].idle = 0.0f;
}

namespace {
// With the game camera copies: a player's own view unless their hero leads (then
// the game's camera is theirs) -- the follow camera while held, else the copy.
bool PlayerView(int idx, const Cam& c, float out[16], float window[2]) {
	Trio t = GetTrio();
	if (!t || idx < 0 || t.Leader() == idx) return false;
	if (c.haveMatrix) {
		memcpy(out, c.m, sizeof(c.m));
		return window == nullptr;
	}
	float w[2];
	if (!GameCamView(idx, out, w)) return false;
	if (window) { window[0] = w[0]; window[1] = w[1]; }
	return true;
}
}

bool P1CamWindow(float window[2]) {
	float m[16];
	return g_config.gameCamera && PlayerView(CoopP1Hero(), g_cams[0], m, window);
}
bool P2CamWindow(float window[2]) {
	float m[16];
	return g_config.gameCamera && PlayerView(CoopP2Hero(), g_cams[1], m, window);
}

bool P1CamMatrix(float out[16]) {
	if (g_config.gameCamera) return PlayerView(CoopP1Hero(), g_cams[0], out, nullptr);
	if (!g_cams[0].haveMatrix) return false;
	memcpy(out, g_cams[0].m, sizeof(g_cams[0].m));
	return true;
}

bool OtherViewMatrix(float out[16]) {
	Trio t = GetTrio();
	if (t && CoopP2Joined() && t.Leader() == CoopP2Hero() && P1CamMatrix(out)) return true;
	return P2CamMatrix(out);
}

bool P2CamMatrix(float out[16]) {
	if (g_config.gameCamera) return PlayerView(CoopP2Hero(), g_cams[1], out, nullptr);
	if (!cam.haveMatrix) return false;
	memcpy(out, cam.m, sizeof(cam.m));
	return true;
}

void BeginTickWith(const float* m);
void P2CamBeginHeroTick() { float m[16]; BeginTickWith(P2CamMatrix(m) ? m : nullptr); }
void P1CamBeginHeroTick() { float m[16]; BeginTickWith(P1CamMatrix(m) ? m : nullptr); }
void P1CamEndHeroTick() { P2CamEndHeroTick(); }

void BeginTickWith(const float* m) {
	basis.active = false;
	if (!m) return;
	Trio t = GetTrio();
	if (!t) return;
	char* trio = t.raw();
	Vector3 fwd(m[8], 0, m[10]);
	if (fwd.len2() < 1e-6f) return;
	fwd = Normalize(fwd);
	basis.a37C = *(float*)(trio + 0x37C);
	basis.a384 = *(float*)(trio + 0x384);
	memcpy(basis.b, trio + 0x39C, sizeof(basis.b));
	float ang = atan2f(fwd.z, fwd.x);
	if (ang < 0) ang += 6.2831855f;
	*(float*)(trio + 0x37C) = ang;
	*(float*)(trio + 0x384) = ang;
	*(float*)(trio + 0x39C) = fwd.x;
	*(float*)(trio + 0x3A0) = 0.0f;
	*(float*)(trio + 0x3A4) = fwd.z;
	basis.active = true;
}

void P2CamEndHeroTick() {
	if (!basis.active) return;
	basis.active = false;
	Trio t = GetTrio();
	if (!t) return;
	char* trio = t.raw();
	*(float*)(trio + 0x37C) = basis.a37C;
	*(float*)(trio + 0x384) = basis.a384;
	memcpy(trio + 0x39C, basis.b, sizeof(basis.b));
}
