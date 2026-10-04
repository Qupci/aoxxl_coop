// Crash and stall forensics, so a one-off crash during real play leaves enough
// in xxl_coop.log to find it: exception code, faulting address (as axxl.exe+off),
// registers, return addresses on the stack, and the heroes' control state.

#pragma once

void CrashHandlerInstall();

// Watchdog thread: call every ~2 s with the frame counter. If frames stop for
// 6 s, the game thread is sampled once and its location logged.
void StallCheck(long frameCounter);

// Game thread: call once from the frame hook so StallCheck knows whom to sample.
void StallRegisterGameThread();
