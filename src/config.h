#pragma once

enum WatchTarget {
	WATCH_ACTIONS       = 0,
	WATCH_COMMAND_BLOCK = 1,
	WATCH_HERO_FLAGS    = 2,
};

enum SplitMode {
	SPLIT_HORIZONTAL = 0,  // stacked, player 1 on top
	SPLIT_VERTICAL   = 1,  // side by side, player 1 on the left
};

struct CoopConfig {
	bool enabled          = true;
	bool verbose          = false;
	bool diagnostics      = false;  // dump live engine state to the log
	int  diagIntervalMs   = 1000;
	int  forceLevel       = -1;     // debug: jump straight to this level once
	bool watchdog         = false;  // background thread reporting engine ground truth
	bool vtProfile        = false;  // count calls through hero / hero-life vtables
	bool testBlockP1      = false;  // blank the action array during Asterix's tick
	bool forceMoveFlags   = true;   // experiment: drive hero+0x384 bits 0-2 from the pad
	bool renderProbe      = false;  // find the scene-render call via RwCamera.beginUpdate
	bool splitTest        = false;  // render the scene twice, to prove re-entrancy
	bool watchActionReads = false;  // page-guard sampler on an input region
	int  watchTarget      = 0;      // WATCH_ACTIONS or WATCH_COMMAND_BLOCK
	int  watchHero        = -1;     // hero index for WATCH_HERO_FLAGS, -1 = leader
	bool inputProbe       = false;  // correlate pad edges against the command block
	int  probePad         = 0;      // which XInput slot the probe watches
	int  probeReportSecs  = 20;     // how often to dump the correlation table
	int  splitMode        = SPLIT_HORIZONTAL;
	bool splitScreen      = true;   // off = shared camera on the leader
	bool p2Control        = true;   // drive P2's hero from pad 2 (vs. just switching)
	bool p2OrderMode      = true;   // steer P2 via the AI order interface
	bool p2UsePathfinding = false;  // order 1 (pathfinds) vs order 15 (direct)
	bool p2InvertStrafe   = false;
	int  joinButton       = 0x0020; // XINPUT_GAMEPAD_START
	int  dropButton       = 0x0020; // held, see coop.cpp
	int  dropHoldMs       = 1500;
	float deadzone        = 0.24f;

	void Load(const char* iniPath);
};

extern CoopConfig g_config;







