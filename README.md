# Co-op mod for Asterix & Obelix XXL (2004, PC)

Adds LEGO-TT-style drop-in co-op to the original PC release: player 2 presses a button on a second
controller and takes over whichever hero player 1 is not currently driving.

The game has no source code, so this is a runtime patch. It is built as **`xxl_coop.asi`**, loaded
by the Ultimate ASI Loader that already ships in the game folder as `dinput8.dll`. That keeps it out
of the way of the QoL patch (`d3d9.dll`) and the XXL Inspector, both of which want to be the
`d3d9.dll` proxy.

---

## Status — read this first

| | state |
|---|---|
| Player 2 drives the other hero (move, jump, attack, grab, dash, swim) | **working**, verified with two real pads |
| Join / leave: Start on the controller the game is not using; Start again or hold Back | **working**; player 1's pad is detected automatically |
| Players keep their heroes through blue circles / scripted switches | implemented |
| Scripted sections (rides, belly slide, lifters started by P1) | co-op pauses, game handles them, resumes after |
| Cutscenes (letterbox bars) | co-op pauses (P2's hero to the AI, one screen), resumes after — automated test |
| Split-screen (stacked or side by side), shared HUD, P2's own camera (right stick) | **working**, verified with two real pads |
| P2's hero in the world: Romans attack him, dropped helmets come to him | **working**, verified with two real pads |
| P2: springs, iron crates, grabbing/spinning Romans, helmets placed in the air | fixed, verified by automated tests; awaiting a real two-pad session |
| Crates / placed helmets around P2 when P1's camera looks away | fixed (both cameras' frustums), awaiting a real two-pad session |
| Swing (push/pull) for P2's Obelix | untested |
| Keyboard as a controller (P1 pad + P2 keyboard, P1 keyboard + P2 pad, both on one keyboard) | **new**, automated test (`kbd`) |
| Sound around P2 (footsteps, crates) audible: range checks and 3D mix use the nearer camera | **new**, automated check only |
| Trigger zones bound to one hero fire for either player (Romans near the lifter for P2 alone) | **new**, automated test (`lifter2`) |
| R3 + A: warp to your partner (both players) | user-verified (P2); automated test (`warp`) |
| Roman squads wake up and chase the non-leading player too | automated test (`pursuito`, `pursuitlead2`) |
| Non-leading player 1 (after the belly slide): launchers, helmets, grabbing, sound, crates | fixed (the non-leader's tick acts as leader, whichever player); awaiting a session |
| Blue circle stepped on by P2 alone; P1 camera in the cable car | changed, awaiting a real session |
| Crash at axxl.exe+0x9C228 (lifters) | fixed; crashes and stalls are now logged with their location |

How it works is in `docs/RESEARCH.md` §0. Automated tests: `tools/run_test.ps1 <scenario>`
(scenarios in `src/autotest.cpp`). Decompiled game code for reference: run
`ghidra_scripts/rebuild_decomp.ps1 -Import` once, then grep `decomp/`.

---

## Building

Needs the **32-bit** VS 2022 Build Tools toolchain (the game is an x86 binary).

```
coop\build.bat
```

produces `coop\build\xxl_coop.asi`.

## Installing

Copy into the game folder next to `axxl.exe`:

* `xxl_coop.asi`
* `xxl_coop.ini`

Both are already installed in `Game Files\`. The mod writes `xxl_coop.log` there.

With DSOAL as the sound wrapper, also copy `xxl_timerfix.asi` (`coop\timerfix\build.bat`). It is
separate from the mod. Without it the game runs at ~45 fps instead of 62: the frame limiter's `Sleep`
needs a 1 ms timer, which IndirectSound's XAudio2 requests but DSOAL doesn't.

`xxl_widescreen2d.asi` (`coop\widescreen2d\build.bat`), also separate, draws 2D text (dialogues,
hints, menus, the font's button icons) at its true shape: the game widens it by aspect / (4/3), a third
too wide at 16:9. It also draws the HUD's 3D models (health shields, helmet, combo columns) without
the stretch, in their places; a row of them (the shields) closes up towards its outer end. The HUD's
see-through plates are drawn narrower about their anchored edge, and what sits on them comes in with them.
It also swaps the text font's page for a 4x sharper one (Arial Bold): copy the folder
`coop\widescreen2d\fonts` PNGs into `Game Files\xxl_widescreen2d\` (one per language pack; without them the
font stays as it is).

## Controls

| | |
|---|---|
| Start on a second pad (or Enter on the keyboard) | join as Obelix (ini `p2_hero`) |
| Hold Back for 1.5 s (Backspace) | leave co-op |
| Hold R3 (P2 also Back / Backspace) + A | your hero jumps to your partner's side |
| Hold R3 + B | take your hero back when the game keeps it waiting, or reset a hero that cannot move |
| Hold R3 + Y / R3 + X (`cheats = on`) | fill the magic potion / make your hero the leader |

Player 2's keyboard keys are in the `[keyboard]` section of `xxl_coop.ini` (default: WASD move,
arrows camera, Space jump, J/K/L attack/grab/dash, U/O shoulder buttons). While player 2 plays on
the keyboard their keys are kept from the game, so one keyboard can serve both players when the
keys don't overlap (the ini has a numpad layout for that).

Player 1 keeps using whatever the game is already configured to use (keyboard or pad) — the mod does
not touch player 1's input path, so the QoL patch's right-stick and mouse options keep working.

Co-op refuses to arm when it would break the game: before the Obelix reunion in level 1 (the class
genuinely has zero instances until then, which the mod checks), while either hero is flagged busy
(`hero+0x390 & 4` — belly ride, boat, shop, cutscene), and across level transitions, which drop
player 2 automatically.

---

## How it works

`docs/RESEARCH.md` is the real documentation. The short version:

* Heroes are `CKHkAsterix` / `CKHkObelix` hooks, managed by a `CKGrpTrio` group that stores which
  one is the **leader** (player-driven) and which is the **companion** (AI-driven).
* Each hero is ticked once per frame by its *Life* object. The mod hooks
  `CKHkAsterixLife::tick` and `CKHkObelixLife::tick` by swapping one vtable entry each — reversible,
  and it cannot collide with a byte patch another mod put inside the same function.
* Both hero update functions read a **single shared command block** at `gameManager+0x40`, and a
  hero only consumes it when its own `+0x1A` byte is zero. That is what makes two-player control
  tractable: clear `+0x1A` on both heroes, then swap the block's contents between the two Life ticks
  so each hero sees its own player's input. The wrappers to do that are already installed
  (`CoopBeforeHeroTick` / `CoopAfterHeroTick`); they just have nothing useful to write yet.
* The companion's behaviour is an AI order byte at `hero+0x564`, and the engine kindly left the
  developers' French debug names in the binary, so the whole order table is known — `0` is
  *Suivre Leader*, which is Obelix trailing Asterix and starting fights on his own.

### Finishing per-player input

The blocker is that only four fields of the command block are identified (movement vector, heading,
control nibble, AI-override byte). The buttons are somewhere in the rest of it.

To decode them: set `diagnostics = on`, play, and diff the hex dumps in the log across frames where
you press exactly one button. The dump already covers `0x00`–`0xBF` of the block. Note that the
engine uses sentinels for "no change" — `0xA` nibbles in `+0x1C`, `-1` in `+0x5C` and `+0x8C` — so
a synthesised command must fill those in for every field it is not driving.

### Finishing split-screen

`CalcProjCamera` at `0x414B70` is the per-frame camera projection setup and is a known-safe hook
point (both the QoL patch and the Inspector already replace it), so per-viewport aspect ratio has a
home. What is still missing is the "render the 3D scene once" call to run twice.

The RenderWare 3 camera entry points are identified (`docs/RESEARCH.md` §7), including
`RwCameraSetViewOffset` at `0x58FD40`, which the engine already uses — sub-rasters plus a view
offset is the standard RenderWare way to do split-screen, so the pieces are there.

**Careful:** the QoL patch overwrites `0x414B70` wholesale. Anything the co-op mod does there has to
chain rather than clobber, or widescreen FOV breaks.

---

## Tooling

`ghidra_scripts/` holds the headless Ghidra helper used for all of the analysis:

```powershell
.\ghidra_scripts\gh.ps1 "decomp 0x414B70 | vtbl 0x625C90 14 | strings CKHkObelix"
```

Commands: `decomp`, `xrefs`, `callees`, `scanimm`, `strings`, `strtab`, `vtbl`, `sym`, `imports`.
Chain them with `|`. It expects the analysed project the first run created under the scratchpad
directory; re-import `axxl.exe` with `analyzeHeadless` if it has been cleaned up.

## Notes on the game folder

* `force_level` in `xxl_coop.ini` boots straight into a level. Prefer it over the QoL patch's
  undocumented `initial_level` key — **`initial_level` crashes this build of the game** (verified:
  it access-violates on startup with the co-op mod removed as well).
* The mod refuses to patch anything if `axxl.exe` is not loaded at `0x400000` or if the two Life
  vtable slots do not hold the values this build expects, so a different game version will make it
  log and stand down rather than corrupt memory.
