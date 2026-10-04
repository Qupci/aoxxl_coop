#pragma once

enum SplitMode {
	SPLIT_HORIZONTAL = 0,  // stacked, player 1 on top
	SPLIT_VERTICAL   = 1,  // side by side, player 1 on the left
};

struct CoopConfig {
	bool  enabled       = true;
	bool  verbose       = false;
	int   forceLevel    = -1;     // debug: jump straight to this level once
	bool  watchdog      = false;  // background thread reporting engine state every 2 s
	bool  vtProfile     = false;  // count calls through hero / hero-life vtables
	bool  renderProbe   = false;  // find the scene-render call via RwCamera.beginUpdate
	bool  splitTest     = false;  // render the scene twice, to prove re-entrancy
	int   splitMode     = SPLIT_VERTICAL;
	bool  splitScreen   = true;   // off = shared camera on the leader
	int   p1Pad         = -1;     // XInput slot the game reads (player 1); -1 = detect automatically
	int   dropHoldMs    = 1500;
	float deadzone      = 0.24f;

	void Load(const char* iniPath);
};

extern CoopConfig g_config;
