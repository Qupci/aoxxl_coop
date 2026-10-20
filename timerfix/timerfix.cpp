// xxl_timerfix.asi: gives Asterix & Obelix XXL a 1 ms timer, independent of the co-op mod.
//
// The game's frame limiter (axxl.exe 0x4016D2) is Sleep(15 - the frame's ms so far).
// Since Windows 10 2004 a process's Sleep has 1 ms steps only if that process asks
// for them (timeBeginPeriod). The game never does; IndirectSound's XAudio2 does it
// for it, DSOAL doesn't. Without it sleeps round up to 15.6 ms and the game drops
// from 62 to ~45 fps.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

BOOL APIENTRY DllMain(HMODULE hinst, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(hinst);
		timeBeginPeriod(1);
	}
	return TRUE;
}
