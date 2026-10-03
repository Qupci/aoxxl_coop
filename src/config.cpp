#include "config.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cctype>
#include <cstdlib>
#include <cstring>

CoopConfig g_config;

static bool ReadBool(const char* section, const char* key, bool def, const char* path) {
	char buf[64] = {};
	GetPrivateProfileStringA(section, key, def ? "on" : "off", buf, sizeof(buf), path);
	for (char* p = buf; *p; ++p) *p = (char)tolower((unsigned char)*p);
	return !strcmp(buf, "on") || !strcmp(buf, "1") ||
	       !strcmp(buf, "yes") || !strcmp(buf, "true");
}

void CoopConfig::Load(const char* iniPath) {
	// GetPrivateProfileString wants a full path or it searches the Windows dir.
	char buf2[64] = {};
	char full[MAX_PATH];
	GetFullPathNameA(iniPath, MAX_PATH, full, nullptr);

	enabled     = ReadBool("coop", "enabled", enabled, full);
	verbose     = ReadBool("debug", "verbose", verbose, full);
	diagnostics = ReadBool("debug", "diagnostics", diagnostics, full);
	splitScreen = ReadBool("coop", "split_screen", splitScreen, full);
	p2Control   = ReadBool("coop", "p2_control", p2Control, full);
	p2OrderMode      = ReadBool("coop", "p2_order_mode", p2OrderMode, full);
	p2UsePathfinding = ReadBool("coop", "p2_pathfinding", p2UsePathfinding, full);
	p2InvertStrafe   = ReadBool("coop", "p2_invert_strafe", p2InvertStrafe, full);

	diagIntervalMs = GetPrivateProfileIntA("debug", "diag_interval_ms", diagIntervalMs, full);
	dropHoldMs     = GetPrivateProfileIntA("coop", "drop_hold_ms", dropHoldMs, full);
	forceLevel     = GetPrivateProfileIntA("debug", "force_level", forceLevel, full);
	watchdog       = ReadBool("debug", "watchdog", watchdog, full);
	vtProfile      = ReadBool("debug", "vtable_profile", vtProfile, full);
	testBlockP1    = ReadBool("debug", "test_block_p1", testBlockP1, full);
	forceMoveFlags = ReadBool("debug", "force_move_flags", forceMoveFlags, full);
	renderProbe    = ReadBool("debug", "render_probe", renderProbe, full);
	splitTest      = ReadBool("debug", "split_test", splitTest, full);
	watchActionReads = ReadBool("debug", "watch_action_reads", watchActionReads, full);
	GetPrivateProfileStringA("debug", "watch_target", "actions", buf2, sizeof(buf2), full);
	watchTarget = (buf2[0] == 'b' || buf2[0] == 'B') ? WATCH_COMMAND_BLOCK
	            : (buf2[0] == 'h' || buf2[0] == 'H') ? WATCH_HERO_FLAGS : WATCH_ACTIONS;
	watchHero = GetPrivateProfileIntA("debug", "watch_hero", watchHero, full);
	inputProbe     = ReadBool("debug", "input_probe", inputProbe, full);
	probePad       = GetPrivateProfileIntA("debug", "probe_pad", probePad, full);
	probeReportSecs = GetPrivateProfileIntA("debug", "probe_report_seconds", probeReportSecs, full);

	char buf[64] = {};
	GetPrivateProfileStringA("coop", "split_mode", "horizontal", buf, sizeof(buf), full);
	splitMode = (buf[0] == 'v' || buf[0] == 'V') ? SPLIT_VERTICAL : SPLIT_HORIZONTAL;

	GetPrivateProfileStringA("coop", "deadzone", "0.24", buf, sizeof(buf), full);
	deadzone = (float)atof(buf);
	if (deadzone < 0.0f || deadzone > 0.9f) deadzone = 0.24f;

	Log("config: enabled=%d split=%d mode=%s deadzone=%.2f diag=%d verbose=%d forceLevel=%d "
		"probe=%d probePad=%d",
		enabled, splitScreen, splitMode == SPLIT_VERTICAL ? "vertical" : "horizontal",
		deadzone, diagnostics, verbose, forceLevel, inputProbe, probePad);
}







