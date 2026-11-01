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
	bool  splitTest     = false;
	bool  showLeader    = false;  // the trio's leader written on the screen
	bool  cheats        = false;  // test helpers: Back + Y fills the magic potion  // render the scene twice, to prove re-entrancy
	int   splitMode     = SPLIT_VERTICAL;
	bool  splitScreen   = true;   // off = shared camera on the leader
	bool  gameCamera    = true;   // split screen: each player gets a copy of the game's camera (off = the mod's follow camera)
	float splitFov      = 15.0f;  // split screen: degrees added to each half's field of view
	int   p1Pad         = -1;     // controller the game reads (player 1): XInput slot 0-3, 4 = keyboard; -1 = detect
	int   dropHoldMs    = 1500;
	bool  obelixPotion  = true;   // the magic potion works on a player's Obelix as on Asterix
	bool  skipCutscenes = false;  // debug: every cutscene ends on Enter (the game's own skip)
	bool  individual    = true;   // each hero its own health, shields and combo bar; a fallen hero can be revived
	bool  romansGuardDowned = true; // ... the basic Romans near a fallen hero stay round it, shielded, until it is revived
	bool  hintJoin    = true;   // [hints] hint_join: how player 2 joins, at a level's start
	bool  hintPass    = true;   // [hints] hint_pass_roman: "LB passes the Roman" while spinning one
	bool  hintCombos  = true;   // [hints] hint_combos: the combo hints (combohints.cpp)
	float reviveHold    = 5.0f;   // ... seconds the other player holds the grab button by it to revive it
	float reviveHoldCatapultWindow = 0.4f; // ... under catapult fire: this share of the time between its stones (at most reviveHold)
	float reviveLimit   = 0.0f;   // ... seconds to revive it before the game over: 0 none, -1 auto (the walk there + reviveGrace)
	float reviveGrace   = 30.0f;  // ... the limit's seconds for the revive itself once there
	bool  safeSequence  = true;   // the non-leading player can't set off levers meant for the leader's switch
	int   sectorCatchUpMs = 1000; // a hero left in an unloaded sector this long is brought to the other
	int   p2Hero        = 1;      // player 2's hero: 0 Asterix, 1 Obelix, -1 the one player 1 is not leading with
	bool  noLock        = true;   // the level never takes a player's hero (waits, walks to markers); cable cars still do
	bool  noLockExceptCorkscrews = true;   // no-lock mode: a corkscrew's circle (light pillar launching its hero) still locks it
	bool  noLockExceptBeams = false;   // no-lock mode: a rotary beam's circle still locks its hero, through the ride and after
	bool  carRiderLeads = false;      // a hero boarding a cable car while the other leads takes the lead at once
	bool  romansInView  = false;      // every spawn marker of a squad in a player's view: its Romans come anyway (farthest marker)
	float deadzone      = 0.24f;

	// Co-op difficulty ([coop_difficulty]; read at startup)
	bool  coopOnly         = true;  // squads and combo bar: only while player 2 plays (single player: the game's own)
	bool  rulesStatic      = false; // ... on all the time in co-op, not the game's own while the heroes are apart
	bool  apartByPath      = true;  // the heroes are apart too where neither can walk to the other (pathfinding)
	bool  toDefeatStays    = false; // a fight begun with the heroes apart keeps the level's Romans to defeat
	float squadAliveFactor = 2.0f;  // Romans of a squad alive at once
	float squadCountFactor = 2.0f;  // Romans of each squad pool (phase) to defeat
	float priceFactor      = 2.0f;  // the merchant's prices
	float comboFactor      = 0.5f;  // combo bar hits counted per Roman hit
	float comboDrainFactor = 1.5f;  // the combo bar losing its hits (before it is full)
	float comboFullDrainFactor = 1.5f; // a full combo bar running out
	bool  noLockRomans     = true;  // no-lock mode: Romans come for a hero the level would have kept waiting ([coop_difficulty.experimental])
	bool  noLockRomansAnyMarkers = false; // ... at any squad's spawn markers near it, not only its own squad's

	// Keyboard as player 2's controller ("pad" slot kKeyboardPad): virtual-key
	// codes, [keyboard] section of the ini.
	bool  keyboard      = true;
	enum { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_LOOK_UP, KEY_LOOK_DOWN, KEY_LOOK_LEFT,
	       KEY_LOOK_RIGHT, KEY_JUMP, KEY_ATTACK, KEY_GRAB, KEY_DASH, KEY_LB, KEY_RB, KEY_START,
	       KEY_BACK, KEY_CENTER_CAMERA, KEY_COUNT };
	int   keys[KEY_COUNT] = { 'W', 'S', 'A', 'D', 0x26, 0x28, 0x25, 0x27, 0x20, 'J', 'K', 'L', 'U', 'O',
	                          0x0D, 0x08, 'M' };

	void Load(const char* iniPath);
};

extern CoopConfig g_config;
