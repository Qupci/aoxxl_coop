// A diagnostic thread that reports ground truth independently of any hook.
//
// It exists because a hook firing exactly once looks identical to a hook that
// was never on the code path. This polls the engine directly, so it keeps
// reporting whether or not anything we installed is being called.

#include "coop.h"
#include "config.h"
#include "inputmgr.h"
#include "log.h"
#include "trio.h"
#include "vtprofile.h"
#include "render.h"

#include <cstdio>

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
	long lastInput = 0, lastAst = 0, lastObe = 0, lastSwaps = 0, lastSkips = 0;

	for (;;) {
		Sleep(2000);

		bool ok = Guarded([&] {
			CKYellowPages* yp = YellowPages();
			if (!yp) { Log("watch: no yellow pages yet"); return; }

			void* gm = yp->gameManager;
			int curLevel  = gm ? *(int*)((char*)gm + ax::kGM_currentLevel) : -99;
			int nextLevel = gm ? *(int*)((char*)gm + ax::kGM_nextLevel) : -99;

			long ci = g_hookCounters.inputUpdates;
			long ca = g_hookCounters.asterixTicks;
			long co = g_hookCounters.obelixTicks;

			// The single most useful line in the log: are the heroes actually
			// being ticked? Frame rate alone does not tell you that.
			bool heroesTicking = (ca != lastAst);
			Log("watch: [%s] level=%d next=%d | hooks/2s input=%ld asterix=%ld obelix=%ld",
				heroesTicking ? "PLAYING" : "NOT PLAYING",
				curLevel, nextLevel, ci - lastInput, ca - lastAst, co - lastObe);
			lastInput = ci; lastAst = ca; lastObe = co;

			RenderProbeTick();
			VtProfileReport();
			GuardWatchReport();

			if (curLevel < 0) return;   // in the menus, nothing else is live

			// Are the patches still in place? A hook that never fires looks the
			// same whether it was reverted or was never on the code path.
			Log("watch:   patches: CKInput[20]=%08X (ours %08X) | heroStateTick@48D4F0 first byte"
				" =%02X (E9 = detoured)",
				*(unsigned*)(0x623F38 + 20 * 4), (unsigned)g_patchedInputUpdate,
				*(unsigned char*)0x48D4F0);

			Log("watch:   instance counts: asterix=%u obelix=%u",
				GetNumLvlClassInstances(ax::kCKHkAsterix),
				GetNumLvlClassInstances(ax::kCKHkObelix));

			// Is player 2's input actually being swapped in, and what does it
			// contain? Distinguishes "the swap never runs" from "it runs and the
			// hero ignores it".
			long sw = g_hookCounters.p2Swaps, sk = g_hookCounters.p2SwapSkips;
			Log("watch:   p2: %s hero=%d pad=%d | swaps/2s=%ld skips/2s=%ld aiSuppressed=%ld",
				CoopP2Joined() ? "joined" : "not joined",
				CoopP2Hero(), CoopP2Pad(), sw - lastSwaps, sk - lastSkips,
				(long)g_hookCounters.aiTicksSuppressed);
			lastSwaps = sw; lastSkips = sk;
			if (g_config.p2OrderMode)
				Log("watch:   p2 order overrides by engine: %u", g_p2OrderOverrides);
			if (CoopP2Joined()) {
				char sbuf[256];
				LineBuf sl(sbuf);
				sl.Add("watch:   p2 synth:");
				const uint8_t* a = CoopP2ActionSnapshot();
				int an = CoopP2ActionCount();
				int shown = 0;
				for (int i = 0; i < an; i++) {
					if (!a[i]) continue;
					if (shown == 12) { sl.Add(" ..."); break; }
					sl.Add(" [%d]=%02X", i, a[i]);
					shown++;
				}
				if (!shown) sl.Add(" all zero");
				Log("%s", sl.c_str());
			}

			Trio t = GetTrio();
			if (t) {
				Hero a = t.GetHero(HERO_ASTERIX);
				Hero o = t.GetHero(HERO_OBELIX);

				// Do both heroes even share an input view? FUN_0048ABF0 reads it
				// from *(hero+0x14)+0x28. And what are the movement fields the
				// state machine consumes?
				auto viewOf = [](Hero h) -> void* {
					if (!h) return nullptr;
					void* owner = *(void**)(h.raw() + 0x14);
					return owner ? *(void**)((char*)owner + 0x28) : nullptr;
				};
				Log("watch:   inputview asterix=%p obelix=%p | axes asterix=(%.1f %.1f) "
					"obelix=(%.1f %.1f)",
					viewOf(a), viewOf(o),
					a ? *(float*)(a.raw() + 0x3D4) : 0.f, a ? *(float*)(a.raw() + 0x3DC) : 0.f,
					o ? *(float*)(o.raw() + 0x3D4) : 0.f, o ? *(float*)(o.raw() + 0x3DC) : 0.f);
				// The state each hero's machines are actually in. hero+0xE5 is
				// the current state of the table driven by FUN_0048D4F0 (the one
				// that consumes player input); +0x561 is the AI action state.
				Log("watch:   states asterix e5=%02X a561=%02X f18=%02X | obelix e5=%02X "
					"a561=%02X f18=%02X",
					a ? *(uint8_t*)(a.raw() + 0xE5) : 0, a ? a.Action() : 0, a.Flags18(),
					o ? *(uint8_t*)(o.raw() + 0xE5) : 0, o ? o.Action() : 0, o.Flags18());

				// The real values a moving player hero carries, so player 2's can
				// be matched rather than guessed. +0x384 bits 0..2 select the
				// movement state; +0x574..+0x57C is the desired direction that
				// FUN_0048ACA0 writes.
				auto dirOf = [](Hero h, int i) {
					return h ? *(float*)(h.raw() + 0x574 + 4 * i) : 0.f;
				};
				Log("watch:   move asterix f384=%08X dir=(%.2f %.2f %.2f) | "
					"obelix f384=%08X dir=(%.2f %.2f %.2f)",
					a ? *(unsigned*)(a.raw() + 0x384) : 0,
					dirOf(a, 0), dirOf(a, 1), dirOf(a, 2),
					o ? *(unsigned*)(o.raw() + 0x384) : 0,
					dirOf(o, 0), dirOf(o, 1), dirOf(o, 2));

				if (CoopP2Joined()) {
					float ax[6];
					CoopP2DebugAxes(ax);
					Log("watch:   p2 axes: before=(%.1f %.1f) afterRead=(%.1f %.1f) "
						"afterTick=(%.1f %.1f)",
						ax[0], ax[1], ax[2], ax[3], ax[4], ax[5]);
					Log("watch:   p2 moveFlags(hero+0x384)=%08X", CoopP2MoveFlags());
				}
				Log("watch:   trio leader=%d companion=%d | asterix hook=%p f18=%02X busy=%d order=%d "
					"| obelix hook=%p f18=%02X busy=%d order=%d",
					t.Leader(), t.Companion(),
					a.hook, a.Flags18(), a ? a.Busy() : 0, a ? (int)a.Order() : -1,
					o.hook, o.Flags18(), o ? o.Busy() : 0, o ? (int)o.Order() : -1);
			} else {
				Log("watch:   no trio");
			}

			// Any live action bytes? Proves the input manager is running even if
			// our frame hook is not.
			InputManager im = GetInputManager();
			if (im) {
				uint8_t* v = im.Values();
				int n = im.NumActions();
				if (n > (int)kMaxActions) n = (int)kMaxActions;
				char storage[256];
				LineBuf line(storage);
				line.Add("watch:   input mgr=%p n=%d updated=%u nonzero:",
				         im.obj, n, im.UpdatedThisFrame());
				int shown = 0;
				for (int i = 0; i < n && v; i++) {
					if (!v[i]) continue;
					if (shown == 12) { line.Add(" ..."); break; }
					line.Add(" [%d]=%02X", i, v[i]);
					shown++;
				}
				if (!shown) line.Add(" none");
				Log("%s", line.c_str());
			} else {
				Log("watch:   no input manager");
			}

			// Armed last: everything above reads the array, and those reads
			// would otherwise consume the one fault the guard gives us.
			GuardWatchArm();
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





