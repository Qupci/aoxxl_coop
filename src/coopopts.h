// The co-op options: the pause menu's "CO-OP Options" (ctrlopts.cpp draws them
// on the controls page, nine to a page). Each is an xxl_coop.ini key, shown
// by its name; a change applies at once (g_config) and is written back.
// A new ini option that makes sense in the game goes into kOpts (coopopts.cpp).

#pragma once
#include <cstdint>

// Reads what the table needs beyond g_config (the sp_ options as the ini has
// them: sp_isolation turns them off in g_config, not in the ini).
void CoopOptsInit();
int CoopOptCount();
// Its ini key (the row's label).
const char* CoopOptKey(int i);
// A YES / NO option: true, its value in *on.
bool CoopOptToggle(int i, bool* on);
// The value as text (the other kinds).
void CoopOptValue(int i, wchar_t* out, int n);
// Left (-1), right (+1) or confirm (0); false when nothing changed.
bool CoopOptChange(int i, int dir);
