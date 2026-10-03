// Player-2 control via the engine's AI order interface.
//
// Why this exists: the engine has no second player-controlled hero. The hero's
// locomotion, gravity and animation for a *companion* all come from the AI
// behaviour machine (FUN_0049DE10), whose state is chosen by FUN_0049EE80
// straight from the AI order byte at hero+0x564. There is no equivalent path
// that a second hero can be pushed down without being the trio's leader.
//
// So instead of fighting that, this drives the documented order interface:
// order 1 ("Aller a position") with a target that is continuously replaced by a
// point a short distance ahead of the hero in player 2's stick direction. The
// engine then walks him there with its own physics and animation -- correct
// gravity, correct turning, correct movement for whichever hero it is.
//
// The trade-off is honest: this steers rather than controls. There is a little
// lag, and it will not be as crisp as player 1. What it is not is a hack around
// the engine -- order 12 is literally named "Sous controle exterieur" (under
// external control), so driving a hero from outside is something the engine was
// built to allow.

#include "coop.h"
#include "config.h"
#include "log.h"
#include "pads.h"
#include "trio.h"

#include <cmath>

namespace {

// How far ahead of the hero the steering target is placed, in world units.
//
// This has to be far enough that he never actually *arrives*: reaching the
// target completes the order, and the engine then drops him back to order 0
// ("Suivre Leader"), which reads as control cutting out intermittently. Since
// the target is replaced every frame anyway, a long lead costs nothing.
const float kTargetDistance = 20.0f;

} // namespace

unsigned g_p2OrderOverrides = 0;   // times the engine changed our order back

// Set when the steering has to be re-armed from scratch (stick released, order
// taken away by the engine), so the next push re-issues rather than assuming the
// hero is still heading where we last told him.
static bool  g_haveLastDir = false;
static Vector3 g_lastDir;

void P2ResetSteering() { g_haveLastDir = false; }

void P2DriveByOrder(Trio trio, Hero hero, int padIndex) {
	if (!trio || !hero) return;
	const PadState& p = PadGet(padIndex);
	if (!p.connected) return;

	float mag = sqrtf(p.lx * p.lx + p.ly * p.ly);
	if (mag < 0.15f) {
		// Stick released: stop where he is rather than coasting to a stale
		// target. Order 3 is "Attendre" (wait).
		if (hero.Order() != AI_WAIT) hero.SetOrder(AI_WAIT);
		P2ResetSteering();
		return;
	}

	// The trio keeps a camera-relative basis at +0x39C, built each frame from
	// the camera yaw (cos, 0, sin). Player 2's stick is interpreted against the
	// same basis as player 1's, so "up" means away from the camera for both.
	const float* fwd = (const float*)(trio.raw() + 0x39C);
	Vector3 forward(fwd[0], 0.0f, fwd[2]);
	float flen = sqrtf(forward.x * forward.x + forward.z * forward.z);
	if (flen < 0.001f) return;
	forward = forward * (1.0f / flen);

	// Right-handed perpendicular in the XZ plane.
	Vector3 right(forward.z, 0.0f, -forward.x);
	if (g_config.p2InvertStrafe) right = right * -1.0f;

	Vector3 dir = forward * p.ly + right * p.lx;
	float dlen = sqrtf(dir.x * dir.x + dir.z * dir.z);
	if (dlen < 0.001f) return;
	dir = dir * (1.0f / dlen);

	// Log what we actually computed. "Same direction whatever the stick does"
	// has two very different causes -- a constant `dir` here, or a handler that
	// latches its target -- and they need opposite fixes.
	{
		static DWORD lastLog = 0;
		DWORD now = GetTickCount();
		if ((int)(now - lastLog) >= 400) {
			lastLog = now;
			Log("p2drive: stick=(%.2f %.2f) fwd=(%.2f %.2f %.2f) dir=(%.2f %.2f %.2f) "
				"pos=(%.1f %.1f %.1f) order=%d",
				p.lx, p.ly, forward.x, forward.y, forward.z, dir.x, dir.y, dir.z,
				hero.Pos().x, hero.Pos().y, hero.Pos().z, (int)hero.Order());
		}
	}

	Vector3 target = hero.Pos() + dir * kTargetDistance;
	hero.AiTargetPos() = target;
	hero.AiTargetDir() = dir;

	AiOrder want = g_config.p2UsePathfinding ? AI_GOTO_POS : AI_GOTO_POS_NO_PF;

	// The order handler latches its target when the order *starts*, not every
	// frame: writing hero+0x568 continuously made the hero walk to wherever he
	// was first pointed and ignore the stick until he arrived. Re-issuing
	// through CKGrpTrio's own event is what re-arms it.
	//
	// Event 0x4804 sets the companion's order and takes a parameter of two
	// Vector3 pointers, {&position, &direction} -- the same shape the trio's
	// handler uses for the Idefix and cinematic goto events (0x4879 / 0x48BA /
	// 0x48EF), which copy param[0] into hero+0x568 and param[1] into +0x574.
	// Note: event 0x4804 only assigns the order -- its handler `break`s to a tail
	// that calls FUN_0041CB50 on the parameters rather than copying them into
	// hero+0x568. Only 0x4879 (Idefix), 0x48BA and 0x48EF actually carry a
	// position. So the target has to be written directly, as below.

	// Only re-issue when the requested heading actually changes, so the handler
	// is not restarted every single frame (which would stop him dead each time).
	float turn = g_haveLastDir ? (dir.x * g_lastDir.x + dir.z * g_lastDir.z) : -1.0f;

	if (hero.Order() != want) {
		if (hero.Order() != AI_WAIT) g_p2OrderOverrides++;
		g_haveLastDir = false;
	}
	if (!g_haveLastDir || turn < 0.985f) {          // ~10 degrees
		hero.SetOrder(want);
		hero.AiTargetPos() = target;
		hero.AiTargetDir() = dir;
		g_lastDir = dir;
		g_haveLastDir = true;
	}
}

