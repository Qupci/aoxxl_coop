# Co-op mod for Asterix & Obelix XXL (2004, PC)

Adds LEGO-TT-style drop-in co-op to the original PC release: player 2 presses a button on a second
controller and takes over whichever hero player 1 is not currently driving.

The game has no source code, so this is a runtime patch. It is built as **`xxl_coop.asi`**, loaded
by the Ultimate ASI Loader that already ships in the game folder as `dinput8.dll`. That keeps it out
of the way of the QoL patch (`d3d9.dll`) and the XXL Inspector, both of which want to be the
`d3d9.dll` proxy.

---

## Status — read this first

This is a **partial implementation**. The engine has been mapped and the plumbing is in place and
verified against the running game, but the two headline features are not finished.

| | state |
|---|---|
| ASI loads, hooks install, engine reachable | **done, verified live** |
| Address/offset map for heroes, trio, AI orders | **done, verified live** |
| Per-frame hero hook (inline detour on the state-machine tick) | **done, verified live at 63 fps for both heroes** |
| `CKInput` action array located and mapped to pad inputs | **done, measured over ~13k frames** |
| Second-controller detection, join / drop | **done**, not yet exercised with a real pad |
| Hero switching from pad 2 (same thing the blue circles do) | **done** |
| Guards for "Obelix not in the level yet", cutscenes, belly/boat rides | **done** |
| Player 2 driving the second hero | **written, completely untested** |
| Split-screen rendering | **not started** — needs the scene-render call located |

Everything marked "verified live" was checked by booting into level 2 with `force_level` and reading
`xxl_coop.log`. The input mapping came from probe sessions played by the project's owner.

**Player 2 control has never actually been run.** The code path is complete and the mapping under it
is measured rather than guessed, but nobody has yet pressed Start on a second pad with this build.
Expect to iterate.

There is still only one camera, so player 2 can walk off the edge of the screen. Split-screen is the
next piece.

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

## Controls

| | |
|---|---|
| Start on a second pad | join / take over the other hero |
| Hold Back for 1.5 s | leave co-op |

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
