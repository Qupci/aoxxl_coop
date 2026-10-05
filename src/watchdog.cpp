// A diagnostic thread that reports ground truth every 2 s, independently of any
// hook: a hook that never fires looks exactly like one that was never on the
// code path, so this polls the engine directly.

#include "coop.h"
#include "config.h"
#include "crash.h"
#include "splitscreen.h"
#include "proximity.h"
#include "klusters.h"
#include "sound.h"
#include "inputmgr.h"
#include "life.h"
#include "log.h"
#include "trio.h"
#include "vtprofile.h"
#include "render.h"

namespace {

// The engine objects live on the game's heap and can be torn down under us
// during a level change, so every dereference from this thread is guarded.
template <class F>
bool Guarded(F&& fn) {
	__try {
		fn();
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

DWORD WINAPI WatchdogThread(LPVOID) {
	long lastInput = 0, lastAst = 0, lastObe = 0, lastRouted = 0;

	for (;;) {
		Sleep(2000);

		StallCheck(g_hookCounters.inputUpdates);

		bool ok = Guarded([&] {
			CKYellowPages* yp = YellowPages();
			if (!yp) { Log("watch: no yellow pages yet"); return; }

			void* gm = yp->gameManager;
			int curLevel  = gm ? *(int*)((char*)gm + ax::kGM_currentLevel) : -99;
			int nextLevel = gm ? *(int*)((char*)gm + ax::kGM_nextLevel) : -99;

			long ci = g_hookCounters.inputUpdates;
			long ca = g_hookCounters.asterixTicks;
			long co = g_hookCounters.obelixTicks;
			long cr = g_hookCounters.p2Routed;
			static long lastSplit = 0, lastProx = 0;
			long cs = SplitFrames();
			long cp = ProximitySlices();
			Log("watch: level=%d next=%d | per 2s: frames=%ld lifeTicks A=%ld O=%ld p2Routed=%ld "
				"split=%ld nearestHero=%ld p2Bonuses=%ld p2Beacons=%ld p2Contacts=%ld klusters=%ld/%ld sound=%ld/%ld region=%ld/%ld | "
				"enforced=%ld trioEvents=%ld | p2=%s%s",
				curLevel, nextLevel, ci - lastInput, ca - lastAst, co - lastObe, cr - lastRouted,
				cs - lastSplit, cp - lastProx, ProximityP2Bonuses(), ProximityP2Beacons(),
				ProximityP2Contacts(), KlusterQueries(), KlusterP2Only(), SoundNearP2(), SoundMixedAsP2(), SoundRegionKept(), SoundRegionStarted(),
				g_hookCounters.enforced,
				g_hookCounters.trioEvents,
				CoopP2Joined() ? HeroName(CoopP2Hero()) : "-",
				CoopSuspended() ? " (suspended)" : "");
			lastSplit = cs;
			lastProx = cp;
			lastInput = ci; lastAst = ca; lastObe = co; lastRouted = cr;

			RenderProbeTick();
			VtProfileReport();
			if (curLevel < 0) return;   // in the menus, nothing else is live
			if (g_config.verbose) CoopLogState("watch");
		});

		if (!ok) Log("watch: faulted while reading engine state (level change?)");
	}
}

} // namespace

void WatchdogStart() {
	if (!g_config.watchdog) return;
	HANDLE h = CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);
	if (h) CloseHandle(h);
	Log("watch: started");
}
