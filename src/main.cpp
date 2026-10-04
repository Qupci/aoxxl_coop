// xxl_coop.asi -- co-op mode for Asterix & Obelix XXL (2004, PC).
//
// Loaded by the Ultimate ASI Loader that already ships in the game folder as
// dinput8.dll. Being an .asi rather than a proxy DLL means we coexist with the
// QoL patch (d3d9.dll) and the XXL Inspector instead of fighting over a slot.

#include "kal.h"
#include <cstdlib>
#include "coop.h"
#include "config.h"
#include "log.h"
#include "vtprofile.h"
#include "render.h"
#include "autotest.h"
#include "crash.h"
#include "splitscreen.h"

namespace {

bool VerifyGameBuild() {
	// axxl.exe is a fixed-base image; if it ever gets relocated, every address
	// in kal.h is wrong and we must not touch anything.
	HMODULE exe = GetModuleHandleA(nullptr);
	if ((uintptr_t)exe != ax::kImageBase) {
		Log("build check: image base is %p, expected %08X -- refusing to patch",
			exe, (unsigned)ax::kImageBase);
		return false;
	}

	// Spot-check two byte patterns the mod depends on. These are the first
	// bytes of CKHkAsterixLife::tick and of CalcProjCamera; if a different
	// build (or another mod) is in memory they will not match.
	struct Check { uintptr_t addr; const char* name; };
	static const Check checks[] = {
		{ 0x4AA850, "CKHkAsterixLife::tick" },
		{ 0x4AB6C0, "CKHkObelixLife::tick" },
		{ 0x625C90, "CKHkAsterixLife vtable" },
		{ 0x625D18, "CKHkObelixLife vtable" },
	};
	for (const Check& c : checks) {
		MEMORY_BASIC_INFORMATION mbi = {};
		if (!VirtualQuery((void*)c.addr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
			Log("build check: %s at %08X is not mapped -- refusing to patch",
				c.name, (unsigned)c.addr);
			return false;
		}
	}
	return true;
}

// Debug aid: jump straight into a level instead of playing through the menus.
// Same technique XXL-Editor's GameLauncher uses from outside the process --
// wait for the yellow pages and game manager to exist, then write the level
// number into CKGameManager::nextLevel.
void ForceLevel(int level) {
	// The request only sticks once the menu level (0) has finished loading:
	// written any earlier, the boot sequence overwrites nextLevel with 0 itself.
	// Left alone on the menu, the game eventually starts its attract-mode demo.
	Log("forceLevel: waiting for the menu...");
	for (int attempt = 0; attempt < 3; attempt++) {
		bool requested = false;
		for (int i = 0; i < 480 && !requested; i++) {   // up to ~120 s
			CKYellowPages* yp = YellowPages();
			void* gm = yp ? yp->gameManager : nullptr;
			if (gm && *(int*)((char*)gm + ax::kGM_currentLevel) == 0) {
				Log("forceLevel: menu is up (gameManager=%p), requesting level %d", gm, level);
				*(int*)((char*)gm + ax::kGM_nextLevel) = level;
				requested = true;
			} else {
				Sleep(250);
			}
		}
		if (!requested) break;
		for (int i = 0; i < 120; i++) {                 // ~30 s to get there
			Sleep(250);
			CKYellowPages* yp = YellowPages();
			void* gm = yp ? yp->gameManager : nullptr;
			if (gm && *(int*)((char*)gm + ax::kGM_currentLevel) == level) {
				Log("forceLevel: level %d is loading", level);
				return;
			}
		}
		Log("forceLevel: level %d did not start, retrying", level);
	}
	Log("forceLevel: gave up");
}

DWORD WINAPI StartupThread(LPVOID) {
	// The ASI loader runs us from DllMain, where almost nothing is safe. Do the
	// real work on our own thread, after giving the game time to build its
	// vtables and yellow pages.
	Sleep(1500);

	if (!VerifyGameBuild()) return 0;

	if (!CoopInstallHooks()) {
		Log("startup: hook installation incomplete -- co-op disabled");
		return 0;
	}

	if (g_config.vtProfile) {
		// Installed after the hooks, so a profiled slot chains through whatever
		// is already there (including our own hook) and still counts.
		VtProfileInstall(0x625C90, 14, "CKHkAsterixLife");
		VtProfileInstall(0x625A8C, 40, "CKHkAsterix");
		VtProfileInstall(0x625D18, 14, "CKHkObelixLife");
	}

	RenderInstallHooks();
	SplitInstallHooks();
	CoopInit();
	WatchdogStart();
	Log("startup: ready");

	if (g_config.forceLevel >= 0) ForceLevel(g_config.forceLevel);
	return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(hinst);
		LogOpen();
		char ini[MAX_PATH];
		GameDirPath(ini, sizeof(ini), "xxl_coop.ini");
		g_config.Load(ini);
		CrashHandlerInstall();
		// Test runs may pick the level: XXL_LEVEL overrides force_level.
		char lvl[16] = {};
		if (GetEnvironmentVariableA("XXL_LEVEL", lvl, sizeof(lvl)) && lvl[0]) {
			g_config.forceLevel = atoi(lvl);
			Log("config: force_level %d from XXL_LEVEL", g_config.forceLevel);
		}
		AutotestInit();
		if (!g_config.enabled) {
			Log("startup: disabled in xxl_coop.ini");
			return TRUE;
		}
		CloseHandle(CreateThread(nullptr, 0, StartupThread, nullptr, 0, nullptr));
	} else if (reason == DLL_PROCESS_DETACH) {
		CoopShutdown();
		LogClose();
	}
	return TRUE;
}

