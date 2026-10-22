# Asterix & Obelix XXL (2004, PC) — reverse-engineering notes for the co-op mod

Target binary: `Game Files/axxl.exe`
MD5 `6953FB1930610B2D0420550479C0A40D`, SHA1 `EE7D6A20B9C07ABB591CF5E36DF50EF200B32033`
Image base `0x400000`, no ASLR. Engine: **Kal** (Etranges Libellules) on top of RenderWare 3
(sections `_rwcseg` / `_rwdseg` are present).

Everything below is either (a) taken from AdrienTD's `xxl-inspector` / `XXL-Editor` sources that
are checked out next to this folder, or (b) recovered with Ghidra from `axxl.exe`. Each entry says
which. **Nothing here has been confirmed against a running game yet** — the runtime diagnostics in
the mod (`coop_diag`) exist to do exactly that.


> **Session 2 (Oct 2026) — read §0 first.** The hero-control model in §5–6 below is from session 1
> and is **wrong**: it hooked the body/AI machines, a layer below where player-vs-AI is decided.
> §0 is the verified model (autotest + real two-pad tests). Function names refer to the Ghidra
> corpus in `coop/decomp/` (regenerate with `ghidra_scripts/rebuild_decomp.ps1`).

## 0. Session 2 — verified model

### 0.1 Hero control: the Life layer
* Every hero hook has a Life (`hero+0x0C`; `life+0x10` = hero). Its per-frame tick is **Life vtable
  slot 17** (`CKHkAsterixLife` vt `0x625C90` → `0x4AA9C0`, `CKHkObelixLife` vt `0x625D18` →
  `0x4AB7E0`), verified at one tick per hero per frame.
* Life state (`life+0x1D`) = `(hero+0x18 >> 1) & 0xF`, handlers = Life vtable slots 24–33
  (state 0–9). **Even = player variant, odd = AI variant**, of 5 modes: 0 normal, 1/2 rides &
  ballista, 3 belly slide/cinematic (`0x480B–F`), 4 **swimming** (hero-driven). Bit 0 of `+0x18`
  (companion flag) always equals the state's low bit: `(f18 & 3) == 0` ⇔ player-driven.
* Modes 0 and 4 have player and AI handlers for every hero; modes 1–3 have a real handler for one
  hero only (single-player sections).
* Player handlers (e.g. `CKHkAsterixLife::State0_Player` `0x4AAAE0`) call `Hero_ProcessPlayerInput`
  `0x499660` (`GetAction` 0–3 stick, 4 A/jump, 5 X/attack, 6 B/grab, 7 Y/dash, 0x10/0x11/0x13/0x14).
  AI handlers call `Hero_TickAiMachine` `0x49DE10` (AI state `+0x561`, order `+0x564`).
* Input: `GetAction(view, id)` = `CKInput.values[view.remap[id]]`; both heroes share the view at
  `trio+0x28`; `CKInput` = `yellowPages+0x14`, values pointer at `CKInput+0x1C`.
* **Co-op mechanism:** hold P2's hero in the player variant (playable modes only) and swap
  `CKInput+0x1C` to P2's array for the length of that hero's Life tick.

### 0.2 Trio rules (`CKGrpTrio::sendEvent` `0x47AC10`, `0x48xx` → `CKGrpTrio_OnEvent48xx`)
* `0x4800/0x4801` (and `0x48B7/0x48B8` instant): refused unless the target hero has its companion
  bit set and the leader isn't busy (`+0x390 & 4`); sets target `ctrl-1`/bit0=0, other `ctrl+1`/
  bit0=1, other's order 0, `trio+0x2C/+0x2D`.
* `0x4806`: all heroes back to mode 0 (`ctrl = bit0`). `0x4807`: mode 1. `0x4809/0x480A`: mode 2.
  `0x480B–F`: switch + mode 3. `0x4802/0x4803`: companion wait/follow.
* ~25 trio virtuals answer "the leader …" (vf25–37, 42, 52–57) and 5 "the companion …" (vf38,
  43–45, 47); hero code also reads `trio+0x2D` directly. **Player-side code assumes it runs on the
  leader.** Co-op therefore time-slices `trio+0x2C/+0x2D` during P2's Life tick.
* Per-frame `CKGrpTrioLife::vf17` `0x484400`: leader position sync, move basis
  (`CKGrpTrio_BuildMoveBasis` `0x4815C0`, from camera heading + leader stick), music triggers. It
  does not rewrite control bits.

### 0.3 Group actions (situation 13) and the lifter crash
* A trio event (family handler `0x482610`, case `0x30`) gives the **leader** `+0x544` = lifter and
  `+0x3F0 |= 0x4000` → `Hero_ChooseSituationState` `0x49C900` picks situation `0xD` →
  `FUN_0049C030`: sends companion/Idefix to markers (`hero+0x548[3]`), waits for AI state 3, and
  after 17 s forces the companion's AI state to 3.
* Crash `axxl.exe+0x9C228`: forced state 3 on a *player-driven* companion never clears; the next
  lifter empties that hero's marker slot and dereferences it. Fixes: trio time-slice, clear
  leftover AI states the way the game does (`table[st](2, 0xFF)`, `+0x561 = 0xFF`), and suspend
  co-op while player 1's hero runs a group action.

### 0.4 Rendering and split-screen
* Per-frame render: **`CKGraphic` (0,2) vtable `0x623D54` slot 17** `0x431A40` (bumps frame id
  `0x6464C4`) → `FUN_00413EC0(view = CKGraphic+4)`: `FUN_00413DC0` camera sync →
  `CSGRootNode` slot 14 scene walk into 15 lists at `view+0x1C` (culls with
  `RwCameraFrustumTestSphere(g_mainRwCamera)` `0x58FE70`) → `FUN_00422D10` → clear
  (`FUN_00414700(renderer,0)`) → `FUN_00413F40` 11 world lists → `Render_View` effects list
  (`view+0x48`) → `FUN_00414070` HUD (2D camera, lists `view+0x4C..`) → `FUN_00414780` present.
* Lists are intrusive (`item+0x24`); slot 1 add, slot 3 draw-and-empty.
* RW: `RwCameraBeginUpdate/EndUpdate` jump through `camera+0x18/+0x1C`; `RwCameraSetViewWindow`
  `0x58FFB0` (cdecl) only marks the frame dirty. D3D9 device `*(void**)0x668978`; RW sets the
  viewport from the camera raster (`width +0xC`, `height +0x10`, `nOffsetX/Y +0x1C/+0x1E`).
* Camera frame is a root RwFrame (modelling `+0x10`, LTM `+0x50`, pos at matrix `+0x30`).
* Split-screen = `splitscreen.cpp` (see its header comment).

### 0.5 World interactions for the non-leader hero (verified by autotest, Oct 2026)
* **Collision service** `CKSrvCollision` = `yellowPages+0x50`, tick slot 17 → `FUN_00447E30`
  (shape-pair tests) + `FUN_00447FF0` (projectiles). Tests: array `+0x1C8` (count u16 `+0x1DC`,
  0x1C bytes: shape1, shape2, u16 receiver idx ×2, next-on-hit `+0xC`, next-on-miss `+0x10`, parent
  `+0x14`, flags u16 `+0x18`, priority `+0x1B`); active list `+0x1BC`, inactive `+0x1C0`; receivers
  `+0x1CC` (12 B: contact list, hook, next). A shape's owner hook is at `shape+0x20`; shape type =
  vf17; handler table `+0x110C[type2 + type1*6]` (cdecl `(s1, s2, out)`).
* Hits are delivered as `receiver->sendEvent(0x300, list)` (entries `{next, test}`, call site
  `0x447FD1`) and projectile hits as `0x301` (entries `{next, u8 rec, u8 projectile, u16 target,
  u8 sub}`, projectile shape at `srv+0x24+8*i`; call sites `0x4483C5` targets, `0x448405` owners).
  Objects accept only the **trio leader's** contacts: `CKHkBumper::sendEvent` 0x4BB4C0 checks
  `shape->owner == trio->vf25()` (leader hook) then uses `vf35` (leader speed) and sends `0xC0B`
  (launch) to the trio; the iron-crate filter `0x4B0DF0` breaks only if the projectile owner is the
  leader and `trio->vf40()` (leader is Asterix) is false. Fix: `proximity.cpp` slices the leader
  around those three call sites when the contact is player 2's alone.
* **Swept spheres**: `CKDynamicBoundingSphere` / `CKDynBSphereProjectile` (vt `0x622DE8`/`0x622E30`)
  slot 14 = scene-walk visit `0x416440`: prev pos `+0x30`, motion `+0x24`, current `+0x3C` (from
  the frame LTM). The OBB test `FUN_00440F10` sweeps along the motion; zero motion falls back to
  a static overlap. Every node of the scene graph is visited on every walk, so split-screen's
  three walks zeroed every sweep — only the first walk of a frame may update them.
* **Hero yaw**: facing `+0xB4`, yaw pair `+0xCC/+0xD4` = atan2(z, x) in [0, 2π) (`FUN_0048C970`).
* **Bonus beacons** (helmets placed in the level): `CKGrpBonusPool::vf27` spawns a pool bonus on
  every uncollected beacon of the live klusters each frame, and attracts it to the leader if
  within the pool radius with `bonus->vf18(hero, &beacon)` (marks the beacon collected). Patched
  check at `0x54C6C6` adds player 2's hero.
* **Beacon klusters**: `FUN_0046DED0(handler, flags, &outside, ref)` (thiscall on
  `yellowPages+0x78`, ret 0x10) — walks the current sector and sector 0 (`yellowPages+0x90`,
  `FUN_0046E240`), klusters at `sector+0x34` (center `+8`, radius `+0x14`, per-handler entry
  `+0x1C/+0x20`); flag 1 = frustum test against camera service planes `+0x500..+0x54C` (far + 4
  sides, outward normals; built by `FUN_00457170`), flag 2/8 = sort by Manhattan distance to
  `ref` or camera `+0x5B0`. Crates (`CKCrateCpnt::vf13`, flags 7) and bonus beacons use it →
  re-implemented with both players' frustums (`klusters.cpp`).
* **Cutscenes**: `CKSrvCinematic` (`yellowPages+0x80`) shows letterbox bars when a playing scene
  has `+0x30` bit 0; `+0xC` bit 0 = bars visible (recomputed every tick). During level 2's scene 1:
  game manager `+0x5C` bit 20 set (HUD hides on it), camera service camera = plain `CKCamera`
  (vt `0x6245BC`), and **no hero Life ticks**. Scene event `0x500+i` starts scene i,
  `0x500+count+i` stops it. Ambient scenes (no bars) play all the time.
* **Trigger zones**: `CKSrvDetection` (`yellowPages+0x54`, vt `0x62504C`, tick = slot 17 `0x46BE90`)
  tracks two objects (`+0x18` nodes, `+0x20` count: Obelix's node 0, Asterix's node 1) and their
  last positions (`+0x1C`, 12 bytes each, written at the end of the tick). Tests are 8 bytes
  (`+0` shape, `+2` object, `+4` bit 0 enabled, `+5` bit 0 "inside" wanted, `+6` event) in five
  lists: `+0x2C` box-enter, `+0x34` sphere-enter, `+0x3C` plane-cross (→ `FUN_00460C20`, ECX =
  event service `yp+0x6C`), `+0x44` box-level and `+0x4C` sphere-level (flag set inline every frame
  while inside/outside). Shapes: boxes `+0xC` (0x18: max xyz, min xyz), spheres `+0x10` (0x14:
  centre, r, r²). All stateless (current vs last position). Level 2: 29/3/145 enter/cross tests,
  about half bound to each hero (lifter Romans: sphere-enter event `0x408`, bound to Asterix). The
  mod runs the tick a second time with the two heroes' nodes and last positions swapped (minus
  "while outside" tests), so every hero zone answers to both players (`proximity.cpp`).
* **Sound**: one listener (`CKSoundManager+4` → wrapper → object: pos `+0x28`, velocity `+0x34`,
  up `+0x40`, at `+0x4C`, right `+0x58`), copied from the camera frame's LTM by
  `CKSoundManager::vf17` (`0x43ADD0`). Range cut via `FUN_005917C0` (sqrt) at `0x43B0D5`,
  `0x43B512` (vf17) and `0x43B9B9`, `0x43BCBF` (start, `FUN_0043B730`); software 3D mix
  `FUN_005DE840(&listener, n, in, out, ...)` (volume/pan per voice, positions in world space;
  voices store x negated at `+0xA0`). The mod answers the range checks with the nearer camera and
  mixes voices nearer P2's camera against a listener copy moved there (`sound.cpp`).
* **Keyboard**: the game reads it from window messages (`0x479C10` → `FUN_00438990` → keyboard
  device vf5 `0x433900`, VK → key index table, state at device `+0x95`); CKInput devices: `+0x04`
  pads (0xCC), `+0x08` mice (0x68), `+0x0C` keyboards (0x3FC). A binding's source (`binding+0xC`)
  holds one raw-byte pointer per preset (`+4..`), `+0x14` = the preset in use. Default keyboard
  preset: arrows move, Space jump, F attack, G grab, D dash, R/C shoulders, P back, I start, Shift/
  Ctrl camera. Without a pad the game opens on "No controller detected" (Enter dismisses it).
* **Sectors**: sector manager `yellowPages+0x90` (0x14C bytes): sectors `+4` (count `+0x20`),
  current `+0x24`. Sector `+4` index, `+6` flags (bit 0 loaded), `+0x18` box max xyz, `+0x24` min
  xyz. Collision (`FUN_004493C0`), klusters and rendering walk only the current sector and
  sector 0 — one streamed sector at a time. Level 2: 11 sectors, 0 and 1 loaded at the start.
  Teleports skip the zone events that stream sectors; autotest `GLIDE` moves heroes frame by frame.
  The sector manager is the level object (game manager `+0x20`): writing a sector number to `+0x34`
  streams it in (XXL Inspector's "go to sector"; autotest `LOADSECTOR`). Game speed: game loop
  `+8` → time manager `+0x10` (float).
* **Magic potion**: one global gauge, `[gameManager(yp+0x8C)+0x40]+0x9C` (float, 300 = full,
  drains ~1/s; `+0x52` a byte the trio reads). `CKGrpTrioLife::vf17` → `FUN_0047DE80` starts the
  trio's potion effects when the gauge is > 0 (`trio+0x5AC`), `FUN_0047EC30` per frame,
  `FUN_0047FA90` ends them; game manager event `0x5302` empties it. The QoL patch's
  `infinite_potion` rewrites 300.0 after every trio tick (`0x48445B`) and NOPs the clear at
  `0x4FEDA4`. With the potion Asterix's `+0x84` (run speed) reads 15, P2's Obelix 7: the player
  variant of Obelix has no potion code (single player never has Obelix drink).
* **Spinning platform**: the platform and its blue circle are `CKHkCorkscrew` (2,44, vt
  `0x628E0C`); it asks the trio about the leader standing on it (trio events `0x0B01`, `0x0C09`) and
  then sends "switch to Obelix, Asterix waits" (`0x4801`/`0x4802` via CKSrvEvent). Level 2's six
  `CKHkPressionStone`s are elsewhere. Excluded from proximity slicing (only the real leader).
* **Roman squads**: one `CKGrpEnemy` group (Life `CKGrpEnemyLife`, vt `0x628938`, slot 17 →
  `FUN_0052AC70`) ticks every squad (group `+0x0C`, chained `+4`, isSubclass `0x604`): `FUN_0052D340`,
  then the squad AI (vf31 `+0x7C`, activates via vf33 on first run) only while the trio leader's
  position (trio vf27 `+0x6C`) is inside the squad box (`+0x11C..+0x124` max, `+0x128..+0x130` min),
  else vf31+vf35 if `+0x152` bit 1, else idle time `+0x138`. Level 2 has 57 squads, boxes large
  enough to hold both heroes. The mod runs a squad for the non-leading player (trio sliced, enemy
  cache dropped) when only that hero is in the box, or both are and it is nearer the box middle.
* **Launchers / lifters and the non-leader**: run unsliced, the non-leading player's tick sent
  itself to "the companion's" marker (`0x4804` → AI order 1, situation 13); the next launcher by
  the leader then crashed at `axxl.exe+0x9C228`. The trio is now sliced around the tick of
  whichever player hero is not leading (was: only player 2's).
* **Event service groups** (`CKSrvEvent`, `yp+0x6C`, tick `0x46B180`): table `+0xC` (count of
  targets, mask, flags per group); the first `+0x1C` groups fire on any flag (OR), the next `+0x1E`
  when flags == mask, kept across frames, the last `+0x20` when flags == mask within one frame.
  Targets `+0x10` (objects), messages `+0x14` (u16), both in group order; each gets
  `sendEvent(message, 0)`. Level 2 group `0x10D` (detector events `0x868..0x86B`, two crossings
  per hero): `CKLevel` `0x1228` (stream sector 8, deferred switch `FUN_0040A830(0xCB, ...)`), a
  `CKManagerEventStopCinematicBloc` `0x500`, trio `0x48ED`/`0x487D`/`0x487E`. Group `0x14D` (`0xA68`):
  game manager `0x5302` (empties the potion). `CKLevel` messages `0x12xx`: `xx>>5 == 0` request
  sector `xx&0x1F`, `== 1` request and switch.
* **Checkpoints** (the "music" the user heard in sector 8 is a checkpoint's jingle):
  `CKGrpAsterixCheckpoint::vf29` (`0x536DD0`, `ret 4`) walks the checkpoint beacons (beacon query
  handler `0x16`) and, with the leader (trio vf27) within radius `+0x24` and trio vf57 true (leader
  `f18 & 0x1E == 0`), stores the position (`+0x28..`), and on a new checkpoint index (profile
  `+0x5F`) flags `gameManager+0x332+2n`; level 2's sector-8 checkpoint flags group `0xC1`, whose
  targets show the `CKHkLightPillar` at (42 57 −120). The pillar plays sound `0x50` itself once
  its shared effect nodes (`gm+0x18 → +0x9C/+0xA0`, scene nodes: `+0xC` = parent) are free
  (`CKHkLightPillar::vf15` state 3 → `FUN_0053AE70`). The mod excludes the checkpoint group from
  proximity slicing (it was sliced to whichever hero stood nearest its *first* checkpoint) and runs
  vf29 a second time with the non-leading player as leader.
* **Sound manager messages**: `0x01xx` → `FUN_0043C940`, `0x02xx` → play list sound `xx`
  (`FUN_0043C010` → `FUN_0043B730`), `0x19xx` → stop. Streams start in `FUN_0043CC20` (cinematic
  stream blocks, `CKSekens`). Diagnostics in the mod log every first (sound, caller) start.
* **Diagnostics**: `WatchBytes()` (crash.cpp) sets hardware write watchpoints on the game thread
  and logs the writing instruction plus a stack scan — found the checkpoint flagging in one run.
* **Potion dash**: while game manager `+0x5C` bit 19 is set, `Hero_ChooseSituationState`
  (`0x49C900`) sends any player-driven hero on the ground into situation `0x10`; it ends only by the
  hero's `+0x388` bit `0x4000`, which only the drinking hero gets. Hidden from the non-leader's tick.
* **Group actions started by the non-leader** (launchers): the sequence asks the trio for its leader
  outside the activator's tick too; the activator is made the real leader (raw `trio+0x2C/+0x2D`).
* **Situation `0x0F`** (`+0x3F0` bit `0x20000`, e.g. the level-2 boss after Obelix's hit): left by
  becoming the AI companion. A player's hero losing the lead in a non-ground situation is passed
  through the AI variant for two ticks (also R3 + B). A companion stuck in a ride mode (launcher
  flight `f18=05`, kept by the save) is put back to `(f18 & 0xE0) | 3` when player 2 presses Start.
* **Belly slide** (`0x480B` → `0x4801`, mode 3): Obelix leads, Asterix rides as the AI variant
  (`f18=0F`); the player variant (`0C`) keeps him at the top. Both heroes' `+kHero_pos` stay frozen
  during rides (cable car, slide) while their nodes move — cameras must follow the node (LTM `+0x80`).

### 0.6 Other facts
* `CKSrvCamera` = `yellowPages+0x68`; dir `+0xAC`; targets `+0xE8`.
* Level survey (levels 2–6): lifters in 2, 3, 4, 5; levels 7–8 have no co-op-ready start.
* The game window pauses on `WM_ACTIVATEAPP` focus loss (`Game_WndProc` `0x479C10`).
* `CKGrpTrioLife::vf12` runs a CRC16 over a code range (anti-tamper, `0x645A10..18`): avoid
  byte-patching code inside it; all co-op hooks are vtable/function-pointer swaps.

---

## 1. Object model

Every engine object derives from `KClass`, whose first ten vtable slots are fixed:

| slot | offset | method |
|---|---|---|
| 0 | 0x00 | destructor |
| 1 | 0x04 | `isSubclass(int clgrp \| clid<<6)` |
| 2 | 0x08 | `reset` |
| 3 | 0x0C | `getClassGroup` |
| 4 | 0x10 | `getClassID` |
| 5 | 0x14 | **`sendEvent(int event, void* param)`** |
| 6 | 0x18 | unknown |
| 7 | 0x1C | unknown |
| 8 | 0x20 | `serialize` |
| 9 | 0x24 | `deserialize` |

*(source: xxl-inspector `global.h`)*

Classes are identified by a **(class group, class id)** pair. Groups 0..14 are
`Managers, Services, Hooks, Hook Lives, Groups, Group Lives, Components, Cameras, Cinematic blocs,
Dictionaries, Geometry, Nodes, Game things, Graphics things, Errors`.

Classes register themselves at startup through

```
FUN_0047A240(clgrp, clid, ?, allocOne, ?, allocArray, ?, "CKClassName")
```

`0x47A274` (inside that function) is where xxl-inspector hooks to harvest class names. Because the
name string is an argument, **every class name is a plain string in the .rdata section**, and the
xref from the name gives you the registration site, from which `allocOne` gives the constructor and
the constructor gives the vtable. That is how all the vtables below were found.

### Finding instances at runtime

```
kcls  = *(void**)(yellowPages->loadingManager + 4)
grp   = *(void**)(kcls + 12*clgrp)
cont  = (ClassContainer*)grp + clid
```

`ClassContainer` is packed: `objects@0, flags@4, u1@8, u2@0xC(u16), count@0xE(u16), clSize@0x10(u16),
pl@0x12(u8)`. Instances are inline (`pl != 0`), a single object (`flags>>17 == 1`), or an array of
pointers. *(source: xxl-inspector `gui.cpp: GetLvlObject`)*

`count == 0` is the normal state for a class with no instances — this is how you detect that
**Obelix does not exist yet** in the opening section of level 1.

---

## 2. Globals

| address | meaning | source |
|---|---|---|
| `0x6621F4` | `CKYellowPages**` — the service locator | inspector |
| `0x665310` | `SGameStartInfo*` (`hwnd`, width, height, fullscreen) | inspector |
| `0x479C10` | the game's own `WndProc` | inspector |
| `0x479E8E` | immediate operand holding the WndProc pointer in `RegisterClassA` | inspector |

`CKYellowPages` fields used here: `kclInfos@0x1C`, `gameLoop@0x30`, `loadingManager@0x44`,
`srvCollision@0x50`, `+0x68` (used by the trio switch), `srvBeacon@0x78`, **`gameManager@0x8C`**.

`CKGameManager`: `currentLevel@0x04` (`-1` while in menus), `nextLevel@0x08`, `levelObject@0x20`,
and **`+0x40` → the hero command block (§5)**.

---

## 3. Classes that matter

| class | (grp,id) | instance size | vtable | notes |
|---|---|---|---|---|
| `CKInput` | (0,4) | — | — | one per mapped action; registered at `0x435D20` |
| `CKSrvCamera` | (1,3) | — | — | `pos@0x88`, `dir@0xAC`, target nodes@`0xE8` |
| `CKHkAsterix` | (2,28) | `0xC78` | `0x625A8C` | ctor `0x4A22A0` |
| `CKHkObelix` | (2,29) | `0xBF8` | — | ctor `0x4A60E0` |
| `CKHkIdefix` | (2,30) | — | — | |
| `CKHkInGameInterface` | (2,132) | — | — | the HUD; `goVanish` = `0x515CB0` |
| `CKHkAsterixLife` | (3,27) | `0x20` | `0x625C90` | |
| `CKHkObelixLife` | (3,57) | `0x20` | `0x625D18` | |
| `CKGrpTrio` | (4,12) | `0x720` | `0x6256A0` | the hero manager |
| `CKGrpBonus` | (4,48) | — | — | |

---

## 4. `CKGrpTrio` — the hero manager

This is the class the whole co-op mode hangs off. Layout recovered from its `sendEvent` handler
`FUN_00482BB0`:

| offset | meaning |
|---|---|
| `+0x2C` | byte — **leader** index (the hero the camera and HUD follow) |
| `+0x2D` | byte — **companion** index (the AI-driven hero) |
| `+0x30` | `CKHkAsterix*` |
| `+0x34` | `CKHkObelix*` |
| `+0x38` | `CKHkIdefix*` |
| `+0x38C` | float — switch proximity radius (squared against hero distance) |
| `+0x48` | object that receives `0x4A00`-family notifications; assigned by event `0x4901` |
| `+0x3F4` | flags: `0x4` in-switch-zone, `0x8`, `0x100`, `0x200`, `0x400`, `0x800` |

`sendEvent` dispatches on `(event >> 8) & 0xFF`; `0x48` lands in `FUN_00482BB0`, which switches on
the low byte. The interesting sub-events:

| event | effect |
|---|---|
| `0x4800` | leader ← **Asterix** (`+0x2C = 0`, `+0x2D = 1`); Obelix gets order 0 |
| `0x4801` | leader ← **Obelix** (`+0x2C = 1`, `+0x2D = 0`); Asterix gets order 0 |
| `0x4802` | companion → order 3 (*Attendre* / wait) |
| `0x4803` | companion → order 0 (*Suivre Leader* / follow) |
| `0x4806` | all three heroes: control bits ← `bit0` |
| `0x4807` | all three heroes: control bits ← `bit0 + 2` |
| `0x480B`..`0x480F` | cinematic in/out; control bits ← `bit0 + 6` |
| `0x4877/0x4878` | Idefix wait / follow |
| `0x487F`..`0x4881` | set `hero+0x1A = 1` for Asterix / Obelix / Idefix |
| `0x4882`..`0x4884` | clear `hero+0x1A` for Asterix / Obelix / Idefix |
| `0x48B7 / 0x48B8` | same as `0x4800 / 0x4801` but with the "instant" flag |

`0x4800`/`0x4801` are exactly what the blue circles on the ground fire, and what xxl-inspector's
"Switch (F2)" button sends:

```cpp
trio->sendEvent(0x4800 | (*((char*)trio + 0x2D) ? 1 : 0), nullptr);
```

`FUN_00481880(newLeader, instant)` is the follow-up: if the two heroes are further apart than
`trio+0x38C`, it teleports/fades (`FUN_00459050(node, 0.0f, 1)`), otherwise it just fades in.

---

## 5. Hero layout (`CKHkHero` common part)

| offset | meaning |
|---|---|
| `+0x18` | byte. `bit0` = **this hero is the AI companion**. `bits1..4` = **control state** |
| `+0x1A` | byte. non-zero = **ignore the player command block** (i.e. this hero is on AI) |
| `+0x6C` | `Vector3` position |
| `+0xB4` | `Vector3` facing direction |
| `+0x390` | flags; `0x200` set at the end of every tick, `0x4` = busy/locked |
| `+0x3EC`, `+0x3F0` | more flag words |
| `+0x561` | byte — current action/animation state |
| `+0x564` | byte — **AI order** (table below) |
| `+0x568` | `Vector3` — AI target position |
| `+0x574` | `Vector3` — AI target direction |
| `+0x884` | flag word: `0x4`, `0x100`, `0x200`, `0x400`, `0x800` |
| `+0xB64`, `+0xB68` | child nodes touched every tick by the Life object |

### AI orders (`hero+0x564`)

Read straight out of the debug name table at `0x64CBD8` (the developers left the French names in):

| # | name | meaning |
|---|---|---|
| 0 | Suivre Leader | follow the leader |
| 1 | Aller à position | go to position |
| 2 | Sauter à position | jump to position |
| 3 | Attendre | wait |
| 4 | Frapper un ennemi | hit an enemy |
| 5 | Envoyer un ennemi | throw an enemy |
| 6 | Mordre un ennemi | bite an enemy |
| 7 | Aller à baliste | go to the ballista |
| 8 | Se suspendre à une poutre | hang from a beam |
| 9 | Utiliser téléphérique | use the cable car |
| 10 | Recevoir un coup | take a hit |
| 11 | Nettoyer chemin | clear the path |
| 12 | Sous controle extérieur | **under external control** |
| 13 | Aller à homme indice | go to the hint man |
| 14 | Aller à magasin | go to the shop |
| 15 | Aller à position sans PF | go to position, no pathfinding |

Order 0 is what makes Obelix trail Asterix and pick fights on his own; orders 4/5/6 are the combat
behaviours the companion AI selects for itself in `FUN_0049EE80`. The big companion AI state
machines live at `FUN_004F05C0` and `FUN_004F1260`.

---

### Why bit 0 is the companion flag

`CKHkAsterix::update` resets the AI order to 0 whenever bit 0 is set, `CKGrpTrio`'s switch handler
refuses to switch *to* a hero unless bit 0 is set (you can only take over someone you are not
already driving), and live values agree: while the player drives Asterix, Asterix reads `f18=00`
and Obelix reads `f18=03`.

---

## 6. The per-frame update path — where co-op has to hook

**This section replaces an earlier, wrong answer.** Static analysis pointed at the hero *Life*
objects, because slot 13 (`+0x34`) of `CKHkAsterixLife`'s vtable is `0x4AA850`, and that is the only
caller of `CKHkAsterix::update` (`0x4A3230`). Runtime profiling showed that slot firing exactly
**once per level** while the game played on at 63 fps. Two things were wrong:

* `0x4A3230` is an **enter/init** routine (it lazily builds the state-machine jump tables and applies
  the initial control state), not the per-frame update; and
* the engine **swaps a hero's Life class depending on what the hero is doing**, so
  `CKHkAsterixLife` is only one of many and is not the one driving ordinary play.

The real per-frame path, found by instrumenting every vtable slot with a counting thunk that also
records its caller:

```
FUN_0048D4F0(hero)          <- hero state-machine tick, ~63/s per hero
  |
  +- FUN_0048F720(prevState)          picks the state selector from hero+0x18 bits 1..4:
  |     ctrl 0/1 -> vtable +0x40      (CKHkAsterix: 0x4A3BA0)
  |     ctrl 2/3 -> vtable +0x44      (0x4A3DE0)
  |     ctrl 4/5 -> vtable +0x48      (0x4A3DF0)
  |     ctrl 6/7 -> vtable +0x4C      (0x4A3E00)
  |     ctrl 8/9 -> vtable +0x50      (0x4A3E70)
  |     else     -> keep current state
  |
  +- handlers[oldState](2, ...)       exit  the old state
  +- handlers[newState](1, ...)       enter the new state
  +- handlers[curState](0, ...)       run   the current state   <-- the actual per-frame work
```

Hero state-machine fields: `+0xDC` owner, `+0xE0` handler table, `+0xE5` current state index.
`FUN_0048D4F0` is called directly from **nineteen** different hook-life ticks, all in `0x4AA000`–
`0x4AD000`, which is why no single Life vtable covers it.

So the co-op hook is an inline detour on `FUN_0048D4F0`, whose `this` is the hero. Its first five
bytes are `51 53 56 8B F1` (`push ecx / push ebx / push esi / mov esi,ecx`) — exactly five, so a
`jmp rel32` does not split an instruction. Heroes are told apart by comparing `this` against the
trio's three pointers; every other hook passing through is forwarded untouched.

Verified live: `asterix=126 obelix=126` ticks per 2 s at 63 fps.

`CKHkAsterix::update` (`0x4A3230`) does, in order:

1. lazily builds three state-machine jump tables into `hero+0xDC/0x3E0/0x558`;
2. reads the **hero command block** at `gameManager+0x40`:
   * `+0x5C` (char) — if not `-1`, copied into `hero+0x1A`;
   * gated on `hero+0x1A == 0`:
     * `+0x20..+0x28` — `Vector3`, the desired movement direction, copied to `hero+0x6C`-relative
       state when non-zero;
     * `+0x8C` — float heading; `sin`/`cos` of it fill `hero+0xB4` (facing);
     * `+0x1C` — low nibble → `hero+0x18` bits 1..4, bits 4..5 → `hero+0x18` bit 0.

**This is the key finding.** There is exactly one command block, and a hero consumes it only when
its own `+0x1A` is zero. So splitting control between two players does not require finding and
duplicating the whole input system — it requires:

1. clearing `+0x1A` on *both* heroes, and
2. wrapping each Life tick so that the command block holds *that player's* pad state while *that*
   hero updates.

Which is precisely what `coop_input.cpp` is built to do.

> Not yet identified: the code that *fills* `gameManager+0x40` from the DirectInput device each
> frame. `coop_diag` dumps the block live so the remaining fields can be labelled from a real run.

### Confirmed on a live run

A diagnostic dump taken just after level 1 finished loading (mod build 2026-07-26):

```
trio @ 03BED4C0  leader=0(Asterix) companion=1(Obelix) flags=00000000 switchRadius=15.00
  Asterix @ 03B397E8 avail=0 ctrl=0 ignoreCmd=0 order=0(follow leader) pos=(0.0 0.0 0.0)
  Obelix  @ 03B38194 avail=1 ctrl=1 ignoreCmd=1 order=3(wait) pos=(55.9 2.1 106.0)
  Idefix  @ 03B3F514 avail=1 ctrl=1 ignoreCmd=1 order=3(wait) pos=(41.6 3.2 59.6)
command block @ 03BA4144  ctrl=0002AAAA move=(0 0 0) ignore=-1 heading=-1.000
```

Two things this settles:

* every offset in §4/§5 resolves to sane live values, and the AI-order table reads correctly;
* the `0xA` nibbles in `controlBits` and the `-1` in `forceIgnore`/`heading` are the engine's
  "leave this alone" sentinels — which is exactly what the `!= 10`, `!= 2` and `!= -1` guards in
  `CKHkAsterix::update` are testing. Synthesised commands for player 2 must use those sentinels
  for every field they do not intend to drive.

---

## 6b. `CKInput` — the mapped-action layer

Class `(0, 4)`, instance size `0x6C`, vtable `0x623F38`, registered at `0x435D20`. It is a
**manager** (a singleton in class group 0), not one object per binding, so `GetLvlObject(0,4,0)`
returns it directly.

| offset | meaning |
|---|---|
| `+0x04` | device array (stride 204 bytes) |
| `+0x10` | device count |
| `+0x18` | array of binding descriptors, **0x20 bytes each**; byte `+4` = binding is active |
| `+0x1C` | **pointer to the action value array — one byte per action** |
| `+0x3A` | number of actions |
| `+0x68` | "already updated this frame" latch |

Its per-frame update is vtable slot 20, `0x436360`. It first checks something on
`yellowPages+0x7C` (`FUN_0040A7F0`, looks like a window-focus test), runs each device, then:

```asm
0043643b  MOV EAX, [ESP+0x14]        ; i
0043643f  MOV EDX, [ESI+0x18]        ; bindings
00436449  SHL ECX, 0x5               ; i * 0x20
0043644e  CMP byte ptr [ECX+0x4], 1  ; binding active?
00436454  MOV EDX, [ESI+0x1c]        ; values
00436458  ADD EDX, EAX               ; &values[i]
0043645b  CALL 0x00435a50            ; CKInput binding update
```

`FUN_00435A50(char* outValue, timeInfo*)` applies the binding: reads the raw byte from the device
(`binding+0x0C` → device, `device+0x14` → raw state pointer), optionally passes it through a mapper
object (`binding+0x08`, vtable slot 2), then does edge detection using `binding+0x05` (latch),
`binding+0x06` (current), `binding+0x07` (previous), with `binding+0x10` selecting continuous vs.
edge-only and `binding+0x1C` meaning "swallow until released". It also timestamps into
`binding+0x14`/`+0x18`.

**Why this matters:** the movement command block in §6 carries direction and heading, but *not* the
buttons — `+0x1C` there is a packed bitfield of small "no change = max value" enum fields (hence the
`0xAAAA` pattern), built nibble by nibble via `FUN_00483CF0`, with no `0xAAAA` immediate anywhere in
the binary. The buttons come from this action array instead, which the hero state machines read.

So per-player input needs **both** swapped around each hero's Life tick: the command block for
movement, and the `CKInput` value array for buttons.

Live values from level 2: manager at `03B2771C`, `numActions = 59`, `numDevices = 1`, all 59
bindings active.

### Measured action indices

From a 10,000-frame probe session, aggregating the action-array diffs on every isolated pad edge
(controller preset, level 2):

| pad input | actions set | cleared on release |
|---|---|---|
| A | 4, 43, 44 | 43 |
| X | 5, 38, 45 | 38 |
| B | 6 | — |
| Y | 7 | — |
| LB | 8 | — |
| RB | 10 | — |
| Back | 12 | — |
| Start | 15 | — |

Only the "held" index of each pair clears on release (43 for A, 38 for X); the others are
edge flags that the binding logic clears by itself the next frame.

**Not all bindings are active in every level.** In level 2, indices 16–35 and 54–58 report
`binding+4 != 1`. An earlier version of this table listed D-pad mappings drawn from single
observations on those indices; they were noise and have been removed. Any co-op input synthesis
should skip inactive bindings the same way the engine's own update loop does.

**Actions 0..3 are analog movement half-axes**, carrying `0x00`..`0xFF` rather than a boolean —
they read as "always-on / analog" in the correlation report because they are non-zero whenever the
stick is off centre. Live samples show each one mirrored at more than one index:

```
[0]=87 [39]=87        [1]=68 [40]=68
[2]=E9 [36]=E9 [41]=E9    [3]=FB [37]=FB [42]=FB
```

so index `n` is duplicated at `n+39`, and 2/3 additionally at `n+34`. The engine keeps several
parallel banks of the same logical inputs (raw / edge / held variants), which is also why a single
button lights three indices.

Stick directions, from pairing 21 live samples against the action values:

| stick | actions |
|---|---|
| up | 0, 39 |
| down | 1, 40 |
| left | 2, 36, 41 |
| right | 3, 37, 42 |

### The heroes do not read this array — measured

Player 2's synthesised array was swapped in for **every one of Obelix's state ticks** (125 swaps per
2 s, verified, with correct values such as `[0]=79 [2]=E0 [36]=E0 [39]=79 [41]=E0`) and he did not
move at all.

A page-guard sampler (`guardwatch.cpp`, `watch_target = actions`) then recorded every code site
touching the array over ~30 s of play. All nine were in `0x435xxx` — **CKInput's own binding code**:

```
0043558A read   00435623 read   004354A0 read   00435542 read   004358C8 read
00435A61 read   00435A9E write  00435B11 write  00435ABC write
```

Not one access from hero code (`0x48xxxx` / `0x4Axxxx`). So this array is CKInput's internal state,
not the channel heroes consume, and swapping it can never affect a hero. **The approach in
`p2input.cpp` is architecturally wrong**; it is left in place because the measured mapping is still
correct and reusable once the real channel is found.

The sampler kept pointing back into `0x435xxx` because everything goes through one accessor rather
than touching the array directly:

```
FUN_004358B0   __thiscall uint8_t GetAction(InputView* self, uint8_t actionId)
    MOV EDX,[ECX+0x08]     ; self+0x08 -> per-owner action-id remap table
    MOV BL,[EDX+EAX]       ; remapped index
    MOV EAX,[ECX+0x0C]     ; self+0x0C -> the CKInput manager
    MOV ECX,[EAX+0x1C]     ; manager->values
    MOV AL,[EBX+ECX]       ; values[remapped]
```

### `FUN_0048ABF0` — the hero's input reader

Following that accessor's callers leads to `FUN_0048ABF0`, in the hero base-class region:

```asm
MOV ESI,ECX               ; ESI = the hero
MOV EAX,[ESI+0x14]
MOV EDI,[EAX+0x28]        ; the input view
GetAction(3), GetAction(2)  ->  hero+0x3D4 = (float)(action2 - action3)   ; left - right
GetAction(1), GetAction(0)  ->  hero+0x3DC = (float)(action0 - action1)   ; up   - down
                                hero+0x3D8 = 0
GetAction(4)                ->  folded into the flags at hero+0x3EC
```

This independently confirms the measured axis mapping (0 up, 1 down, 2 left, 3 right, 4 = A) and
gives the fields the state machine actually consumes: **`hero+0x3D4` and `hero+0x3DC`**.

It ends in a plain `RET` with no stack arguments, so it is safe to call as
`__fastcall(hero, nullptr)`.

**And it has exactly two callers, both hook-life ticks** — the ones that drive the *player's* hero.
The AI companion's life never calls it. That is the whole reason player 2's input had no effect: no
matter how faithfully the action array was populated, nothing was ever converting it into Obelix's
movement fields.

The fix is therefore to call `FUN_0048ABF0` on player 2's hero, inside the bracket where their
action values are installed.

### That was necessary but not sufficient

With the reader called on Obelix, his movement fields do get populated and they survive his tick:

```
inputview asterix=03CB73A8 obelix=03CB73A8      <- both heroes share one input view
axes      asterix=(0.0 0.0) obelix=(17.0 -254.0)
p2 axes   before=(17.0 -254.0) afterRead=(17.0 -254.0) afterTick=(17.0 -254.0)
```

Nothing overwrites them — a page guard over Obelix's `+0x380..+0x3F7` recorded no writes to `+0x3D4`
or `+0x3DC` at all. He still did not move, because his **AI action** was steering him. Comparing the
two heroes while the player drove Asterix:

```
asterix e5=16 a561=FF f18=00     <- player:    no AI action running
obelix  e5=13 a561=00 f18=03     <- companion: AI action 0, order 0 "follow leader"
```

`hero+0x561 = 0xFF` is the engine's "no state" marker — the same sentinel `FUN_0048D4F0` uses for
`hero+0xE5`. The player's hero runs with it; the companion does not. Forcing it on player 2's hero
each frame is what stops the AI competing for control.

### There are two state machines per hero

Forcing `hero+0x561 = 0xFF` did not work either: Obelix kept following normally. The reason is that
`CKHkAsterix::update` builds **three** dispatch tables, and two of them are ticked every frame by
separate drivers:

| driver | table | current state | state chooser |
|---|---|---|---|
| `FUN_0048D4F0` | `hero+0xE0` | `hero+0xE5` | vtable `+0x40`.. via `FUN_0048F720` |
| `FUN_0049DE10` | `hero+0x55C` | `hero+0x561` | `FUN_0049EE80` |

Both share the `2=exit / 1=enter / 0=run` handler protocol and the `0xFF` no-state marker, and both
take the same inline detour (identical `51 53 56 8B F1` prologue).

**Which is which — corrected.** `FUN_0048D4F0` drives the **animation** machine: its walk handler
`FUN_0048E5A0` does nothing but pick animations (`FUN_0048D650((flags & 7) + 0x0D, ...)`), which is
why `hero+0xE5` tracks walk/run/idle poses.

`FUN_0049DE10` is the hero's **main action machine** — locomotion, gravity, jumping, attacking — and
`FUN_0049EE80` chooses between *player* handlers and *AI* handlers for it. The player's whole input
sequence (the `FUN_0048ABF0` / `FUN_0048ACA0` calls and the `hero+0x384` bitfield rolls around
`0x4AB291`) lives **inside one of its handlers**: those functions end in `RET 8` and take
`(mode, prevState)`, so they are state handlers, not life ticks.

Suppressing `FUN_0049DE10` for player 2's hero was therefore exactly backwards. Observed result:

* the hero froze in whatever animation was playing when player 2 joined;
* he kept drifting on whatever velocity he had, forever;
* **gravity stopped** — pushing him up a slope left him hovering at that height;
* buttons did nothing, because attack and jump are states of that machine.

Hand-rolling the player sequence (`FUN_0049A3B0`, `FUN_0048ABF0`, `FUN_0048ACA0`, `FUN_0049A1A0`,
`FUN_004A1BF0`, plus the movement bitfields) reproduced the *animation* — the direction vector at
`hero+0x574..0x57C` tracked the stick correctly, unit length — but never the locomotion, because the
real work is in the handlers.

The correct intervention is to let the machine run and make it believe the hero is the player's,
which is what clearing bit 0 of `hero+0x18` does. Both machines now get the same bracket: install
player 2's action values, clear the companion bit, run the original, restore.

Incidental finding from forcing the movement field: `hero+0x384` bits 0..2 index an animation
directly, so `1` is walk and `2` is the next one along — the "falling" pose player 2's hero struck
when that value was guessed rather than copied from a moving Asterix (`f384` low byte `0x09`).

### Reading input and applying it are two calls

The player's hook-life tick (e.g. at `0x4AB2C2`) runs:

```asm
MOV ECX,ESI / CALL 0x0049A3B0
MOV ECX,ESI / CALL 0x0048ABF0    ; read input   -> hero+0x3D4, +0x3DC
MOV ECX,ESI / CALL 0x0048ACA0    ; transform it -> movement
MOV ECX,ESI / CALL 0x0049A1A0
```

`FUN_0048ABF0` only *stores* the raw axes. `FUN_0048ACA0` (`__thiscall`, hero in ECX, uses SEH,
plain `RET`) normalises them and produces the actual motion, writing `hero+0x578`/`+0x57C` at the
end. Calling the reader without the transform leaves correct-looking values in the hero's fields
that nothing acts on — which is exactly what the axis logging showed.

Both are called for player 2's hero now. `0x49A3B0` and `0x49A1A0` are not, and are the next things
to try if movement is still wrong.

Two other pieces worth recording:

* `FUN_004815C0` (a `CKGrpTrio` method) builds the camera-relative movement basis into
  `trio+0x39C..0x3A4`, and it consults **only the leader**: `trio + 0x30 + trio[0x2C]*4`. The basis
  itself is camera-relative and hero-independent, so it does not need duplicating — but anything
  that gates on "am I the leader" is worth suspecting.
* Both heroes share a single input view object, so swapping the manager's value array does reach
  both. The array was never the problem.

---

## 7. Rendering / camera

`0x414B70` — `__thiscall CalcProjCamera(CRendThing* self, float fovDegrees, char force)`, `ret 8`:

```c
if (!force && fabs(self->cachedFov /*+0x28*/ - fov) < 0.001f) return;
if (self->viewport /*+0x04*/ && self->+0x08) {
    self->cachedFov = fov;
    ss[0] = tan(fov * pi/180 * 0.5f);
    self->cachedRatio /*+0x2C*/ = viewport->height /*+0x10*/ / viewport->width /*+0x0C*/;
    ss[1] = self->cachedRatio * ss[0];
    RwCameraSetViewWindow(self->camera /*+0x10*/, ss);   // FUN_0058FFB0
}
```

The QoL patch and xxl-inspector both replace this function wholesale to get widescreen FOV, so it
is a known-good, safe hook point — and it is also where a per-viewport aspect ratio has to be
applied for split-screen.

Renderer object (built by `FUN_004144A0`):

| offset | meaning |
|---|---|
| `+0x04` | viewport/screen descriptor: `width@0x0C`, `height@0x10`, `+0x14` |
| `+0x08` | non-zero when the camera is usable |
| `+0x0C` | "BackBufferCopy" raster |
| `+0x10` | **main 3D `RwCamera`** |
| `+0x14` | second `RwCamera` — ortho, view window 0.5/0.5, near 0.1, far 1.9 → the 2D/HUD camera |
| `+0x18` | device descriptor passed to init |
| `+0x1C` | frame buffer object |
| `+0x28`, `+0x2C` | cached FOV / aspect |

RenderWare 3 entry points identified from that init function:

| address | RW function |
|---|---|
| `0x590060` | `RwCameraCreate` |
| `0x58E7B0` | `RwCameraSetRaster`-like (camera, raster) |
| `0x58DD80` | returns the frame-buffer/raster pair |
| `0x58FFB0` | `RwCameraSetViewWindow(camera, float[2])` |
| `0x58FD40` | `RwCameraSetViewOffset(camera, float[2])` |
| `0x58FD70` | `RwCameraSetNearClipPlane` |
| `0x58FE40` | `RwCameraSetFarClipPlane` |
| `0x58FF40` | `RwCameraSetProjection` (2 = perspective) |
| `0x58DFF0` | frustum resync, called by `SetViewWindow` |

Other callers of `CalcProjCamera`: `0x4145D7`, `0x414AD4`, `0x456E0C`, `0x4591D5`, `0x453804`,
`0x4540FF`.

### The cameras are stock RenderWare 3

The renderer init stores its objects in globals:

| address | object |
|---|---|
| `0x662EB4` | frame-buffer object |
| `0x662EB8` | **the main 3D `RwCamera`** |
| `0x662EBC` | the 2D/HUD `RwCamera` (orthographic) |

and the camera is the unmodified RW3 `RwCamera`:

| offset | field |
|---|---|
| `+0x04` | parent `RwFrame` (the transform) |
| `+0x14` | `projectionType` — **1 = perspective, 2 = parallel** |
| `+0x18` / `+0x1C` | `beginUpdate` / `endUpdate` function pointers |
| `+0x60` / `+0x64` | `frameBuffer` / `zBuffer` rasters |
| `+0x68` / `+0x70` | `viewWindow` / `recipViewWindow` |
| `+0x78` | `viewOffset` |
| `+0x80` / `+0x84` | near / far plane |

Confirmed live: `viewWindow=(0.9336 0.5252)` (16:9 — the QoL patch's widescreen FOV), `near=0.12`,
`far=60`, `projectionType=1`. The HUD camera is the one the init sets to projection 2.

### `FUN_00413FF0` — render one view

Found by pointing the slot profiler at the camera's `beginUpdate`/`endUpdate` pointers and recording
the callers (`0x414041` and `0x414063`):

```asm
00413FF0  PUSH ESI / MOV ESI,ECX / TEST byte ptr [ESI+0x68],0x20   ; 7-byte prologue
...
00414035  MOV ECX,[ESI+0x8] / MOV EAX,[ECX+0x10]   ; renderer -> main RwCamera
0041403C  CALL 0x0058FD30                          ; RwCameraBeginUpdate(camera)
0041404A  CALL 0x00414110                          ; *** draw the scene ***
0041405E  CALL 0x0058FD20                          ; RwCameraEndUpdate(camera)
00414067  RET 4
```

So `0x58FD30` / `0x58FD20` are `RwCameraBeginUpdate` / `RwCameraEndUpdate` (`__cdecl`, one
argument), `0x414110` is the scene draw, and `FUN_00413FF0` is "render one view" — `__thiscall`
with one stack argument. Its first instruction boundary at or past five bytes is seven, so it takes
a clean inline detour.

**Verified: calling it twice per frame is safe and costs nothing** — the picture is identical (the
second pass overdraws the first) and the game still holds 62 fps. Re-entrancy was the main risk in
the whole split-screen idea, and it is not a problem.

> Still needed: `RwRasterSubRaster` (to give each pass a half-screen sub-raster of the frame and Z
> buffers) and per-pass camera placement via the camera's parent `RwFrame` at `+0x04`.

---

## 8. Useful functions

| address | signature |
|---|---|
| `0x40D2C0` | `__thiscall kfRefToPnt(void* self, uint* ref)` |
| `0x515CB0` | `__thiscall CKHook::goVanish(KHook*)` — used to hide the HUD |
| `0x54BE20` | `__thiscall CKGrpBonus::spawnBonus(KGroup*, int type, Vector3*)` |
| `0x54E2C0` | `__thiscall CKHkBasicBonus::followHero(KHook*, Vector3*, KHook*)` |
| `0x54DF10` | `__thiscall CKHkBasicBonus::fall(KHook*, Vector3*)` |
| `0x5354B0` | `CKProjectileTypeAsterixBomb::spawn(...)` |
| `0x459050` | `(node, float, int)` — fade / visibility, used by the hero switch |
| `0x483CF0` | `__thiscall bitfieldSet(byte* self, char value, byte shift, byte width)` |

---

## 8b. Diagnostics gotcha: bounded log lines

The diagnostic dumps assemble variable-length lists — one entry per action, per vtable slot, per
changed byte — and the counts are engine-controlled. Building those with `sprintf` into a fixed
`char[256]` crashed the game with `0xC0000409` (`STATUS_STACK_BUFFER_OVERRUN`) the moment a level
left 25 of the 59 input bindings inactive and each one appended `(off)`, pushing the line to ~315
characters. It had run fine for 10,000 frames in a level where every binding was active.

All of these now go through `LineBuf` in `log.h`, which bounds every append and marks the line
truncated instead of running off the end. Anything new that builds a list into a log line should
use it too.

---

## 9. Host environment

* `dinput8.dll` in the game folder is **ThirteenAG's Ultimate ASI Loader**, so `.asi` plugins in the
  game directory are loaded automatically. The co-op mod is built as `xxl_coop.asi` for this reason.
* `d3d9.dll` is **AdrienTD's XXL QoL Patch** (windowed mode, widescreen FOV, right-stick remap,
  mouse input, configurable heap). Its source is not public. It already replaces `CalcProjCamera`
  at `0x414B70`, so the co-op mod must not blindly overwrite that function — see
  `coop_render.cpp`.
* Its `xxl_qol_settings.ini` has an undocumented `initial_level` key (the string is in the DLL but
  not in the shipped ini). **It access-violates on startup** on this build, with or without the
  co-op mod loaded.

### `force_level` works fine

Writing `CKGameManager::nextLevel` (what XXL-Editor's `GameLauncher` does, and what the mod's
`force_level` does) gives real, playable gameplay. An earlier note here claimed otherwise; that was
wrong. The symptom that prompted it — hero ticks frozen at zero while the game clearly played — was
caused by hooking the wrong function, see §6.

Two readings that misled the diagnosis, both corrected below: `hero+0x18` bit 0 means "AI
companion", not "hero exists" (§5), so the player's own hero reads as `avail=0`; and
`CKHkAsterixLife::tick` is not the per-frame tick (§6).
* `dsound.dll` is IndirectSound; unrelated.

### Roman agents (CKHkClueMan, 2,161) — spawn by leader distance (2026-10-05)
`CKHkClueMan::vf15` (0x4B8FB0) spawns the spy (FUN_004B12F0, node at hook +0x10) when the
trio leader (trio vf25) is within `+0x68` of the spy's spot `+0x12C/+0x130/+0x134` (x/y/z)
and removes it (FUN_004B1790) when the leader leaves that range; `+0x70` is the talk range.
An unspawned spy also needs `[[[gm+0x18]+0x24]+0x64]+0x10` bit 1 (opened in play by the first
spy's dialogue; the `spy2o` autotest sets it). Level 2 spies: (-824.7,507.6,-1060.5) r50
(start), (-762.8,507.7,-994.8) r38 (lifter area; stands at (-743,508,-1008) once spawned).
No detector is involved: the rectangle where the user saw it appear is ~36 units away.
Proximity slicing could not help because an unspawned spy has no node to locate it — LifePos now
uses the spot for clue men, and the slicing range is at least the spy's radius.
Dialogue: the zone in front of the spy (plane-cross 66 for the first one) sends trio 0x5120;
the trio walks the *leader* to the spy (situation 09) — if player 2 set it off from afar, player 1
has to steer Asterix there.

### Sector streaming: Sas boxes (2026-10-05)
CKLevel (yellow pages +0x90): sectors at +4 (sector box at sector +0x18, max xyz then min xyz),
current +0x24, loading +0x2C, requested +0x34 (-2 = none). `CKLevel::vf17` (0x40FD10) takes the
trio leader's position (game manager vf28 -> trio vf27) and checks the *current sector's* Sas list
(sector +0x10, count +0x14; FUN_00410DA0 with ECX = sector). A CKSas (12,17) has two boxes at a
sector border: +0xC (max xyz, min xyz) streams in sector +4, +0x24 streams in sector +8. Zone events
can switch too (CKLevel message 0x1222 "wait for sector to load", e.g. level 2 group #263, with
rectangles for both heroes). Level 2 example: sector 1 Sas 0 = (-800..-806) -> 1, (-808..-815) -> 2
at y 526..537, z -965..-974. Sector boxes overlap a lot (level 2: the spinning-platform area is inside both sector 1's and 2's
boxes), so they can't tell where a hero is. Co-op `WatchSectors` tracks each player hero's own sector
with the Sas boxes (as the engine does for the leader); a completed switch is credited to the
leader's Sas or to the hero whose zone fired (`ZoneEventHero`: the enabled detection entry carrying
the event -> its node -> hero); unknown switches (e.g. a forced load) move everyone along. A hero
whose sector isn't the streamed one for 2 s (outside Sas boxes) is brought to the other player
(autotests `sas`, `sectorzone`).

### Roman agent dialogue walk
Trio 0x5120 (FUN_00482610 case 0x20): leader +0x3F0 |= 2, param (0x34 bytes: +0 four bytes, +4
walk target, then directions) copied to hero +0x4C8; on arrival the trio sends 0x5120 back to the
spy, which starts the talk. A leader who falls off the walk (water reset) comes back playable and
the spy waits forever; walking there by hand does not help. `WatchSpyWalk` puts the leader on the
target and resends 0x5120 when the walk stalls (2.5 s no progress) or drops out. In autotests even a
normal dialogue stops after sound 0x84 (needs something the test doesn't provide), so tests compare
with `spya` up to that point.
Spy camera: `CKHkClueMan::sendEvent` 0x5139 (dialogue start) saves the camera service's track
(yp+0x68 +0x38) into spy +0x94/+0x98; the end (FUN_004B9760, just before sending trio 0x5121)
switches back via camera service +0x34 = spy +0x94 (+0xE0 = 1.0, +0xE4 = 0). A second start saves
the dialogue's CKCameraFixTrack (vt 0x6244EC) → camera stuck after the dialogue. Co-op keeps the
pre-dialogue track and writes it back into +0x94 (`WatchSpyCameras`), checked again at 0x5121.

### Torches, levers, safe sequence (2026-10-05)
* CKHkTorch (2,32): carryable torch. On the ground its tick offers itself to the leader within
  +0x14 (trio 0xC06 -> trio +0x4C, cleared by the trio every frame; vf40 = leader is Asterix). The
  hero's input processor sets +0x3EC bit 0x10 from action 5 (X); FUN_00499CF0 (thiscall, ret 4)
  turns that into bit 0x2000 when something is offered (FUN_0049A080); the situation chooser then
  picks situation 5 (take). Carried: hero +0x464 = torch, torch +0x3E state 1, fire level in bits
  2-4 (0x5128 lights it: level 4). 0x513C (as CKHkAsterixLife::InitOnce does) throws it away.
  Cheat (R3 + D-pad up/down, Inspector buttons): offer the nearest torch inside Asterix's tick and
  add bit 0x10 at FUN_00499CF0's entry.
* CKHkActivator (2,52, vtable 0x628DA0): lever; events +0x28 (on push) and +0x2A (on anim end);
  hit through collision contacts 0x300 / projectile hits 0x301. Level 2 sector 5: the lever behind
  the iron crates switches to Asterix (0x48B7); the hearth (CKHkHearth "on get fire") then has
  Obelix lead and sends Asterix to a marker (0x4813). Hit by a non-leading Obelix while Asterix led,
  the order broke and Asterix was parked for good. Safe sequence (ini safe_sequence): contacts from
  the non-leading player don't reach a lever whose event group switches the trio to the hero who
  leads already (autotest `lever`, control run with the option off shows 0x48B7).

### Launcher started by the non-leading player (2026-10-05)
Group action (hero situation 0x0D): the non-leader that starts one (sector 2's CKHkTrioCatapult)
takes the lead for it (round 8). Co-op used to suspend only for player 1's hero in a group action,
so a launch by player 2 kept split screen and player control of the other hero, and the lead stayed
with Obelix after landing (the user's "far away" camera = the mod's own follow camera seeded from the
launcher's 9.7-unit flight camera). Now either hero's group action suspends co-op (the game flies the
companion along), and when co-op resumes the lead goes back to the previous leader (trio 0x4800/1).
Autotests `launchp2` / `launcha` match after landing (positions, leader, game camera). Follow
cameras are seeded at most 6 units out.

### Level 1: intro, Obelix not yet found (2026-10-06)
* Cutscenes: CKCinematicScene (12,37), listed by the cinematic service (yp+0x80, +0x10/+0x14). Scene
  +0x20 flags: bit 0 playing, 0x1000 "end on Enter" (needs (flags & 0x180) != 0x80);
  CKSrvCinematic::vf17 calls FUN_00461A80 (finish every running bloc) on its view's action 0. Level 1's
  intro is scene 1 (flags 0x4F13 while playing), no 0x1000. After the skip the scene fades out for ~5 s
  (game manager +0x5C bit 20 stays on meanwhile). `[debug] skip_cutscenes` adds 0x1000 to all scenes;
  autotest CINE_SKIP calls FUN_00461A80 on playing scenes.
* Obelix is in level 1 from the start, waiting at (56, 2, 106) (AI order 3); hero +0x1A is nonzero
  until trio 0x4883 ("discover Obelix", from rectangle #17 at (52.1, 5.5, 105.8) crossed along x ->
  cutscene scene 2) clears it. Co-op can't start before that ("Obelix has not joined Asterix yet").
  Autotest `lvl1obelix` (-Level 1).

### Magic potion on Obelix
Hero_ReadCommandBlock (0x49A350) sets hero +0x388 bit 0x4 (potion) only when the hero's vtable slot
15 returns 0 = Asterix (Obelix's returns his class id). With the bit, a player's Obelix runs at 15
(vs 7) and takes no hits (autotests `potionmove`, `hurto`/`hurtop`: 10 hit frames vs 0). The triple
jump is in Asterix's body-state table (extra states 0x4A3730/0x48D960/0x48DA50, CKHkAsterix_
InitStateTables vs Obelix's FUN_004A69F0) -- Obelix has no third jump state/animation.

### Round 21 (2026-10-07): full-game issues
* **Pressure plates** (CKHkPressionStone, flags `+8`): bit `0x10` = activatable by hero (trio vf29 =
  object under the leader's feet, hero `+0x280`, == plate `+0x28`), bit `0x40` by a pushed object;
  events `+0x30/+0x32`. Plates whose event groups message the trio stay leader-only (blue circles);
  the others tick with the non-leader as leader when the leader is not on them.
* **Non-leader takes the lead** (CoopAfterLifeTick) when its own tick starts a group action (sit 0x0D),
  a Roman agent's talk (sit 0x09: X on the spy sends the spy 0x5139 and the trio 0x48BA, companion gets
  AI order 13) or a machine (mode 1/2: machinegun = trio 0x4809, Asterix Life State4). Lead returned
  when co-op resumes. Cutscene regroup also brings a hero on AI order 13.
* **Spies**: talk offer (trio 0x511F) to a leader within `+0x70` of the spy node; slicing uses the node
  once spawned. The "gate" `[[gm+0x18]+0x24]+0x64` is the button prompt's CAnimatedNode, not a gate.
* **HUD action prompt** ("Activate"): `FUN_0049A1A0` (every player state) writes gm `+0xCC` bits 4-7
  (5 = on a launcher, hero `+0x3F0` bit 0x4000 from trio 0x5130; 0xE none). Last hero ticked won;
  now a prompt from either player's hero stays for the frame.
* **Button prompts on objects**: CKGrpMeca (gm `+0x18`): owner `+0x90`, gm `+0x5C` bit 0, kind gm `+0x68`;
  objects show them on 0x90D+kind, hide on 0x90A (event sequences).
* **Merchant**: CKHkAsterixShop (2,172) state `+8` bits 0-2 (trio 0x5123 opens). Menu =
  `FUN_004BFC50`, input view by name (FUN_00437FF0 on CKInput, name at 0x64F28C; view =
  `[CKInput+0x40][i]+0x10`, remap at view `+8`); actions 2/0x12 left, 3/0x13 right, 4 buy, 5 back.
  Purchases: `[gm+0x40]+0x10` bit per item index.
* **Combos**: CKInput `+0x34` = combo recognizer (vt 0x623F18), run at the end of CKInput::Update
  (`FUN_004353C0(values, bindings)`, ret 8); writes value `numActions + k`. Heroes' view (trio `+0x28`)
  maps actions 0x10..0x15 to 59..64. Combos: 0 A A X B (Club, item bit 1), 1 A B X (Power Hammer, 2),
  3 A A Y B (Mole, 0x10), 4 A X B Y (Twister, 4; Fusion item 8); 2/5 unused by heroes. Needs combo
  bar `[gm+0x40]+0x10` bit 0x4000 (filled by FUN_0048C860: gm `+0xA4`=`+0xA0`, `+0xC4`). Combos are
  accepted on the ground (FUN_00494DB0). Dev cheat: gm `+0x5C` bit 7 + gm `+0x408` == 0x24011980
  (also unlocks menus, not used). Player 2: recognizer run on a deep copy for P2's values.
* **Fight mode / music**: trio tick `FUN_00481950`: enemy from the leader's enemy cache within trio
  `+0x400` → music group (trio `+0x40E`+2·id), camera bits, trio `+0x3F4` bit 0x40, `+0x408` refresh,
  end after `+0x404` s (`+0x40C` group). Now also run for the non-leader first.
* **Squad reset**: inactive squads out of the game camera frustum (vf35, planes svc `+0x500..+0x540`)
  are reset (vf34, all Romans back to the pool); kept while the other player's view sees the box.
  Reinforcements spawning at markers in P2's view: NOT found yet (not vf35, not kluster query).
* **Beacon klusters**: cameras' rankings interleaved (helmet pool capacity).
* **Tools**: autotest PLACEAT/GLIDEAT, S_NEAROBJ, S_WATCHRW (read/write HW watch), S_INPUTDUMP,
  S_FINDGROUPS/S_WATCHGROUP2, env XXL_LOGFLAGS=1/2 (every flagged event), stream-start backtrace.

### Round 22 (2026-10-07)
* **Twister for both**: the attack chooser `FUN_00494DB0` (thiscall(current), ret 4) copies the
  leader's Twister (state 9/10 at hero `+0x135`, table `+0x130`) only in the AI variant. Hooked:
  a player's hero on the ground and not attacking copies the other player's freshly started Twister
  (`FUN_004963D0` → 9 or 10); a copied Twister is never copied back. Fusion input still unknown
  (X with both spinning did nothing in autotests, single player included).
* **Reinforcements** (`FUN_00530060`, thiscall(out), ret 4, from squad vf37): spawn point = camera
  service `+0x5B0` minus `+0x15C` plus random × `+0x144`, dropped on the ground, must be inside the
  squad box and reachable. Squads run for the non-leader now use that player's camera (OtherViewMatrix);
  points within 40 units in any player's frustum are refused (retried next frame). Squad member
  groups (squad `+0x144`, 0x1C each): `+0x11` alive, `+0x15` fallen awaiting replacement, `+0x16`
  spawned, `+0x18` budget.
* **Helmet pools**: CKGrpBonusPool `+0x28` handler, `+0x2C` beacons placed per frame (level 3
  helmets: 25 of 50 objects; the rest for drops), free list `+0x38` (via `+0x14`). With the split
  screen up the cap is raised by 3/4; kluster ranking now balances beacons (entry `+0xC` count) per
  camera. Far helmets can still drop out when both views see many nearer ones (capacity).
* Diagnostics: env XXL_KLOG="x,z" (kluster list per handler near a point), XXL_LOGTRIO (single
  player trio events), autotest S_P1COMBO (P1 combo action), S_WATCHENEMIES.
* **Twister Fusion** (round 23): Twister (attack 10, handler 0x493F70) sub-states hero `+0x38E & 0x3F`:
  0x22 start, 0x23 spin, 0x25 merging (FUN_00498800), 0x26 fused tornado (FUN_00498A30), 0x27 end.
  Fuse check FUN_004984F0: own request `+0x188` (X while spinning, needs `+0x189` "partner near",
  set only by the AI companion's spin within trio `+0x3D8`) + companion at 0x23; AI variant follows the
  leader's request. Co-op: nearness emulated, non-leader's request moved to the leader (leader drives),
  the partner runs the check and both steps through their AI branch (f18 bit 0 + real leader for the
  call), split screen off (CoopOneScreen) during 0x25/0x26 -- not suspension (AI switch ends the Twister).
* **Marker spawns** (squad vf38, 0x52FB80, thiscall(count, group) ret 8): markers squad `+0x5C`
  (8 bytes, +0 -> position), count `+0x64`; picked from the game camera (svc `+0x5B0` pos,
  `+0x5BC` look) outside a cone of 1.5 x FOV (camera `+0x38` -> `+0x44`). Co-op: list filtered per
  call (no marker in any player's view within 40 units, P2's cone, or within 8 units of a hero),
  non-leader squads use that player's camera.

## Round 25 (2026-10-09)

- **Lifter "sidekick not present"** (CKHkLifter::vf15 0x4AD670, vtable 0x625E2C): state 0 (bottom) with
  flags +8 bit 0 ("wait for trio") counts +0x34 while the leader (trio vf29) stands on it without the
  companion (vf44); past [gm+0x18]+0x24 → +0xA0 it flags event +0x70 (trio 0x4887 / 0x48BD teleport
  Idefix / sidekick to marker). Ticked sliced to player 2's Obelix it teleported Obelix every 9 s.
  Co-op zeroes +0x34 before every lifter tick: lifters wait for both players.
- **Roman pool / vf36** (0x52F850): a squad activating (vf33 0x52EF70) with the CKGrpPoolSquad empty
  calls vf36, which resets (vf34) every active squad of its group whose box lacks the camera look-at
  (svc+0x5BC). Two players in different places → the squads around the non-leader freed each other
  every frame, their Romans respawned at their start spots (hanging above the LV5-8 lifters).
  Hooked: squads holding either hero are kept.
- **Trio events 0x4813–0x4844** sidekick walk to marker n (companion +0x564 = 1), **0x4845–0x4876**
  run to marker n (+0x564 = 2), **0x4885–0x48B6** teleport Idefix to marker n, **0x48BB–0x48EC**
  teleport sidekick to marker n. Hero zones whose group sends these are left out of the swapped
  detection pass (LV4-9 / LV5-4 boat boxes).
- **Catapult flight** = situation 0x0D. A landing-zone 0x4800 (switch to Asterix) arriving while Obelix
  leads in flight left both heroes hanging at the end of the flight (no 0x5135 landing). Leader switches
  arriving during a flight are deferred until no hero is in 0x0D (CoopDeferTrioEvent).
- **Shine (open)**: x10 bonus = CClone node (no children); the rays around it and the green potion aura on
  Asterix are drawn only in the scene walk that is the frame's 2nd (normal split: stamp, P1, P2 → P1 only;
  without the stamp walk → P2 only; an extra stamp walk before P2 doesn't help). Not CGlowNodeFx, not
  CParticlesNodeFx near the bonus, not list 12 (that is the 2D HUD, HUD camera = parallel projection),
  not the back-buffer copy stamp (renderer+0x24), not clone sync. P2's aura comes out orange/yellow instead
  of green. `XXL_SPLITDBG=2` puts player 1's camera in both halves for A/B screenshots (shine25 scenario).

## Round 26 (2026-10-12): shine in player 2's view -- SOLVED

The x10 bonuses' rays and the potion aura on Asterix are CParticlesNodeFx systems. Ruled-out leads
from round 25 aside, the cause was the particle walk handling: player 1's walk stepped the systems and
player 2's walk stepped them again with a tiny time step, which (a) found the aura's per-frame emission
points (+0x40, cleared by the walk when +0x10 bit 12) already used up and (b) left the rays' systems
drawing nothing in the half drawn right after a step. Now the stamp walk (player 2's camera, dummy lists)
steps every system player 2 sees, and both real walks only add the stepped system's atomics
(FUN_0041E390, thiscall(node, lists) -> visible) plus walk its children when the node's stamp
(+0x14) is this frame's; unstepped systems (seen by player 1 only) are stepped by player 1's walk.
Repro: shine44 (-Level 4, both heroes at -720.4 10 -179.6) with XXL_SPLITDBG=2 (player 1's camera in
both halves).
- Follow-up: particle systems with +0x10 bit 9 (0x200) are stepped whether or not they are in view and
  stamp themselves only when visible. The x10's yellow rays, stepped by player 2's stamp walk while out of
  player 2's view, were then skipped by player 1's walk (its work bit already cleared): only the red rays
  showed when teleporting there with split screen on. The split frame now tracks which systems were stepped
  (pointer set, generation per split frame) instead of trusting the stamp.

## Round 27 (2026-10-12): each player gets a copy of the game camera

* `CKSrvCamera` (yp `+0x68`, alloc `0x6A0`): vf17 `0x454620` = tick (thiscall(frame), dt at frame `+0xC`).
  Target `+0xE8` = a hero's hook `+0x10`. `0x459050` (was "Node_FadeVisibility") is really
  `SetTarget(target, transition s, cut)` (ret `0xC`), used by `CKGrpTrio_SwitchLeaderFollowup`.
  Tracks: list `+0x20` (linked `+4`), next/current/previous `+0x34/+0x38/+0x3C`. Camera sectors
  (`CKCameraSector`, list `+0x24`): box `+8..+0x1C`, track `+0x20`, flags `+0x29` (bit 1 on, bit 3
  one-shot) -- the level's camera triggers, picked by the target's position (LV3-2 beach = fixed track).
  Camera object `+0x1C` shares the main RwCamera's frame; view window/clip go to the main RwCamera.
  Input view `+0x580`: actions 0-4 -> bindings 46-50 = right stick up, down, right, left, R3 (edge).
  Fight camera = `+0xC` bits 28-31 (`0x80000000` on), end calls track vf24.
* Track sizes: rigid `0x88`, classic `0xDC`, path `0x80`, fix `0x70`, axis `0x80`, plain `0x6C`,
  passive path `0x94`, spy `0x11C`.
* `gamecam.cpp`: the game's state = the leader's camera; the non-leading player's camera is a byte copy
  of the service + its tracks, swapped in after the game's tick for a second tick (aimed at that hero,
  with that player's camera input), then everything (service, all tracks, frame, window, clip, nodes
  `+0x5D0/+0x5D4`) goes back. On a leader switch (SetTarget from the switch follow-up, or the leader
  byte changing) the two states change places. Fight bits per hero (proximity FightHook records which
  pass refreshed trio `+0x408`). Held heroes get the old follow camera (p2cam). ini `camera`, `split_fov`.
* Autotests: cam32 / cam32o (beach trigger per player, right stick zoom), cam32s (switches), camfight,
  cable33 (user's LV3-3 spot: P2 gets the passive path pull camera, P1 the follow camera), launch48.
* Round 27 follow-up: hero code writes camera wishes into the service (in the air: +0xC bits 11/12, many
  `FUN_0044F520` calls with ECX = svc+0xC from 0x4966E0, 0x496EB0...), so the non-leader's Life tick now
  runs with its copy swapped in (`GameCamBeginHeroTick`) -- P2's camera no longer rises with P2's jump.
  The copy code asks `GameCamLiveHero()`, not trio+0x2C (sliced to the ticking hero).
* LV2-2 slide end: classic track +8 bit 1 = right-stick turning allowed (the slide-start track 0x30B4 has it
  off). The spy-dialogue camera fix (round 16) put the pre-dialogue track (the slide's) back at 0x5121 over
  the game's follow camera; now only a dialogue camera (fix track / plain CKCamera) is replaced.

## Round 28 (2026-10-12)
* LV2-6 plate (5.7 2.8 -389.2): its sequence also messages the trio (0x2000 fight timer) -> PlateForEither now
  uses GroupCommandsTrio (leader switches / companion orders) instead of "messages the trio at all".
* Spy dialogue 0x5120 set off by the non-leader (non-leader within 25 of the walk target, leader > 6 away):
  the non-leader takes the lead for the dialogue (raw trio swap before 0x5120; given back on resume like group
  actions), so the game brings the other hero in as the companion (0x48BA = companion AI order 13 to the
  leader's dialogue spot, it walks in from off screen as in SP). Autotests spy21o (leader sets it off) /
  spy21a (non-leader); these force the spies' gate, so the spy itself is not spawned in them.
* LV5-9: the slide's agent (two talks: landing, slide end) stays spawned at the slide end; the launcher's landing
  zone (0x0988) sent it 0x5198 again -> talk 700 units away. CKHkClueMan::sendEvent hooked: 0x5198 ignored when
  both heroes are > 250 from the spy's node and spot (150 broke the slide-end auto talk) (co-op only). Not autotested (needs the slide state).
* LV5-8 lifter Romans partly missing while the previous section's Romans live: the Roman pool (CKGrpPoolSquad)
  has a fixed number of Roman objects from the level file; heap size does not change it.
* Far-spy check, follow-up: measured from the talk's walk target (+0xFC, or +0x108 for a two-part spy's second
  talk) and from the heroes' node (LTM) positions -- during the belly slide +kHero_pos stays at the slide's start,
  so the slide-end agents of LV4-10 / LV5-9 looked 400+ units away and their talk was dropped.
* Pad bindings: the user saw A/B and the menu's D-pad change mid-session (after an Inspector level jump);
  WatchBindings logs any change of the game's binding sources with the full table.

## Round 30 (2026-10-13)

* LV2-10 boss gate: rectangle #34 (1672.3 -738.8 -1210.9, group #147: trio 0x4800 + 0x4803, lights, enables #4)
  starts the boss intro cutscene; #4 (1687.1 -739.6 -1210.9, group #20: boss 0x4C18/0x4C15, trio 0x4829 "sidekick
  walks to marker 22", a CKManageEventCinematicBloc 0x501, ...). Both bound to Asterix, so the swapped detection
  pass had to carry them for player 2's Obelix -- and the round-24/25 filter (any group that commands the trio)
  dropped them. Filter now (ZoneMeansItsHero): only companion orders -- wait (0x4802, 0x4849) and marker
  walk/run/teleport -- keep a zone to its own hero, and not when the group also runs a cutscene (cinematic bloc
  vtables 0x624A7C..0x624F48, 0x629B78, CKSrvCinematic 0x624F90). Plain leader switches answer either hero.
  Autotests: boss210 (both zones by Obelix), circle45 unchanged (LV4-5 circle 0x0528 still Asterix-only).
* A cutscene zone set off by the other hero brings the zone's own hero to it when the cutscene camera comes up
  and they are 40+ units apart (CoopNoteStoryZone; RegroupForCutscene alone skips it while the game holds a
  player hero, e.g. the walk to marker 22).
* Boss runs (LV2-10, LV3-6): the waking zone's group disables the Tornado/Mole combos (trio 0x48EE/0x48F1) and
  walks the sidekick to a marker (LV2 #4 group 0x014: 0x4829; LV3 #46 group 0x0BB: 0x4824, then Obelix's own
  rectangles #52/#53 walk him on with 0x4825/0x4823; Asterix's #143 sends the boss 0x4C15 "move to other end" --
  the LV3 machine only rolls once a hero runs past it). Autotest `bossscan` (S_BOSSSCAN) lists every group with a
  marker walk on levels 2-6: only those two have the combo fingerprint. Co-op: the run's groups (start group plus,
  through the detection-service enables 0x14..0x18+index, every group that walks the sidekick on) are collected
  when the start fires; their marker walks are dropped (CoopDeferTrioEvent) within 500 ms of a run zone, and the
  other player is put on the waker (the detection service's last position for it moved too, ProximityForgetMove,
  so the jump crosses no zone). Autotests boss210, boss36.
* Sky spinning in split screen: CSkyNodeFx::vf14 (walk, 0x41AF50) scrolls the sky textures by the camera's turn
  since its last walk (state on the node: last forward x/z +0x54/+0x58, last right +0x64/+0x68 with +0x10 bit 6,
  last height +0x6C, scroll offsets +0x4C/+0x50/+0x5C/+0x60) plus a drift by frame time. Walked three times per
  split frame (P2 stamp, P1, P2), each walk took the angle between the players' cameras as a turn: per-walk log
  (XXL_SKYLOG=1, XXL_SKYFIX=0) jumped ~1.2 between the halves every frame with the cameras facing each other.
  Now the stamp walk skips the sky and each player's walk swaps in that player's copy of the state (sky2 autotest).
* Shadows (CKSrvShadow objects, vtable 0x62520C, one per shadow): slot 26 (0x46F980, ret 0x10) places a shadow
  once per frame from the shadow service's update (callers 0x470E19 / 0x470F0B, outside the render): culled and
  faded by distance to the game camera (svc+0x5B0) and its matrix (2nd arg, pos +0x30) nudged ~0.27 units
  towards that camera (service yp+0x88 +0x4C). The game camera follows the leader, so the non-leader's shadow
  leaned towards the leader (XXL_SHADOWLOG=1 logs the heroes' offsets; XXL_SHADOWFIX=0 for the game's own). In
  split screen each shadow is now placed with the player camera nearer to it (OtherViewMatrix vs svc+0x5B0).
  Autotest shadow33 (LV3-3 room x 290.1 z -331.7; the user's coordinate had y/z swapped).
* Shadows, follow-up: one shadow placement serves both halves, so any sideways nudge looks pulled towards
  someone in the other view (the user saw it with the nearer-camera rule too, LV2-9 white floor 1516.3 -640.5
  -882.7, autotest shadow29). The split default now places each shadow for a camera straight above its ground
  point at the nearer player camera's distance: the nudge is (0, 0.3, 0) for every shadow, culling/fading keep
  the nearer camera's distance. XXL_SHADOWFIX=0 game / 1 nearer camera / 2 straight up (default).
* Shadows, second follow-up: lifted straight up they showed above the feet (the game's nudge goes along the line
  of sight, which does not move a shadow on screen for that camera). The shadow service (CKSrvShadow, found by
  vtable 0x62520C in the yellow pages; tick slot 17 = 0x470A70, ret 4) builds one mesh of all shadow quads; in
  split screen it now ticks again right before each half's DrawWorld with svc+0x5B0 = that half's camera eye and a
  zero frame time (fades/smoothing advance only in the game's own tick). Log (XXL_SHADOWLOG, per walk): Obelix's
  shadow nudged towards player 1's camera in walk 1 and player 2's in walk 2. XXL_SHADOWFIX=0: game's own.
* Animation blends in split screen: CAnimatedNode / CAnimatedClone walks (slot 14: 0x41DA30, 0x41D270, ret 8)
  end in FUN_0041DA70, which cross-fades the previous animation (+0x40) into the current one (+0x28): +0x10 bits
  7-8 blend mode (1 fade, 2 fade with the old one still playing, 3 fixed weight), +0x48 the blend clock (+= frame
  time per walk), +0x44 its length (0.12 s for Asterix starting to walk). Three walks per split frame: blends ran
  3x fast (log: clock 0 -> 0.115 in 2 frames) and the bones were blended on top of the last walk's -- heroes
  snapped between animations. Now only the frame's first walk of a node blends (stepped table, key node+1); later
  walks run with bits 7-8 cleared (fixed: clock +1 dt per frame). XXL_ANIMLOG / XXL_ANIMFIX=0, autotest anim2.

## Round 32 (2026-10-13): LB pass, RB Idefix

* Hero input (Hero_ProcessPlayerInput 0x499660 -> hero +0x3EC): LB (action 8) bit 0x80, RB (action 10) bit
  0x100, kept by FUN_00499CF0 only on the ground (+0x3E9 == 0). The AI companion's chooser (Hero_ChooseAiState
  0x49EE80) reads the *leader's* (trio vf 0x90) bits: LB while following -> AI order 5 "throw an enemy" (state
  0x49E340: walk to the target, +0x884 bit 1 = grab; entering sets +0x884 bit 0x80); RB for Idefix -> order 6
  "bite" (range gate: Idefix +0xBDC bit 1 with the leader beyond +0xB80). Enemy pick FUN_00495C60: the leader's
  own target first, else the trio's enemy list (vf 0xF8 / distances 0xFC, built around the leader while trio
  +0x3F4 bit 0 is clear) within +0x844. Sound handlers (Asterix 0x4A5620, Obelix 0x4A89C0, Idefix 0x4AA080)
  read the leader's bits too ("no" sound 0x1B when nothing to bite).
* Idefix (CKHkIdefixLife vtable 0x625DA0, tick slot 17 = 0x4AC560): on the frame the non-leading player's hero
  has RB, his tick runs with that hero as the trio leader, the range gate off and the enemy list rebuilt
  (autotest rbp2: order 6 from 25 units away, "no" sound when busy).
* Pass (research): AI order AI states table at hero +0x55C (0x49DF40, 0x49DFA0, 0x49DFD0, 0x49E150, 0x49E1E0,
  0x49E340 (5), 0x49E4C0 (6), 0x49E680, 0x49E710, ...). Romans' states are vtable slots 40+n (0 tick, 1 enter,
  2 exit; return the next state): 3 = held/spun (slot 43), 4 = flying to the trio leader (slot 44, 0x4D0D80:
  target = trio vf27 at the throw, flight time (dist + c[+0x100]) / c[+0x104]; trio 0x4C06 near the end).
  Hero +0xBC8..+0xBE4 / camera +0x668.. = the receiver's slow-motion catch (FUN_00496EB0, events 0x4C0B /
  0x4C13 to the incoming Roman). The code that throws on the leader's second LB is not found yet (no trio
  event; autotest lbsp, XXL_LOGTRIO=2).
* Pass, found: the companion's spin (body handler 0x492660, steps +0x38E 4 grab / 5 lift / 6 spin) throws on the
  leader's LB (or after +0x868 turns facing the leader): FUN_00495F10 (let go), Roman event 0x4C0F(0) and step 7.
  Roman events (Class_2_84::sendEvent 0x4CBA60, FSM state +0x21, state table +0x1C): 0x4C0F(param) -> state 3
  held, 0x4C0F(0) -> state 4 fly to the trio leader (+0x24 |= 0x800), 0x4C0B -> state 1 thrown forward (+0x400).
  The player's own grab-spin is handler 0x492CF0 (steps 0x0A grab, 0x0B lift, 0x0C spin; on letting go
  0x4C0F(0) + 0x4C0B). The FSM tick (Class_2_88 vf30 0x4CA8D0) asks the chooser (vf26, Class_2_84::vf26
  0x4D1220) first: +0x24 bit 2 -> 1, bit 0x80 (hit) -> 2, 0x20 -> 3, 0x800 -> 4.
* Co-op pass (coop.cpp WatchLbPass, after each player hero's tick): LB with the hero at step 0x0C holding a Roman
  in state 3 and the other player within 30 units -> FUN_00495F10, 0x4C0F(0) with the other hero as the trio
  leader, step 0x0D; the Roman ticks with the receiver as the leader while flying (ProximityPassTo / TickLife)
  and its "hit" bit is cleared while still within 3 units of the thrower (the contact with the thrower read as a
  hit and ended the flight at once). Otherwise LB plays the other hero's "no" (sound bank +0x200, sound manager
  yp+0x18: 0x43C130 playing?, 0x43B730 start). Autotests pass22 (archer of LV2 sector 2 flies to Obelix, 0x4C06
  sent, lands stunned beside him), lb2. Diagnostics: XXL_PASSLOG (events to and flight of a passed Roman).
* The whistle: the AI companion's step 5 (lifting the grabbed Roman, handler 0x492660) plays its sound bank +0x280
  (Obelix sound 0x95, Asterix 0x9F) from the sound handler's step-change switch -- the "ready" whistle; the
  player's spin (steps 0x0A-0x0C) never plays it. A co-op pass plays the thrower's +0x280 (user-confirmed).
  XXL_LOGSOUNDS=1 logs every sound start from hero code (2: from anywhere); autotest lbwait (one LB, wait).
* Pad layout changing mid-session (xxl_coop.pad_strange*.log): CKInput +0x64 = controller preset in use;
  FUN_00438950 (thiscall(preset), ret 4) points every binding's source (+0xC -> +0x14 current, +4.. per preset)
  at it. CKAsterixGameManager::vf10 picks it (state 6: FUN_00503AA0 -> 3 with a pad, 1 + "no controller"
  without; state 1: FUN_00503B00 saved options); the options menu only sets 1. Preset 0 (X/B/Y shuffled, menus
  on the left stick) was in use in both bad sessions until the return to the menu replayed the intro and set 3;
  both had force_level 2 then an Inspector jump to level 5 seconds later. Now any running level on preset 0 gets
  the game's choice (3 with a pad, 1 without); every level change logs "input: level N, controller preset P".
  Autotest preset0 (S_SETPRESET).
* Third wrong-preset session (xxl_coop.pad_strange_newer.log): preset 1 (keyboard) from the first frame, which
  came 5 s after the menu (normally <1 s) -- the game picked at boot (FUN_00503AA0 from vf10 state 6, called at
  0x503AED) while FUN_00438AB0 saw no pad yet; force_level skips the title, the only place it looks again. Now
  0x438950 is detoured: a choice of 1 made without a pad is remembered, and a level running on it gets 3 once
  the game sees a pad (1 chosen with a pad -- the options' keyboard layout -- is left alone). Every call logs
  "input: the game sets controller preset P (from caller; it sees a/no pad)". Autotest preset1 with
  XXL_PRESETNOPAD=1 (the next choice counts as made without a pad).
* Snow in split screen (LV2 sector 1 camp): CFogBoxNodeFx (vtable 0x622F14) walk 0x416D10 copies the frame time
  to +0x20 each walk and moves the particle groups its camera sees (others gathered at their group centre):
  records +0x360 (7 floats), fades +0x35C (8 dwords), box fade +0x38C; modes +0x2D3 (1 = positions from the
  absolute time, unaffected). Three walks per split frame moved the snow up to 3x. Now: no stamp-walk step;
  player 1's walk steps; player 2's walk steps, then every particle player 1's walk changed is restored to that
  walk's result (one step per particle per frame). User-confirmed visually (autotest fog21, XXL_FOGFIX=0 for
  A/B); the XXL_FOGLOG position read (+0x360 [3..5]) did not show the motion -- that log is not trustworthy.
  XXL_PARTLOG: particle systems called twice while "active" in one split frame (diagnostics).

## Round 35 (2026-10-13): shared modes, machines for Obelix, hints, dialogue for pad 2

* Every control read goes through InputView_GetAction (0x4358B0, thiscall(view, action), ret 4): value =
  CKInput.values[view+8 remap[action]], view = an 0x10-byte entry of a named input map (CKInput +0x40 maps,
  +0x5C name hashes, +0x38 count; FUN_00437FF0(name) -> index). Maps: "Astérix Trio" 0x64D05C (heroes),
  "Astérix Baliste" 0x64F550 (machine gun: 0/1 turn = values 36/37, 2 fire = 38 X held), "Astérix Bateau"
  0x650F7C (boat: 0-3 stick = 39-42, 4 jump = 43 A held), "Camera" 0x64713C (46-50), "Sekensor" 0x64743C
  (dialogue text: 0 advance = 45, X edge), menus 0x64F28C. shared.cpp detours it: in a shared mode each (map,
  action) is answered from the owning player's values (gun: Obelix turns, Asterix fires/leaves (A = trio
  0x4806 via FUN_0049A3C0); boat / belly slide: Obelix steers + camera, Asterix A; Twister Fusion: Asterix
  stick up/down, Obelix left/right + camera); Sekensor takes either player's. XXL_ACTIONLOG=1 logs every
  (caller, map, action) once and routed answers. Autotests mgun33o/a (LV3-3 gun 289.0 31.9 -326.1),
  boat49o/a (LV4-9 -165.4 6.0 -652.0), slide22s (LV2-2), spy3p (P2 clicks through a dialogue).
* Machines: their tick offers them to the leader in reach (trio 0x4A01 gun / 0x4901 boat: trio +0x48 =
  machine, +0x3F4 bits 10-11 = 2/1); X runs FUN_0049A080 (from FUN_00499CF0 when +0x3EC bit 0x10), which only
  works for Asterix (hero vtable +0x3C == 0) -> trio 0x4809 (gun: Asterix mode 2, companion AI order 7 to
  its place, then 0x480A) / 0x4807 (boat, mode 1). Hero +0x3EC bits: 8 A, 0x10 X, 0x20 B, 0x40 Y, 0x80 LB,
  0x100 RB. Co-op: Obelix's X at an offered machine is held back (no swing) and served next frame: Asterix is
  put on Obelix's spot, made the leader (lead returned on resume) and the trio gets the same event. Both
  heroes must be within 12 units (4 vertical) for either hero's X; else the "no" sound and a hint.
* Hints (hints.cpp): the HUD prompt (CKHkInterfaceInGame 2/132, tick vtable 0x6285F0 slot 18; kind gm +0xCC
  bits 4-7, 0xE none; strings [interface +0x1A4 + kind*4] via the loc lookup FUN_00411780, detoured for id
  0x3F00C0DE). Text object +0x15C (laid out from +0x20: words +0x2D4/+0x2D8, box width +0x28, height +0x2C,
  word places +0x3C8 + 8i, FUN_0042EF80); a hint is laid out on one line of a 2.0-wide box, then its words are
  moved onto the hint's own '\n' lines, right-aligned; units: the screen's width = the aspect (yp+0x34 -> +4).
  The prompt goes to +0x428 (target +0x42C, place +0x434, state +0x24 bits 21-22). Plate +0x140 (CBillboard2d,
  drawn from +0x14 leftwards by +0x34, +0x38 high, height/y carried over by the game -> restored). Hints:
  level start (P2 joins with Start), LB pass (first 3 spins a level, until one pass), mode roles, machines,
  lifter waiting (lifter 2/47 state 0 with +8 bit 0 and one hero on +0x50, hero +0x280, on ground).
  XXL_HINTLOG=1 logs geometry. Not autotested: the lifter and LB hints, Twister Fusion routing.
* Merchant opened by the non-leader (round 35 follow-up): X there sends trio 0x48EF (the companion -- with the
  trio sliced, the *leader* -- gets AI order 14 to the shop) and puts the opener in situation 0x0A; the shop's
  camera follows the leader. Left the companion, P1's half showed the shop's / Obelix's camera afterwards and the
  visit could end early. Now situation 0x0A started by the non-leader takes the lead like a Roman agent's talk
  (lead returned on resume, trio 0x4800). The shop offers itself every frame to the leader within its +0x5C of
  the merchant (FUN_004C1160: trio 0x5122 -> leader +0x3F0 |= 8). Shop state (+8 bits 0-2): 2 the merchant's
  words, 3 the item menu; player 2's X/B is the menu's back (action 5) only in state 3 -- in state 2 X moves
  the words on (Sekensor) and was also closing the menu. Autotest shopox.
* Freeze on a sector change (xxl_coop.crash.log, LV2-2): the game thread hung in a DirectSound buffer Release
  (CKSoundDictionary::reset -> FUN_0043C350 -> FUN_005F0ED0 -> FUN_005F3670) under the IndirectSound wrapper
  (dsound.dll, XAudio2; its log reports an XAudio2 critical error / device stopped responding). No mod code on
  that path.
* Machine gun alone / joined (2026-10-14): 0x4809 no longer stages the non-leading player's hero as an idle AI
  (so no AI order 7 for Obelix): Asterix uses the gun alone (mode 2) with co-op and the split screen on
  (GunSolo; shared.cpp SHARED_GUN_SOLO routes the whole gun map to Asterix's player). Obelix's X within 6 of the
  handles target (gun +0xD0 node: +4 -> matrix +0x80 place / +0x70 direction, as 0x4A05 answers; the AI's walk
  ends ~4 units off it) joins: held as the game's companion with AI order 7 to that target -> trio 0x480A on
  arrival (Obelix mode 2) -> suspended, one screen, SHARED_GUN (Obelix turns). Asterix's A (0x4806) ends it.
  Only the boat still needs both heroes. A lead taken for the gun is given back when the solo use ends
  (GiveBackLead). Autotest mgun33j. Hints: a new hint over a prompt already on screen (state 2) restarts the
  prompt (state 0) so it slides in to the hint's place.
* Merchant in split screen (2026-10-14): shopping no longer suspends co-op. The opener takes the lead (sit 0x0A,
  as before) so the shop's camera is the game camera = the shopper's half; trio 0x48EF from the shop entrance
  (caller 0x49BD4B) no longer stages the other player's hero (no AI order 14: it plays on). The HUD pass
  (DrawHud, 2D camera renderer +0x14) gets the shopper's half as its viewport (its beginUpdate wrapped like the
  main camera's): the merchant's words and menu (and the rest of the HUD) squeezed into that half; XXL_SHOPHUD=2
  also doubles the HUD camera's vertical view window (experiment). Input: the menu map and the Sekensor answer
  from the shopper's values only (player 2's menu presses now go into player 2's values). The lead is given
  back when the shop closes. Autotests shopsplit (P2 shops), shopsplita (P1 shops; its synthetic P1 input
  does not fill the menu's back binding, so it does not leave).
* Merchant follow-ups (2026-10-14): (1) the world's pause FUN_004FFCD0 (thiscall gm, 1 = on; every Life of the
  enemy groups gets +4 bit 1, skipped by the bundle tick; also used by spies) is skipped while a merchant opens in
  co-op (detour). gm +0x5C bit 26 = shop HUD mode (in-game interface FUN_00515830 hides the normal HUD; blocks
  the pause menu). (2) The merchant's camera pushes the game camera's far / fog planes out (60/48 -> 100/80):
  camera copy now records its own near/far/fog (RwCamera +0x80/+0x84/+0x88) after its tick (GameCamPlanes) and
  each split half uses its player's. (3) Shop HUD in the shopper's half (XXL_SHOPHUD 1 squeezed, 2 half size,
  3 default): HUD camera (renderer +0x14) viewport = the half, view window y doubled (half size, centred band);
  2D managers at yellow pages +0x34 -> +0x34 / +0x38 (CScene2d: +4 first child, +0x10 next, +0xC count) and
  +0x3C (CMenuManager): elements (CBillboard2d 0x6235C0 ... CText2d 0x623B6C; +5 bit 2 = shown, +0x14/+0x18
  place, +0x34/+0x38 size; containers' child lists at +0x24/+0x28) near the HUD's top moved up / bottom down by
  half a HUD, full-width bars twice as high; the HUD's 3D models (life shields, helmet) follow the HUD camera's
  frame, moved down along its up axis by 0.5 (2D elements ignore the frame; RwCameraSetViewOffset has no
  effect on either). Restored after the HUD draw. Autotests shopsplit / shopsplita.

## Round 40 (Oct 14): death focus, individual heroes

- **Health** = game manager +0x40 -> +0x50 (points, 5 a shield), +0x51 (shields 3..6; a respawn, gm vf13, sets 3).
  Heal FUN_004BB260 / add shield FUN_00507210 (thiscall gm, ret 4; bonus pool types 5 / 4; merchant FUN_004BFC50
  writes directly). Hero_PreLifeTick sets hero +0x390 bit 2 ("dead"; trio.h's `Busy()`) from health == 0; the
  situation chooser then always picks 0x0E (death, handler 0x49C720: at the end of the animation with no health it
  sets gm +0x2FD = game over). The chooser never leaves 0x0E by itself: revive = exit handler (mode 2) + +0x3E9 = 0xFF.
  The **game manager's tick** (FUN_00504080) with health 0 starts the death sequence (FUN_005008D0: latch gm +0x188,
  level event group gm +0x166 -> death cutscene bars, gm +0x5C bit 20) and the reload (FUN_00407120 when +0x2FD).
- **Death pit**: FUN_0049D610 (hero tick) on ground with flags +0x2A bit 2 takes all health; an *AI* companion then
  sends trio 0x4800/0x4801 (it leads, so the death shows on it). Ground: hero +0x280, CGround vt 0x624258,
  CDynamicGround vt 0x6241BC; on the ground = +0x384 & 7 == 0.
- **Hits**: hero sendEvent (CKHkAsterix/CKHkObelix vtable slot 5, shared 0x48BC70), class 0x03xx -> FUN_00495FA0.
  Event 0x4C0E (a hit landed) fills the **combo bar**: gm +0xB0 hits, +0xB4, +0xA8 decay timer (reset from gm +0x9C),
  full (cb +0x10 bit 0x4000) -> gauge gm +0xA4 (max +0xA0), +0xC4, level gm +0xCC bits 9-10; runs down in the gm tick.
- Co-op death focus: the hero whose tick / event took the last point gets the lead (CoopGiveLead), the other is AI.
- Individual heroes (individual.cpp): per-hero copies of the above swapped in around each hero's Life tick, its
  sendEvent, heal/shield pickups (trio leader byte = picker), the in-game interface tick (player 1's hero); default =
  the leader, or the standing hero (StandingOr) so the gm tick never sees a fallen hero's 0. Fallen hero: own 0x0E,
  gm +0x2FD cleared after its tick, 0x03xx events dropped, excluded from the "busy" suspension; revive = partner within
  3 units holding grab (binding +0x18 hold time; P2: p2HoldTime[6]) 10 s, or a sector change. Pit: laid on the newest
  safe spot >= 0.7 s old. Font of the HUD prompt has no '|', '#', '%', '>', '~' (has '=', '-', '*', 'o', '+', ':').
- Round 41 fixes: during a game over the per-hero swap stops (IndivEnter no-op) -- the respawn wrote 15 into one
  hero's place, a later swap put the other's 0 back and the game killed it again (slide, user log). Revive 8 s, half
  the health ((shields*5+1)/2), gm +0x5C |= 0x4000 so the HUD reads it again. Fallen heroes count as "out" for all
  proximity slicing (HeroOut): Romans/lions no longer get them as their leader (down2a: 22 hits -> 0). Preset 1 the
  hook never saw being chosen (boot pick before the ASI's hooks) with a pad visible -> 3.

## Round 42 (Oct 14): player 2's HUD (hud2.cpp)

- In-game interface (2/132) pieces: health = FUN_0051A140 (state +0x24 bits 4-5; fields +0x1E4..+0x277; 30 CNode
  shield models +0x2C..+0xA0, node i = shield i/5 fill i%5, 0.08 apart; sparkles CParticlesNodeFx +0xA4); combo bar
  = FUN_00518190 (bits 12-14, +0x2A0..+0x2EF, models +0xB4..+0xCC: a column at the left edge) + its glow FUN_00519E70
  (bits 23-24, +0x2F0..+0x30F, +0x494..+0x49F); helmet counter = FUN_0051C160 (model +0xA8, plate CBillboard2d +0x134
  drawn leftwards from its x, texts +0x150..+0x158); x3/x10 sign FUN_0051E440 (+0x130, +0x13C). Show triggers in game
  manager +0x5C: 0x4000 health changed (consumed by the health part), 0x1000000 HUD button held (action 0xF), 0x40.
  All parts are thiscall(interface, time), ret 4. CNode = 0x20 bytes (+0x10 flags, bit 1 hidden; +4 frame).
- HUD camera (renderer +0x14): parallel, view window 0.5 x 0.5, frame right = -x, at = +z, at (0.5 0.5 -1); screen x
  = minus the camera-space x; 2D elements' x in 0..1 = screen x + 0.5.
- Player 2's HUD: the health/combo state (bits, ranges, element flags/anim/frames) kept as a second copy, ticked after
  the game's with player 2's stats and show flags (Back held on pad 2), swapped in for a second HUD pass (repeat walk
  into the 3 HUD lists, other interface models hidden, FUN_00414210 + lists vf3 between Rw camera begin/end, no 2D),
  the models' screen x negated (mirrored to the right edge). The interface writes modelling matrices only; LTMs are
  recomputed from modelling x parent LTM when a copy is applied. Helmet counter moved to the top middle for the
  game's pass. Player 1's HUD flag 0x4000 = player 1's own stats changed. Back hold no longer leaves (individual).
- Round 43 (user test of the P2 HUD): FUN_0051B550 (which shield models show, from the health; also run by the game
  manager's FUN_005154A0 and the interface's vf15) now runs with the HUD owner's stats (detour; vf15 slot patched).
  FUN_005154A0 (gm, on its HUD toggles -- a hero going down) sets the health part to state 0 and shows the models
  (pause menu); outside the pause menu nothing hid them: shields hidden whenever the part is in state 0 and gm +0x5C
  bit 20 is clear. P2's particle nodes (+0xA4 chips, combo sparkles) finish in the HUD walk: captured back into
  P2's copy after its pass (they had stayed emitting, flags 0x02D1). P2's shields shifted as a block (the last one
  rightmost, first to go) instead of mirrored; combo column mirrored. x3/x10 sign (+0x3A0 sliding to +0x31C from
  1.01) drawn rising off the top instead.
- Heroes walking into each other: no contact event between two heroes; the mover's frame vertical step (+0x94)
  tilts up sliding along the other hero's sphere (the taller Obelix rose onto Asterix). In single player the AI
  companion just blocks. Rising within 2.2 of the other hero on the same level without a jump press (+0x3EC bit 8,
  1.5 s grace) is undone after the tick (HoldOffPartner). Autotests bump2 (co-op), bump1 (single player).
- Round 44 -- the "automatic jump" (supersedes the HoldOffPartner note above, which is removed: it left the jump
  animation and sank heroes into raised ground): hero contact event 0x0300 (list +0 next, +4 volume pair, owner at
  volume +0x20). Touched by a player-driven hero (+0x18 bit 0 clear), a hero is pushed aside and, moving into it,
  hops out of the way: FUN_0049CDF0 sets vertical speed +0xA8 = 12 (call at 0x48C32E) -- the AI sidekick dodging
  the player; touched by the sidekick, the player's hero gets neither (blocked). Two players' heroes both took the
  sidekick's part. Fix: while a hero handles a contact with the other player hero, that hero's +0x18 bit 0 is set
  (passes for the sidekick). Found with a rise watch on +0xA8 (autotest S_WATCHRW x=7). Not Idefix (bump2i).
  Correction (user): passing for the sidekick made the heroes go through each other -- the player's hero touched by
  the *moving* sidekick takes nothing. Touched by a *waiting* sidekick (AI state +0x561 == 3 or 9) it is pushed back
  (blocked) without the hop (bVar6 = 0). Now the other hero passes for a waiting sidekick (+0x18 bit 0 and +0x561 = 3)
  for the length of the handler: both solid, the runner pushes the other along, no hop, no lift (bump2/bump2i).
- Round 45 (individual heroes, user test): bonuses are applied by the pool's vf40 (CKGrpAsterixBonusPool 0x506E90,
  thiscall(pool, hero), ret 4 -- CKHkBasicBonus's tick passes the hero it flew to); the mod had credited the trio
  leader (a shield picked up by P2 went to Asterix). vf40 is wrapped with the collector's stats; the heal / add-shield
  hooks keep the current hero inside any IndivEnter scope (leader only as a fallback). The health part takes the
  "health changed" flag only in its hidden state (a change while it slides away waits for it): each player's flag is
  now kept pending until that player's HUD takes it (P1's HUD missed changes right after a revive). P2's HUD: the
  layout mirrored (models placed at negated screen x, drawn as they are) -- a mirrored HUD camera showed the shields'
  back faces whatever the cull render state (0x14) was set to: the game's draw sets culling itself. Autotests
  shield2o (LV2-1 shield), revshield (-Level 2: pit, revive, then a shield given the bonus's way, S_GIVE).
- Round 46: P2's HUD fully mirrored (each shield eaten from its left). The HUD draw lists set the cull mode
  themselves (CAnimationManager vf14/21/28/35/42/49 call the engine's render state function, engine +0x20, with
  0x14 = 1/2/3), so for P2's pass the HUD camera's right axis is negated and engine +0x20 temporarily points at a
  wrapper that swaps cull back (2) / cull front (3); restored (and cull back set) after the pass.
- Round 47: the mirrored P2 HUD showed the combo bar's "Combo" word mirrored. Shields and the combo bar are in
  the same HUD draw list (0) -- tested by swapping the cull mode per list -- so P2's HUD is two passes: health
  (mirrored camera, cull swapped) and combo bar (mirrored camera, each combo model's frame turned round on its own:
  mirrored twice, normal winding, the game's culling). P2's copy is captured after un-hiding the other part's
  models. Autotest combofull2 (S_GIVE b=2 sends the hero 0x4C0E x40) fills the stats but not the bar's full look
  (also P1's) -- the user checks the word in play.
- Round 48: six shields per hero (merchant) at the single player HUD's size crossed the screen's middle (into the
  other player's HUD and the helmet counter): with more than four, each row is scaled from its first shield (sizes
  and spacing, LTMs only) to 0.28 between the first and the last (shields6). P2's X in the merchant's item menu no
  longer means "back" (left the shop; P1's X does not): B only.
- Round 49: helmet counter centring moved +0x154 too -- the slide's helmet requirement text ("0/1500", under its
  own plate +0x138, FUN_0051CD80), not the counter's: only +0x134 / +0x150 / model +0xA8 move now. Health bonuses:
  CKGrpAsterixBonusPool::vf39 (0x507320, thiscall(pool, hero), ret 4; the pool update asks for the leader, the mod
  for the other player) only refuses the potion pool in the game; in individual mode a ham (pool type 5) only goes
  to a hero below its own shields' health, a shield (type 4) to one below six, nothing to a fallen hero (autotest
  shieldfull). Preset: the pad's layout goes into the presets at boot only when a pad is seen (vf10(1) ->
  FUN_00503B00, done flag game manager +0x5BC); xxl_coop.its_back.log: none at boot, so the mod's preset 3 was a
  default table (face buttons A0+0F..12 instead of 0E/10/0F/11). Now FUN_00503B00 is run first if +0x5BC is 0
  (safe in a level: tested by forcing it).
- Round 50: a HUD shown second left its shield look on the other's (user screenshots, either order): FUN_0051B550
  also sets each shield's paleness -- the alpha byte (+7) of the colours in the shield node's list (node +0x1C:
  item +4 next, +0xC colour; 0x50 empty, 0xFF otherwise) -- shared by both HUDs. Now part of each player's copy
  (autotest hudorder: P1's HUD by a heal, P2's by Back, both orders).
- Round 51: snow (CFogBoxNodeFx) fast again in split screen: P2's HUD pass walks the scene (up to two walks a frame,
  g_walkPlayer 0) and the fog walk hook stepped the boxes there with no restore. The HUD walks are flagged
  (g_hudWalk) and leave the fog boxes alone. Other walk-dt users to watch with any new walk: particles (stepped-once
  guard), sky (walks 1/2 only), anim blends (repeat walk), fog (now). User-verified in fog21.
- Round 52: cheats -- P1's R3 cheats used Player1Pad() (pad identification, can take a minute): any connected pad
  other than P2's until then; R3/Back + Y toggles the potion (empties to 0.001 so the game's countdown ends it);
  D-pad left fills the presser's own combo bar (IndivEnter). Twister "stumble" (user, a modded level full of
  Romans; also with Obelix leading): the hero contact handler pushes a hero back from touching enemies (push vector
  +0x22C..+0x234) unless +0x38C bit 0x4000 -- not set in the Twister for either hero; the Twister picks Romans up
  before they touch it in vanilla levels. In co-op the push is now undone for a hero in the Twister (attack state
  +0x135 == 10). Not reproducible in vanilla levels: user to check in the modded level.

- Round 53: merchant (user, LV2-1). (a) P1's Asterix froze while P2's Obelix was in the shop's item menu: the menu
  turns the gameplay bindings off (FUN_00437F10(CKInput, 0, 0), thiscall, ret 8; group 0 gameplay, 1 menus; binding
  +4 = active) -- P1's controls; P2's are the mod's own values, so only P1 was hit. While P2 shops, group 0 is put back
  on each frame. (b) The other player setting off a Roman agent's dialogue (trio 0x5120) with the shop open crashed:
  the shop is now closed first -- FUN_004C1040(shop, 0) (state machine, handlers at shop +0x1A8, +0x1AD current),
  FUN_004BF670(shop, 0) (hides the menu's pieces +0x28..+0x34: vf 0x40 show / 0x44 hide; the menu tick only hides
  them on a purchase, the game's reset vf16 does too), trio 0x5124 (natural end); the merchant won't reopen until
  the shopper is out of situation 0x0A. Autotests shopsplit (P1 moves; log line "controls back on"), shopspy (menu
  open, state 3, closed by the dialogue, no crash, no leftover legends).
- Round 54: merchant states (handlers at shop +0x1A8, this = shop +0x1A4, (code 0 tick / 1 enter / 2 exit, x)):
  0 idle (FUN_004C1160), 1 walk up (enter: world pause FUN_004FFCD0(gm, 1), gm+0x5C |= 0x4000000, shop camera), 2 the
  merchant's words (exit shows the menu pieces), 3 menu (enter FUN_004BFAD0: bindings group 0 off + values zeroed,
  group 1 on; exit FUN_004C0200 back), 4 goodbye words, 5 leaving (FUN_004C1B90: enter restores the camera; exit
  unpauses and clears gm+0x5C 0x4000000; its tick goes to 0 and sends trio 0x5124). gm+0x5C 0x4000000 = at the
  merchant: the game manager's tick shows (FUN_00515830) / takes down (FUN_00515A30) the shop's screen (bars) on
  its edges, mirrored in bit 0x8000000. User: a shop closed in states 1-2 left the bars after the agent's dialogue
  -> the forced close also runs state 5's exit. User: P1 paused for a moment when P2's menu came up (the zeroed
  values, a frame before the re-enable) -> the menu's call at 0x4BFB1B skips "group 0 off" while P2 shops
  (the per-frame re-enable stays as a fallback). Autotests shopspyw (state 1), shopspy, shopsplit(a).
- Round 55: user: shop closed while the merchant talks (state 2) still left bars and the merchant open after the
  agent's dialogue. The bars are the shop's own: +0x38/+0x3C (top/bottom), slid in by FUN_004BF980 in state 1, out
  in state 5 and only then hidden (vf 0x44); shop+8 bit 0x40 = sliding; shop vf19 hides both at once. The merchant's
  pose (+8 bits 3-5; FUN_004C0ED0(pose, dt): 0 rest, 1/4 talking, 3 leaving) stays the talking one while +0x1CA bit
  0x20 (talking, set by state 2's enter) is set and his line plays (speech service yp+0x84: vf 0x68 play, 0x6C stop,
  FUN_0046F590 done). Forced close now: stop the line, clear 0x20, state 0, menu pieces off, state 5's exit, vf19,
  pose 0. FUN_00515830/FUN_00515A30 (gm tick, gm+0x5C bit 26 edges, mirror bit 27, bit 28 = HUD hidden) only
  hide/restore the HUD. Autotest shopspyw (state 2 now): state 0, pose 0, bars flag off.
- Round 56: experimental rules ([experimental], read at startup, single player too; experimental.cpp).
  Merchant prices: shop +0xE4, 12 uint32, scaled after CKHkAsterixShop::deserialize (0x4C0C50). Squads
  (CKGrpSquad::deserialize 0x52DE80): pool records (squad +0x144, 0x1C each, count +0x14C): +0 CKGrpPoolSquad,
  +4 enemy cpnt, +8 member list, +0x10 U1 (alive after a restart: vf33 else-branch / FUN_0052D460), +0x11 alive
  (loaded as U2 = placed when the squad first wakes), +0x14 queued, +0x15 fallen to replace, +0x16 sent in so far,
  +0x18 to defeat (int16, < 0 endless). Remaining = +0x18 - +0x16 - +0x15 - +0x14. Squad scripts (message actions)
  run through vf25 (0x4C7F80, thiscall(cmd, args) ret 8): 5 cond alive (vf43 = +0x11) vs args[0] (op args[1],
  pool args[2]), 8 queue (vf53), 12 send in at a point (vf55), 16 cond sent in (+0x16), 19 replace (vf65), 20 keep
  N alive (vf54); 6/7 distance conds, 0/1/2/9/10 choreography. LV4-1 squad 1: "pool 0 alive <= 6 -> replace 1";
  LV6-4 squad 8: "pool 0 alive <= 7 -> keep 8", "pool 1 alive <= 2 -> keep 3", "pool 0 alive == 0" (phase).
  Romans come from the pool's free list (vf12 0x30, chained +4; FUN_00530380): its size caps Romans at once (LV6-4:
  8 for squad 8's pool, 27 basic Romans in the level). Scaling: every count x squad_romans_to_defeat (and cond 16);
  pools sent in by script: U1/U2 and script counts (5 >= 2, 8, 12, 19, 20) x squad_romans_at_once; placed-only pools
  (count <= placed: set pieces, LV4-1 squad 21's hiding Romans) keep their spots and get the rest as a wave once
  all have fallen (vf37 hook sets +0x15). LV4-1 gate: 100 -> 200 (autotest lock4). Env XXL_SQUADCMDS logs scripts.
- Round 57 (user's play of round 56): placed-only squads (count <= U1/U2: hiding Romans, lone guards) are left as the
  level has them -- a raised count there met the gate early and the top-up wave ran at the players; top-up removed.
  Kept by enemy type (pool cpnt vtable): JetPackRoman 0x627C90, MobileTower 0x627D20, turtles 0x627D50 / 0x627DB8 /
  0x627DEC / 0x627E20 (LV4-1 gate 194). Script numbers at once only while no player's hero is held (CoopHeroHeld;
  cable car autotest swing). Combo bar: hits gm+0xB0 (+1, +3 potion; added in four places: FUN_005093A0, 0x480000,
  2x 0x4E0000), level thresholds gm+0xB8..+0xBA, full at gm+0xC8 (bytes; FUN_0048C860), HUD fill hits / +0xC8, full
  bar refill +0xA0 / +0xC8 -- combo_bar_fill divides the thresholds (ExperimentalFrame; 0/7/16 full 30 -> 0/14/32
  full 60). Slide / boat (co-op suspended): the one HUD shows the leader's stats (hints.cpp; the bar redrawn on the
  switch).
- Round 58: fight gates: each squad with +0x154 >= -1 (-1 all pools, n pool n; 0xFE none) adds count - sent + alive
  to gm+0x64 every frame (FUN_0052D340, from CKGrpEnemyLife's tick) -- the gate's number. Kept pools' doubling is
  given to the biggest doubled pool counted by the same gate (ExperimentalFrame; LV4-1 194 -> 200). Combo drain: a
  hit lost every gm+0x9C s before full (0.65), a full bar lasts gm+0xA0 s (30): combo_bar_drain /
  combo_bar_full_drain divide them. One screen (co-op suspended: slide, boat): player 2's HUD pass now also runs
  after the game's own HUD draw (FUN_00414070 detour; hud2 on while suspended), so Obelix's bar shows on the right.
  P2 Start / Back-hold leave ignored while suspended (leaving mid-slide left both heroes stuck).
- Round 59: things that need both heroes stay shut while one is down (individual heroes): the standing hero who reaches
  one is put back on its last solid ground (IndivBlockPartnerSpot; safe spots: static CGround only, not a blue circle's
  plate, not where a launcher offers itself, a spot blocked again within 1.5 s forgotten). Grounds (hero +0x280) of
  lifters (2,47: +0x50 dyn / +0x54 static), the corkscrew (2,44), cable cars (2,158 / 2,159) -- any field pointing at
  it -- and blue circles (2,21 +0x28, events +0x30/+0x32 commanding the trio); launchers: hero +0x3F0 bit 0x4000 in
  the prompt (FUN_0049A1A0) -- bit and prompt cleared; detector zones (CKSrvDetection::vf17 0x46BE90, flags on
  outside->inside, no auto-disable) whose group commands the trio: event dropped; a Roman agent's 0x511F / 0x5120
  dropped. Checkpoints (CKGrpAsterixCheckpoint::vf29 0x536DD0, leader in radius) also send leader switches (the
  cable car, the slide, LV2's boss gate) -- not blocked (progress saves). Autotest env XXL_DOWNAT=<hero>:<ms> puts a
  hero down in any scenario. Verified: cable car, spy, launcher, LV4 blue circle zone; lifter / corkscrew tests only
  place the hero beside them (user to check).
- Round 60 (user's play of round 59): the cable car is taken from the hero's own tick (0x49ACB9 "Obelix leads",
  0x49ACC7 "Asterix rides", 0x4812 AI order 9) before the frame check -- those events from there are held back, the
  hero reset (StartAiCycle, as R3+B) and put back. Grounds: only the objects' CDynamicGround (vt 0x6241BC) count (the
  station grounds a cable car points at teleported the hero around it); the check also runs after each hero tick
  (IndivAfterHeroTick), before the objects tick (blue circle / spinning platform were set off in the same frame).
  LV5-8's lifter: hero ground = lifter +0x50 (dynamic), blocked (autotest liftd58). Hints: the game's own prompt
  taking the plate over gets its slide places back (+0x42C/+0x434 = +0x428; the boat's "Use" plate stayed where the
  hint had put it). Same-level reload (Inspector): gamecam's copy kept the old level's tracks (same camera service)
  and wrote them back into freed memory -> crash; now dropped when the trio changes or a track is no longer in the
  service's list (autotest reload2, S_RELOAD). One-screen P2 HUD pass (slide, dialogue) stepped the snow, particles,
  sky and anim blends again: the walk hooks treat it as a repeat walk. Preset: its_back2.log -- the level ran on
  preset 3 with A0+0F..12 (no pad at boot, layout flag set anyway), the title screen later rebuilt A0+0E..; which one
  the user calls swapped is unclear -- asked.
- Round 61 (user: still through the dialogue, blue circles, cable car): the objects went on by themselves -- the
  spy's dialogue cutscene started with no trio event once the hero came back (xxl_coop.dialogue.log); the cable car
  put-back went to a spot 3 units off, walked back onto, and the "same spot again: forget it" rule left no spot
  (xxl_coop.cable_car2.log). Now the objects' own ticks are held (proximity TickLife -> IndivHoldsBack) while the
  standing hero is at one with the other down: spy (spawned, within +0x70), blue circle (+0x28 ground, events command
  the trio), corkscrew / lifter / cable cars (own CDynamicGround); put back to the newest safe spot >= 4 units away
  (SafeSpotAway). Preset (user): wrong right at level 2's start (force_level), right after the title screen. The
  title's table (vf10(1) -> FUN_004FF1E0 + FUN_00503B00 with the profile's options) is A0+0E/10/0F/11/12...; at a
  force_level start the same calls give the default A0+0F/10/11/12/13 (options not loaded) -- force_level-only.
- Round 62: put-backs from what needs both heroes go along a trail of safe spots one every 2 units (16): the newest
  >= 6 units away (cable_car3.log: every put-back went to the boarding platform 2.4 units off, the 8 time-spaced spots
  being all around the car). The belly slide's slope (ground +0x2A bit 3 "Slide") is held while a hero is down
  (slide.log: the start zone held back 10 times, then the slope walked down to the level's end). Pad layout: the
  title's preset 3 (FUN_00503AA0's FUN_00438950(3) from 0x503AED with a pad) is saved per binding as device /
  index / offset (xxl_coop_padlayout.txt) and put back when a level started without it (force_level: the mod's
  switch from the keyboard preset 1).
- Round 63: (1) the object-tick hold never ran in play: BundleTickHook hands every bundle to the game when a player
  hero is out (HeroOut -- a fallen hero is), so TickLife / IndivHoldsBack were skipped exactly while a hero was down
  (the spy restarted its dialogue by itself, dialogue2.log). That branch now mirrors the bundle tick with the hold.
  (2) The cable car: the hero's situation 2 (FUN_0049A670, +0x480 the car's node) boards on a later tick -- the car's
  own 0x512B, then trio 0x4801 / 0x4812 -- and from then on the car holds the hero (put-backs ignored, frozen on it,
  cable_car4.log). Situation 2 is now left at once (table[2](owner, 2, 0xFF), +0x3E9 = 0xFF, only when table[2] is
  0x49A670) and the hero put back. Autotests cabled2 / spyd2 (three approaches each, XXL_DOWNAT=1:1000).
- Round 64: LV2-2's belly slide starts at a cross detector (plane z -903.82, event 0x0028, group 5, bound to Obelix,
  fired for Asterix only by the co-op swapped pass): its group sends 0x480B to CKAsterixSlideFP (vt 0x6259E4), not to
  the trio. With Obelix down the swapped pass did not run (HeroOut) -- the plane was simply walked past and the slope
  walked down (slide2.log). Now the swapped pass runs when the only hero out is the fallen one, and zones whose group
  needs both heroes (GroupNeedsBoth: trio commands, trio ride modes 0x4806/07/0B..0F, any message to the slide path)
  are held back with the put-back (ZoneEventHero already names the hero who crossed, swapped pass included). Last
  resort of a put-back: the hero's own position a second or more ago (every 0.1 s). Autotest slidecross.
- Round 65 (slide3.log): a put-back went to a trail spot 0.12 past the slide's line (recorded in the crossing frame),
  so the hero ended up on the far side. Zone put-backs now use the hero's position the frame before -- the detection
  service's own (det +0x1C, by the hero's node index, swapped pass included) -- 1.5 units further back along the way
  it came; spots noted in the last 0.5 s are dropped on every block. Autotest slidecross3 (three crossings: all put
  back before z -903.82).
- Round 66: dialogue put-back from where the hero was when the frame began (its own record; the detectors' previous
  positions are the game's, always kept). Locked sequences (user: a hero waiting in the rope cable car / on a blue
  circle / the rotary beam softlocked when the other fell): trio 0x4802 sets the companion's AI order +0x564 = 3
  (wait; 0x4803 -> 0), the cable car holds the rider (order 9). At the lock the detection service's enabled bits are
  kept (5 lists, 8-byte entries, bit 0 of byte 4); if the other hero falls while it still waits / is held: rope cable
  cars (CKHkTowedTelepher 2,159 only -- not the automatic CKHkTelepher) within 12 units get 0x512D and vf16 (back to
  the start), the detectors are set back, trio 0x4803 + the lead, the hero released (AI reset pass) and put on solid
  ground. Autotests swing / lockcircle with XXL_DOWNAT: both undone, the hero free.
- Round 67 (crash_logs, Oct 16-17): (1) crash at level start (force_level) in CKInput_UpdateBinding 0x435A9A: a
  binding's per-preset previous-value pointer (src +0x18 + preset*4; current one at +0x28, compared while +0x1C
  "wait for release" is set) was null for preset 3 -- the pad-layout restore set only the value pointer (+0x10).
  The binder (CKServiceManager::vf62) puts it one block on: device A (+4, 0xCC) +0x18, B (+8, 0x68) +0xE, C
  (+0xC, 0x3FC) +0x90; the restore now sets both. (2) Four freezes (game thread in NtWaitForAlertByThreadId = a lock
  wait, watchdog still logging; two right after a music stream started): cause not found -- the stall report now
  dumps every thread (any module) and critical sections on the game thread's stack with their owner.
  (3) A Roman agent's zone (group -> CKHkClueMan 0x5198) let through with a hero down made the spy ask the trio
  (0x5120, dropped) and the zone never fired again -- now held back whole (GroupNeedsBoth). (4) Launcher: no longer
  blocked; the fallen partner is revived beside the hero at it (user: a fallen hero can lie out of reach).
  (5) Undo of locked sequences: blue circles' visual is a CKHkLightPillar (2,60; vt 0x629030) switched by the
  circle's group (0x900 on, 0x901 off; the hearth in LV4 sector 2 sends 0x900, the circle zone 0x0008 0x901) --
  the lock's starting event is kept (picture taken in FlagEventHook before the group runs) and its pillars get the
  opposite message. CKHkCorkscrew (2,44) is its own circle: vf15 flags +0x40 when the leader is within +0x3C of its
  sub node and sets +8 bit 0, cleared only when its turn ends (or 0x99C / vf20) -- its +8 is restored. Towed cable
  car vf16 (= CKHkTelepher::vf16, its init) hides the node (+0x10 bit 1): shown again. Restoring all models'
  hidden bits was tried and dropped (crates broken meanwhile came back).
- Round 68 (user: spinning platform still dead after Obelix fell, xxl_coop.CKHkCorkscrew.log; cable car stuck when
  pulled far). LV2-1 platform flow: Asterix on its circle -> event 0x0050 (trio 4801/4802, pillar 0x901, detectors);
  Obelix walks to the turnstile through zone 0x03C8, whose group sends the corkscrew 0x99C + 0x99A (switched off),
  trio 4803 (Asterix freed), detector toggles, CKCameraSector 0x700, CKPFGraphTransition 0xA01; the box at the
  platform's top (Asterix) switches it on again. Obelix falling after 03C8 left it off for good. Now a lock ended by
  4803 keeps its picture while a corkscrew in it differs in +8 bits 0/1; a fall then restores it (+0x28 = 0, pillar,
  detectors). Autotest corkdown. Cable car: the rider's own position stays at the boarding spot, so the undo's
  "car within 12 units of the rider" missed far pulls; the car nearest the rider at the lock is kept and reset.
  Autotest cabled33 (-Level 3, pull to x 287). A fallen rider (Asterix) stays in the car; Obelix can take the rope
  again (user: fine as is).
- Round 69 (user: LV2-10 shop + save point, xxl_coop.broken_merchant.log): P2 opened the save point's menu
  (CKHkAsterixCheckpoint 2,193) while P1 shopped: bars + gm +0x5C bit 20, co-op suspended with the shop still open;
  after the menu the shop's camera stayed in P2's half and P1's shop input was gone. Now bars while shopping close the
  shop first (CloseShopFor, as for the spy's dialogue) -- autotest shopsave (-Level 2 sector 10; the save menu does
  not take the autotest's P1 values or keyboard keys: closed by hand). Joining while a shop is open is refused
  (CoopAllowedNow "at the merchant"; autotest shopjoin). Revive 5 s (was 8). Lifter "waits for" hint after 4 s.
  Keyboard P2: center_camera key (R3), default R in the shipped ini.
  Then (user: camera still stuck): the forced close left the camera service on the shop's track, which the save menu
  returned to. The shop's leaving-state entry (FUN_004C1B90(1)) hands it back: svc +0xC |= 0x400, +0xE0/+0x34 from
  shop +0x120/+0x11C, +0xE4 = 0, SetTarget(shop +0x19C, 1 s) (0x459050 -- the index's "Node_FadeVisibility" is
  CKSrvCamera::SetTarget), new track +0x48 = shop +0x1B0. Now done in CloseShopFor (user-verified with shopsave).
- Round 70 (user: xxl_coop.save+shop.log, xxl_coop.bro_test.log): LV2-10's zone by the save point (event 05A0,
  group 0B4: detection 1622, corkscrew 0999, trio 4800 + 4803, light pillar 0900) sends "Asterix leads" when
  Asterix walks in. In single player the merchant pauses the world, so it never arrives mid-visit; in co-op it took
  the lead from Obelix at the merchant: Obelix reset through the AI in situation 0A, the shop went on as Asterix's
  (camera in player 1's half, Obelix walking with the menu up), and CloseShopFor's camera hand-back went into
  Asterix's camera aimed at the shop's +0x19C (Obelix) -- player 1's view mirrored player 2's after the save menu.
  Now leader switches (4800/4801/48B7/48B8) wait while a shop's state is not 0 or a hero is in situation 0A
  (CoopDeferTrioEvent, sent once out); the lead taken for the shop goes back only once the shopper is out of 0A;
  CloseShopFor hands the camera back only if the game's camera is the shopper's (else GameCamDropCopy). Autotest
  shopzone.
  Combo bar: the game manager tick (FUN_00504080) ends the full state when the gauge runs out (stats +0x10 bit 14
  cleared at 0x504295); the mod's run-down for the non-leading hero (RunDownCombo) did not -- that hero stayed
  "full" with an empty gauge, hits went nowhere, the bar hidden until the hero led again. Fixed (autotest
  combodrain: log line). Game manager +0x160 bits 2 (a hit filled the full gauge) and 4 (a combo spent,
  FUN_00493810) are taken by the combo bar's state 4 (FUN_00518190) -- now per hero in Stats.
- Round 71 (user: xxl_coop.cinematic+shop.log): LV2-10's boss zone crossed by the other player while one shops: the
  cutscene's camera (plain CKCamera 6245BC) had taken the camera service before the bars closed the shop, and the
  hand-back (SetTarget + new track +0x48) went into the cutscene's camera; the cutscene's end went back to the shop's
  track (sh +0x118, which the shop's start put in svc +0x34: FUN_004BF6C0), so both views stayed on the shop. Now the
  hand-back waits until svc +0x38 is the shop's track again (WatchShopCamera; a camera copy on it starts over), aimed
  at the live camera's hero. The shopper is brought to the other hero when a cutscene closes the shop (game then
  places the companion by the leader). The shop also closes for trio 0x4804 (launcher / lifter: companion to its
  marker), 0x4807 (boat, once not refused), 0x480A..0x480F (slide and other rides) and when the streaming leaves the
  shopper behind (sector catch-up brings it over); not for cable cars (0x4812) or light pillars (0x4802/0x4803).
  Autotests shopboss (boss zone), shopsas (Sas to LV2 sector 2; shopsas0 = same without the shop -- the cameras in
  its screenshots right after the scripted jumps look the same either way, the user saw it fine live).
- Round 72 (user: xxl_coop.shop+boss_tp.log, xxl_coop.shop+launcher.log): LV2-10's boss run (zone 00A0, no cutscene
  once the boss's cutscene has played) brought the shopper along with its shop still up: BringAlongForBossRun now
  closes the shop first (autotest shopbossrun). LV4-5 launcher with player 1 in the item menu: player 1's pad dead
  after the landing. The menu (FUN_004BFAD0) turns the gameplay bindings off (FUN_00437F10 group 0; per binding +4)
  and its exit (state 3's handler 0x4C1A40, mode 2 -> FUN_004C0200) on again -- but the forced close ran inside
  Obelix's tick (player 2's X on the launcher -> 0x4804), when CKInput +0x18 is player 2's copy of the bindings
  (hold times), so the "on" was thrown away with the copy. Now whatever the game changes in the copy during player
  2's tick (all but +0x18..+0x1B) goes back to the real table (autotest shoplaunch4). Lifters do not send 0x4804
  (every 0x4804 in the test logs came from 0x49C699 with a launcher; LV2-1's lifter waits for both players): the shop
  stays (autotest shoplift). Closing the window could hang in the game's own shutdown (game thread waiting in its C
  runtime / DirectSound teardown, seen in older sessions too): the watchdog ends the process after 10 s with the game
  manager gone and no frames.
- Round 73 (user: xxl_coop.combo_bar.log, LV6): one player's bar going full left the other's held at the top, red,
  never reaching "Combo". The combo bar's level bounce is in interface +0x24 bits 27-28 (FUN_00519B00, run in
  state 2; count +0x2EC), set to 3 on entering the full state (FUN_00441B60(3,0x1B,2)); P2's HUD copy swapped
  bits 4-5, 12-14, 23-24 only, so the bounce went to the other bar (bounce 3 sets 0xC4's fill to the top; its own
  count 0 ran past 0 = 255 bounces). Bits 27-28 now per HUD (autotest combomix: hits given in batches, S_GIVE b=3).
  Pause menu: gm +0x5C bit 26 = pause asked, bit 27 = HUD hidden for it (FUN_00515830 hides, FUN_00515A30 shows);
  P2's HUD passes are skipped while bit 27 is set (combomix flips bit 26 with S_POKE8 a=-2).
- Round 73b (user: P2's bar and shields still over the real pause menu): the pause menu is game manager state 2
  (gm +0xCC bits 0-1, FUN_00503D80(2) after action 0x0C, with CKHook_GoVanish hiding the in-game HUD first) and
  puts the pause interface (vt 6287C0, type 4) in gm +0xF8 -- bit 27 of +0x5C is not involved. P2's HUD passes now
  need gm +0xF8 == the in-game interface (type 9). Autotest combomix opens it with S_POKE8 a=-3 (state b); Escape
  and P1's injected Start do not reach the game's pause in autotests.
- Round 74 (user: xxl_coop.lv5-3-block*.log): LV5-3's big block (CKHkPushPullAsterix 2,147, vt 626110) after the
  powder keg: once Obelix pushes, the level sends the block 09A7 (toggle +0x14 bit 0; with a hero pushing, +0x18 bit
  4, the trio gets 090C(bit): the leader's +0x3F0 bit 6, "pushed object slides on its own" -- FUN_0049A670 state 3
  then moves the hero at +0x470 without input) and 4 s later 09A7 again (090C(0) ends it). The block's tick
  (vf15 0x4B5140) clears +0x18 bit 4 when the trio's leader (trio vfA0) is a hero it is not for (+0x14 bits 6/7);
  proximity slicing ticked it with Asterix as the leader whenever Asterix was nearer its origin (the block slides
  away from Obelix: 6.6 -> 10.7 units), so the second 09A7 found nobody pushing, never sent 090C(0), and Obelix
  pushed in place for good. A block a hero pushes (hero +0x484 == block, +0x3EC bits 25-30 != 0) now ticks with
  that hero as the leader (PusherOf). Autotest pushkeg5 (-Level 5): P1 X lights the keg at (-346.7 19.8 369.8)
  with the torch cheat (S_TORCH 0 then 6), P2 turns, holds X and pushes; Asterix glides beside the sliding block.
  XXL_NOPUSHFIX=1 reproduces the stuck push; XXL_PUSHLOG logs the distances.
- Round 75 (user: xxl_coop.mole_fov.log): Asterix's mole combo (P1 combo 3 in autotests) widens the field of view
  70 -> 100 and back. Render_CalcProjCamera (0x414B70, thiscall on the renderer = camera service +0x18) keeps the
  last FOV at renderer +0x28 and sets no view window when asked for the same; P2's camera tick (gamecam TickCopy)
  left its own 70 there while RestoreExtras put the live (wide) window back, so the mole's "back to 70" was skipped
  and both halves stayed wide. Renderer +0x28 is now saved / restored with the window (XXL_NOFOVFIX: as before).
  Combo cheat (pad: R3 / Back + D-pad left, Inspector export): the stats set straight to full never moved the HUD's
  bar (it turns "Combo" only on seeing +0xB0 cross +0xC8 in its state 2); now hits are set one short and the hero
  lands two 0x4C0E hits 0.6 s later (CombosCheat(true); already full: gauge refilled). Autotest molefov.
- Round 76 (user: xxl_coop.one_roman_at_a_time.log, LV5-6 squad 4, single player, x2 squads): squad 4 (pools 18 /
  40 / 24 / 8 leaders; script: phase 2 "queue 4 of pool 1, 2 of pool 3; pool 1 alive <= 3 -> keep 4; pool 3 alive
  <= 1 -> keep 2") sent pool 1's Romans in one at a time. Pool 1 draws on CKGrpPoolSquad 7 (7 Romans), which also
  feeds squad 40 (box x -200..-94, z -23..70, the same arena; 3 placed when it wakes, 10 to defeat). The doubled
  start (6) held 6 of the 7 for the whole fight -> 1 free. Pool record +8 is the member list (chained +0x50; the
  taken pool element: +0xC the Roman, +0x54 the cpnt; FUN_00530380). Fix (CheckSharedPools, first frame of the
  level, before any squad sends in): a pool record's U1/U2 go back to the level's numbers when its CKGrpPoolSquad
  also feeds a scaled record of another squad whose box (max +0x11C, min +0x128) overlaps (LV5: 13 records).
  Script numbers stay doubled. XXL_NOSQUADSHAREFIX: as before. Autotest fight56 (-Level 5, potion, 150 s; WATCHENEMIES
  now lists each Roman pool's holders by real members): pool 1 1 alive / 6-8 queued -> 4 alive.
- Round 77 (user: xxl_coop.lv3-6_asterix_then_obelix.log / _obelix_then_asterix.log, LV3-6): Romans ignored the hero
  in the open while the other stood behind the bars. Squad 4 (14 Romans; box x -201..-132 z -251..-168, fight zone
  x -194.4..-170.6 y -2.4..8.4 z -234.8..-208.0 = squad +0x104..+0x10C max, +0x110..+0x118 min) held both heroes
  and was run for the one nearer the box middle -- the one behind the bars, outside the fight zone: its script
  kept target mode 4 (+0x152 bits 2-4; 4 = fixed point +0x84, 5 = trio leader) and the Romans in formation round a
  fixed centre. Squad pursuit: vf31 (CKGrpSquad::vf31 0x52B2E0) moves the formation centre (node +0x30, pos +0x30)
  towards vf45's target (FUN_0052BAB0) and gives each Roman a slot (FUN_0052B670: Roman vf12(0x46) component, +0
  slot, +0xC facing, +0x18 step = (slot - Roman) x/z / max(dist, squad +0xC0)); the Romans' own (sliced) ticks only
  attack the nearest hero. Fix (proximity.cpp EnemyTickHook): both heroes in the box -> run for the one in the fight
  zone (else the box middle rule); both in the zone of a target-5 squad -> SplitSquad: half the members (records' +8
  lists) get their slots moved round the other hero (slot - centre + hero; side kept per Roman, newcomers to the
  smaller side, one Roman a frame rebalanced). XXL_NOSQUADZONEFIX: as before. Autotests sq36a / sq36b (one hero
  behind the bars; 3 -> 14 Romans on the open hero), sq36c (both in the open: 7 / 7, Obelix's 7 follow him).
- Round 78 (user request): Romans at once (squad scripts' numbers, AtOnceNow) also go back to the level's while a
  hero is down (individual heroes, IndivFallen) and come back on the revive; Romans to defeat stay scaled. Starting
  Romans (U1/U2) are set at load, as with held heroes. XXL_NOATONCEDOWNFIX: as before. Autotest sq36c with
  XXL_DOWNAT=1:12000 XXL_UPAT=20000 XXL_SQUADCMDS=1: "keep 2 alive" sent as 4, as 2 while Obelix is down, x2 again
  after the revive.
- Round 79 (user: Snipaste_2026-10-19_17-34-39 / 17-49-27 / 17-54-22, xxl_coop.lv3-2_p2_hud.log). (1) P2's combo bar /
  shields stayed on a dialogue's screen (LV3-2 agent): for cutscenes and dialogues the game vanishes its HUD --
  CKHook_GoVanish (0x515CB0) sets gm +0x5C bit 20 and hides the models of the parts showing (P1's, the ones in the
  interface), FUN_005154A0 shows them again when bit 20 clears; vf18 ticks no part meanwhile. P2's passes are now
  skipped while bit 20 is set (XXL_NOHUDVANISHFIX: as before; autotest hudvanish -Level 3). (2)+(3) HUD layout
  (hud2.cpp Xform: scale about a pivot then move; models' LTMs, 2D places/sizes, put back after the pass;
  XXL_NOHUDLAYOUT: as before). 2D elements: x 0..1 from the left, y 0..1 from the bottom; HUD camera space x =
  0.5 - 2D x, y = 2D y - 0.5. Flags byte 2 = horizontal alignment (0 left, 1 centre, 2 right: drawn leftwards).
  CText2d: text object inline at +0x20 (vt 0x623BCC = CText2d vt slot 24; draw 0x42F430, layout 0x42E840): box
  +0x28/+0x2C, font +0x680, font size +0x684 (font base x +0x6C4 / 512), +0x2F4 entries (words) placed at +0x3E8
  (x, then y counted down from 1) -- scaling box, size and places scales a text about its place. Pieces: gate
  counter FUN_0051F3C0 (bits 17-18; picture model +0xD4 + 4 x gm +0x68, model +0x110, plate +0x144, number
  +0x160 = gm +0x64; slides on +0x3F4 from the right to +0x3DC = 0.879; the gate's zone LV4-1 (-289.9 0 117.8));
  spy's helmet requirement FUN_0051CD80 (gm +0x5C bit 11; plate +0x138, text +0x154 slide on +0x388 to +0x318,
  model +0x12C on +0x394 to +0x33C; y 0.789); x3/x10 sign at y 0.789, or 0.635 under the requirement (hero stats
  +0x84 value, +0x98 time). Side by side: helmet counter (3/4 size, 0.1 lower, under the health rows), the
  requirement and the x3 sign under it in the top middle, both coming down from the top instead of sliding across
  P2's half; gate counter 3/4 size in the middle (it slides 0.1 into place). Stacked (split_mode horizontal):
  each player's HUD single player's way in their own half -- P1 as the game's, P2 not mirrored, moved down 0.5;
  combo columns 3/4 size, bottoms at 0.47 / 0.97 from the top (column bottom = model +0xBC, 0.35 up); helmet
  counter, requirement and x3 sign at the right as in single player; gate counter at the right edge, its middle
  on the halves' border. Autotests hudlayout / hudreq / hudx3 (-Level 4; Back held on both pads, gm +0x5C bit 6
  "helmets changed", bit 11 and the stats poked); env XXL_SPLITMODE=horizontal|vertical overrides the ini.
- Round 80 (user: Snipaste_2026-10-19_22-14-52, horizontal split): the shop's HUD in a stacked half had every letter
  half as high as wide -- mode 3 doubled the HUD camera's vertical view window, right for side-by-side halves only.
  Stacked (splitscreen.cpp ShopHudSideways) doubles the horizontal one instead: half size across the half's middle;
  2D elements spread sideways (x < 0.25 / > 0.75 moved by 0.5, full-width billboards twice as wide), the interface's
  models (+0x2C..+0x130: shields left, helmet right) moved by half a HUD along the camera's right axis (hud2.cpp
  Hud2ShopSides, LTMs, put back in Hud2EndP1); no frame shift. The restore of the view window now checks x too (left
  doubled each frame it went to inf and nothing of the HUD drew). XXL_NOSHOPHUDFIX = the old way. Autotests
  shopsplit / shopsplita with XXL_SPLITMODE=horizontal; vertical unchanged.
- Round 81 (user + friend, LV5-6: both heroes down -> crash, twice; logs from the friend's PC over ssh, saved as
  userlogs/friendpc-crash1/2.log; Windows: axxl.exe C0000005 at +0x11A9A0): the in-game HUD's health part keeps
  its place among the 6 x 5 shield models at +0x274 (row) / +0x275, set by FUN_0051B550 from the health: row =
  health / 5, minus 1 on a whole shield -> 0xFF with no health. The low-health pulse (FUN_0051A140 state 2,
  0x51A996: [itf + 0x2C + (row * 5 + col) * 4]) draws from it before taking the refresh flag (gm +0x5C 0x4000);
  at the game over's respawn (0 -> 15, the last hero's pulse still on) it read row 0xFF -> crash. LV2-1's game
  over did not crash (no pulse on). Fix (hud2.cpp KeepHealthIndex): row/col kept inside the table before the HUD
  tick (vf15 hook) and after each FUN_0051B550 (detour); XXL_NOHUDINDEXFIX: as before. Autotest over56 (-Level 5)
  with XXL_DOWNAT=0:5000 XXL_HPAT=1:6000:2 XXL_DOWNAT2=1:9000 (new: XXL_DOWNAT2 second hero down, XXL_HPAT
  <hero>:<ms>:<hp>): crash at the respawn with the fix off, none with it on.
- Round 82 (user, LV5-6 squad 4 at -116.16 9.84 33.90, Twister): (1) side by side the gate counter (FUN_0051F3C0)
  slid out to the right (state 3: +0x3F4 from +0x3DC to the picture's width + 1, width = picture node +0x1C -> +8
  -> +0x38) and in from there (state 1) across player 2's half: now it drops down / rises from the bottom
  (hud2.cpp LayoutGate; XXL_NOGATESLIDEFIX). (2) Its sparkle (particles +0x114, vt 622FCC) is placed once at the
  interface's start (frame modelling +0x40 = +0x3DC, 0.5; set off by +0x10 |= 0x40 when state 2 sees gm +0x5C
  bit 0 drop) -- particles leave from the frame, so moving the node for the pass did nothing: PlaceGateSparkle
  moves its modelling + LTM to the counter's middle (0.5, 0.494) while the side-by-side layout is on, back
  otherwise. (3) The fight never ended, counter at 0 (test with both heroes in the arena): the marker spawn hook
  (proximity.cpp, vf38 0x52FB80) returned without a call when every marker was in a player's view, but its caller
  had counted the Romans as sent in (pool record +0x16 went to 80/80 with none alive) -- so the lock waited for
  Romans that never came. Now the farthest marker from both heroes is used (XXL_NOMARKERDROPFIX: as before).
  Autotest gatedone (-Level 5; XXL_GATESHOTS: shots as the counter comes / goes, ends 3 s after): won in ~15 s.
- Round 83 (user: a fallen hero can lie where the other can't get to -- LV2-6's big pit, Asterix carried over by
  the CKHkTelepher; LV2-3's hearth room, its door shut by the hearth's #423): the level's pathfinding graph tells.
  CKSrvPathFinding = yellow pages +0x64 (vt 0x62483C): +0xC nodes, +0x10 count. CKPFGraphNode (vt 0x6247DC): +8
  box max, +0x14 box min, grid at +0x20 (+0x24 cells, 4 bits, even index in the high half; +0x28/+0x29 nx/nz;
  +0x2A mask 0xFF/0xF7), +0x2C transitions (+0x30 u16 count), +0x32 own index. Cell table 0x665240 by mover type
  (FUN_0045B830; heroes' AI 1, Romans 2): 7 wall, 0 (XXL-Editor cyan) Romans only, 4 (yellow) sidekick only, bit 3
  set over a box by objects (FUN_0045A880: crates, crumbly zones, rolling stones, push blocks). CKPFGraphTransition
  (vt 0x62480C): +4 0 walk over / else jump, +8 target node, +0xC flags (bit 31 on: event 0xA00 on, 0xA01 off,
  level start copies bit 30; bits 0-1 who may: 1 sidekick, 2 Romans), +0x10 doors 0x38 each (+4 take-off box
  max/min, +0x1C landing box, +0x34 u32), +0x14 count. Search: FUN_0045D890 (A* over nodes, FUN_0045E100 = the
  flags test), FUN_0045E1F0 (over one node's cells). LV2-6: bridge = node 39, its jumps to the cliff (39<->46)
  3FFFFFFD until the lever (86.2 2.4 -396.5) -> BFFFFFFD. LV2-3: the hearth room (node 94) <-> door node 58 walk
  over 3FFFFFFE (Romans only, off): the sidekick is never to go in; the door's cells are cyan. CKHkSwingDoor
  (2,98): +0x18 open 0..1, +0x28 steps, +0x2A target step, +0x2C/+0x30 leaves (frame +0x10 = cos of the angle);
  LV2-3 has 8 objects for 2 doors (copies on the same leaves, never moving) -- the leaves tell.
  pathfind.cpp: one BFS over (node, cell) for a hero on foot: cells not 7 and not bit 3, cyan shut within 3 units
  of a closed door's leaf; walk-overs unless the sidekick's and off; jumps the sidekick's and on (from within a
  cell of the take-off box's centre). A place is seeded in the node it is deepest in (the bridge node reaches a
  cell onto the cliff), on its cell or the walkable ring nearest (up to 3). individual.cpp BringToFallenLeader:
  fallen while leading (leader as the frame began) and the other can't walk to it -- asked every 0.5 s from 2 s
  after the fall, three noes -- the other is put 1.5 units beside it on a walkable cell. Not leading: stays (the
  user's rule: the level brings the leader to its sidekick, not the other way). XXL_NOREACHFIX: never.
  Autotests pf26 / pf23 (graph dumps, REACH verdicts: pit no -> lever -> yes; door open yes -> shut no; the 5-cell
  gap -49.6 45.2 -182.5 / -37.0 45.1 -182.6 yes), reach26 (XXL_DOWNAT=0:5000: brought; with XXL_NOREACHFIX
  stays), reach26o (Obelix leading: stays), reach26b (bridge out: yes, stays), reach23 (XXL_DOWNAT=0:8000:
  brought into the hearth room).
- Round 84 (user: after a revive the lead stayed with the hero who had stood): a hero that led when it fell
  (fellLeading, round 83) gets the lead back once revived -- by the grab, a launcher, XXL_UPAT -- through
  CoopGiveLead in the next IndivFrame (a launcher's revive runs inside a hero's tick, where the switch is refused),
  only if the other hero still leads. Cleared at a level change and at the respawn (the game's lead).
  XXL_NOLEADBACKFIX: as before. Autotests reach26 / reach26o with XXL_DOWNAT=0:5000 XXL_UPAT=9000: Asterix leads
  again (fix off: Obelix keeps it); not leading when he fell: Obelix keeps it.
- Round 85 (user's idea: the hero brought to its fallen leader may not get out of the leader's place -- Obelix's
  size): BringToFallenLeader keeps where the hero stood before its first move (broughtFrom) and where it was put;
  the leader's revive (any way) sends it back there next IndivFrame, unless it has gone on more than 10 units from
  where it was put (a sector change's revive, a launcher). Cleared at a level change and the respawn.
  XXL_NOSENDBACKFIX: it stays. Autotests (XXL_DOWNAT + XXL_UPAT): reach26 -- back to the cliff 114.5 2.6 -387.5
  (fix off: stays by Asterix); reach23 (XXL_DOWNAT=0:8000 XXL_UPAT=12000) -- out of the hearth room again.
- Round 86 (user, xxl_coop.lv2-3_tp_back.log: Obelix not sent back after reviving Asterix by the grab): the
  grab's revive suspends co-op for one frame (67.079 -> 67.094, "a scripted mode" as the hero gets up), and the
  send-back was taken and dropped in that frame (the lead's give-back waited it out). Now the send-back waits for
  co-op to resume too, and says why when the hero stays. Autotest reach23g (XXL_DOWNAT=0:8000): player 2 holds B
  by Asterix -- suspended / resumed, then back to -72.4 -1.6 -590.4. (The XXL_UPAT revive, as at a launcher, has
  no such frame: why reach23 passed.)
- Round 87 (user, xxl_coop.lv2-6_tp_back.log: after the revive Obelix went to the level's start, -854 508 -1073;
  two moves to get to Asterix): Obelix had been the waiting AI companion (trio 4802) -- NoteSafeSpot took spots only
  in situation 0, the AI's is 0xFF, so his newest was from the level's start. Asterix's fall undid the lock and
  IndivPutOnSafeGround put Obelix there (move 1); BringToFallenLeader brought him from there (move 2) and kept it as
  where he came from. Now an AI hero (+0x18 & 3 == 3) in situation 0xFF on solid ground takes spots too, and
  put-backs (SafeSpotAway) ignore spots more than 40 units away (none: the hero stays). XXL_NOFARSPOTFIX: as
  before. Autotest reach26w (XXL_DOWNAT=0:5000; trio 4802, Asterix down on the far side, P2 holds B): fix off ->
  -854.0 508.5 -1073.2 as in the user's log; on -> put back where he waited (117.8 3.1 -389.0), brought once, back to
  117.5 3.1 -387.2.
- Round 88 (user: (1) Obelix brought to Asterix fell into LV2-6's pit, xxl_coop.lv2-6_obelix_tps_to_death.log; (2) old:
  after a pause the fallen hero walked over to the other and fell again there, into a gap if one was on the way).
  (1) Asterix lay at the pit's edge (93.2 2.2 -391.1), inside the bridge's node (39), whose cells read walkable
  with the bridge in: PfSpotBeside chose 94.5 -- over the pit. Now SpotBesideFallen: one of the fallen hero's own
  safe spots / trail points 1-3 units from where it lies (nearest 1.5), else where it lies (the heroes ease
  apart). XXL_NOBESIDEFIX: the graph's cell. Autotest reach26p (Asterix stands at 93.2, walks into the pit): off ->
  Obelix at 94.5, falls, game over; on -> put at 93.2, eased to 92.9 -392.6, alive.
  (2) The pause menu's bars (gm +0x5C bit 20, world paused) suspend co-op, and while suspended every non-leading
  hero was made the AI companion (coop.cpp) -- a fallen one too: at the unpause, before the resume, it ran its AI
  life (situation 0xFF, walking: 20261014-190940 log at 538.4) toward the leader, then dropped into its death
  again where it got to. A fallen hero is no longer made the companion. XXL_NOFALLENAIFIX: as before. Autotest
  pausedown (XXL_DOWNAT=0:3000; pause state 2 + gm +0x5E ^ 0x10, back to state 1 -- the game clears the bars): off
  -> walks -854.9 -> -851.5; on -> stays at -854.90, co-op and the split screen resume.
  Also autotests relight23 / relight23n (LV2-3 hearth lit again after the send-back; the level's own wait): the
  "locked" Obelix of 20261020-190948 was LV5-5-like level behaviour per the user, closed.
- Round 89 (user: every fix so far confirmed on the pad): all 19 XXL_NO...FIX switches removed, each fix keeping only
  its own path (REACH, LEADBACK, SENDBACK, FARSPOT, BESIDE, FALLENAI, SHOPHUD, PUSH, FOV, NOFAIR, HUDLAYOUT,
  GATESLIDE, HUDINDEX, HUDVANISH, SQUADZONE, SQUADSHARE, ATONCEDOWN, MARKERDROP); PfSpotBeside (only the BESIDE
  switch's) gone. From now on a switch lives until the user confirms its fix (CLAUDE.md). Reproducing an old
  bug: build the fix commit's parent; the round entries above keep each "fix off" result. Checked after the
  cleanup: reach26 (XXL_DOWNAT=0:5000 XXL_UPAT=9000) and pausedown behave as with the fixes on.
- Round 90 (2026-10-20): button icons. The game's action icons are characters of its font "arial24-windows"
  (Rt2d bitmap font, CManager2d 13/16 vtable 0x623A28, fonts at +8 + id*4, ids 0-9; read from xxGLOC.KWN by
  Rt2dFontStreamRead FUN_005FC970, cdecl(stream), chunk 0x199): U+E000 action (CKInput action 5), E001 Dogmatix
  (10), E002 jump (4), E003 pause (12), E005-E008 up/down/right/left (0/1/3/2), E009 targeted attack (7), E00A grab
  (6), E00B launch Obelix (8), E00E confirm (20), E00F cancel (21); E004/E00C/E00D empty cells. English = 01GLOC.
  Loc strings embed them ("Use \xE000"; spies' texts in GLOC). Pages arial24-windows0/1 (256x256, 28 px cells);
  the images precede their texture chunks (string-named RW textures). Font in memory: +0x0C intergap, +0x10 flags
  (2 = wide), +0x14 4 page textures (RwTextureDestroy'd with the font), +0x24 128 shorts (ASCII -> glyph),
  +0x124 first wide char, +0x128 count, +0x12C shorts (RwMalloc'd), +0x130 glyph count, +0x134 RwSList of 0x20
  glyphs (+0 width/height, +4 texture, +8 u0 v0 u1 v1, +0x18 0, +0x1C page byte). The renderer (FUN_005FCF20)
  binds each glyph's own texture (state 1 = *tex, 9 = tex +0x50 filter), width FUN_005FDAB0 sums width+intergap.
  RW (cdecl): RwRasterCreate 0x591160(w,h,depth,flags 0x504), RwRasterLock 0x591200(r,level,1) -> pixels (stride
  r+0x18, A8R8G8B8), Unlock 0x590F60, RwTextureCreate 0x5889B0(raster) (+0x50 filter, +0x51 addressing, +0x54 refs),
  RwSListGetNewEntries 0x617AF0(list,n,hint), RwMalloc/Free = [0x66885C]+0x134/+0x138; PI texdict read 0x572A90.
  icons.cpp: the PNGs of Game Files\coop_icons (coop\icons, tools\make_icons.ps1) go on a 1024x512 page (64 px
  cells, GDI+), one RW texture in a free page slot of each font with U+E000, glyphs U+E100.. added (wide map grown
  to U+E1FF); keycaps (key.png / key_wide.png + the key's name) drawn on demand. Fonts loaded before the hooks
  (title screen) are found through CManager2d. With player 2 in, LocString (hints.cpp hook) rewrites action icons:
  a HUD prompt string (interface +0x1A4 ids) -> the buttons of the player whose hero has the prompt (coop.cpp
  PromptHook: g_prompt.hero; offered to both heroes -> both); any other string -> one icon when both players press
  the same, else "P1/P2"; runs of direction icons ("[Left] [Right]") -> one stick glyph (ls_leftright...).
  Player 1's buttons come from its binding (binding +0xC -> +0x14 into the pad, offset 6-9 left stick, 14+n button
  n (A B X Y LB RB Back Start L3 R3), 26-29 D-pad; or the keyboard byte -> vk); player 2's from the mod's layout /
  the ini keys. Both players on one keyboard is impossible (P2's keyboard slot only exists with P1 on a pad).
  Co-op hints use the icons too (HintHero / HintHeroName / HintPlayer). The game's prompt text is fetched every
  frame in its shown state (FUN_00520CC0 case 2 -> FUN_0042E590 -> FUN_00411780), so an owner change shows at once.
  Note: the gun (LV3-3) is only ever offered to the leader in reach, so its "Use" is the leader's player's.
  No on/off switch (user, Oct 20: fixes get none from now on; build the previous commit instead). XXL_ICONP2KB: player 2's icons as on the keyboard (tests).
  Autotests icons21 (LV2-1 spy pages, X; on pads: "press X"; ICONP2KB: "press X/F"; NOICONS: the game's icon),
  catapulta / catapultp (launcher "Activate": P1's X / P2's F), iconsgun -L3, mgun33o -L3 (hint icons).
  Found on the way, not fixed: a co-op hint replaced by another with the same key (gun solo -> shared gun hint)
  runs past the screen's right edge (plate +0x14 1.04), with or without icons.
  Tools: tools\kwn_textures.py (RW images of any .KWN), tools\export_game_icons.py (the font's icons),
  tools\icon_sheet.ps1 (contact sheet). The designer's export is F:\XXL COOP MOD\icons_export.
- Round 91 (2026-10-21, user's screenshots: the controls menu showed "X/F" etc., P2's shop showed nothing of
  his): whose buttons a text gets, in LocStringHook (hints.cpp) by what asks for it. Text objects take strings
  through FUN_0042E590 (thiscall(id), push esi/edi; its call returns to 0x42E5A6, so its caller's return address
  is 0x10 past the hook's) or FUN_0042E680 (re-fetches the object's id +0x14, called from shared 2D code:
  Shared_Class_0_0_sendEvent_x3, CColorTextButton2d vf16). Rules: a HUD prompt id -> its hero's player; while a
  hero shops (CoopShopper) everything else -> the shopper (the shop's "Back"/"Buy" buttons, ids 0x21 / 0x36C, are
  refreshed through FUN_0042E680 with the pause menu's ids, so only the time tells them apart; CKHkInterfaceShop
  vtable 0x628818 slot 15 = set-up, 18 = tick, but neither fetches them); CKSekens vf17-vf21 (0x46E940-0x46ECF0,
  dialogues) -> both; anything else (menus: only player 1 drives them) -> player 1. Known gap: player 1 pausing
  while player 2 shops would show player 2's buttons in the pause menu. XXL_ICONLOG=1 logs (string, caller,
  shopper, whose) once each.
  Hint overflow: the prompt box's text is placed only while it slides in (FUN_00520CC0 case 1 writes text +0x14
  from +0x434); shown (case 2) the game leaves it, so a hint that got wider afterwards (the revive bar's text, a
  hint replaced under the same key: gun solo -> shared gun) ran past the right edge (plate +0x14 1.04). Shown, the
  text +0x14 (and +0x42C / +0x434) now follows the measured width every tick. Autotests: mgun33o -L3 (plate inside),
  down2a (revive bar), shopsplit / shopsplita with XXL_ICONP2KB (shop: P2's G / Space; P1's B / A), icons21 (spy:
  still X/F), menus logged "for player 1". The revive bar's text has the hero icon too.
- Round 92 (2026-10-21, user: the merchant fixed; the pause menu while the other shops left to do): a text
  object keeps its own copy of its string (FUN_0042E590 copies it to +0x18) from its last refresh, and the pause
  menu's are not refreshed when it opens. The game refreshes all texts whenever a loc variable changes
  (CLocManager::vf12 -> FUN_0042A220, thiscall(CManager2d = yellow pages +0x34, 3, 0), ret 8; it did so when the
  merchant opened -- the in-game interface FUN_0051E47F -> FUN_0051C860 -> CLocManager::vf12 --, with whoever's
  buttons were due then, so the pause and controls texts got the shopper's). Whose buttons are due: the game
  state (gm +0xCC bits 0-1, 2 = the pause menu and its pages) first -- player 1's --, then a shopper, then player
  1. HintsFrame refreshes all texts the game's way when that changes (P2 in/out, pause menu, shopper, P2 on the
  keyboard). Autotest shoppause (XXL_ICONP2KB XXL_ICONLOG=1): P2 at the merchant's menu, game state 2 for 3 s, back:
  at the pause "Back"/"Confirm" (ids 0x21/0x20) fetched for player 1, after it the shop shows G Back / Space Buy.
- Round 93 (2026-10-21, user: freeze xxl_coop.crsah-10-20.log; then DSOAL cost 62 -> 48 fps). (1) The freeze is a
  deadlock inside IndirectSound 0.20 (dsound.dll -> XAudio2_7): the game thread creates a voice (sound start
  FUN_0043B730 -> FUN_005F0ED0 -> FUN_005F36A0: IDirectSound::DuplicateSoundBuffer, then FUN_005F3610 QI for
  IID_IDirectSound3DBuffer) and waits on XAudio2's critical section, which XAudio2's worker thread holds while it
  waits inside IndirectSound's voice callback (DSOUND+0xCED2). The xxl_coop.asi+0x258D6 frame in that stack is a
  stale return address (KlusterSetP2Camera's epilogue in build 6dddc2e, checked with a /MAP build). Co-op only
  starts more voices (P2's range checks), so it hits the race more often. Replaced by DSOAL r696 (Game Files;
  IndirectSound kept in IndirectSound_backup, use_dsoal.bat / use_indirectsound.bat). (2) DSOAL's fps drop: the
  frame limiter at 0x4016D2 is Sleep(15 - the frame's ms so far); since Windows 10 2004 a process's Sleep has
  1 ms steps only if it calls timeBeginPeriod itself (NtQueryTimerResolution shows the system-wide 1 ms set by
  other programs and is no evidence). IndirectSound's XAudio2 requests it, DSOAL doesn't, so sleeps round up to
  15.6 ms. Fix: a separate coop\timerfix\xxl_timerfix.asi (timeBeginPeriod(1) at load), kept out of the mod at
  the user's request. Diagnostic: env XXL_PROFILE samples the game thread every ~1 ms and logs where its time
  went every 10 s (module, innermost game return address). Its own 1 ms Sleep loop does not change the game's
  timer, but an earlier version called timeBeginPeriod and so hid the drop. Autotest soak: DSOAL 45 fps, DSOAL +
  timerfix 62, IndirectSound 61.5.
- Round 94 (2026-10-21, user: LV2-1 softlock, xxl_coop.pass_to_down.log). (1) A lifter waiting for a hero locked by
  the level: Asterix on the spinning platform's circle (event 0050: trio 4801/4802, pillar 0x901) while Obelix rides
  the lifter back down and stands on it. The lifter's own wait (+0x34, counts while Asterix is held -- the co-op
  zeroing in TickLife is not on that path) teleported him onto it after ~8 s, still locked, and the pillar stayed
  dark. Now WatchLifterHint (lifter state 0 + flag bit 0, one hero on +0x50) undoes the waited-for hero's lock
  after 2 s with the fallen-partner undo (UndoLock: pillar back, detectors / circle flags, rope cable car reset,
  trio 4803, hero on its last solid ground; the lead is not moved). While such a lifter waits, that hero can't
  board a rope cable car either (g.lifterWaitsFor: its situation 2 left after its tick, the hero-tick 4800/4801/4812
  held back, a car left in "rider on" +0x7C & 0xA0 == 0x20 reset). (2) Situation 2's handler is 0x49AB50: the
  table at hero +0x3E4 is indexed by +0x3E9 ([0] 0x49A630, [1] 0x49A670, [2] 0x49AB50). Round 63's check against
  0x49A670 never matched -- the cable car block with a partner down only ever worked through the event hold-back;
  it now leaves situation 2 as intended (cabled2: three approaches put back). Without that exit the car offered
  itself again (0x0C0E) in the frame of the put-back, took the hero back and stayed in 0x2571 for good. (3) LB
  pass to a fallen partner: no pass, the partner's "no" (bank +0x200); the pass hint isn't shown (and is hidden
  if the partner falls mid-spin). Autotests lifterlock, liftercar (LV2-1: Asterix in the cable car, Obelix on the
  lifter; then the car boards again), liftcab33 (-Level 3: releasing the rope already frees Asterix there), pass22
  with XXL_DOWNAT=1:3500; env XXL_LIFTLOG adds lifter / rope car fields to the 10 Hz log.
- Round 95 (2026-10-21, user: LV3-3 cable car left halfway, xxl_coop.lv3-3_lifter_car.log): not the lifter -- the
  cross detector that switches the leader (zone 0830, group 106: trio 4800 + 4803, detectors, CKHkLight 0x999/0x99B)
  freed Asterix while the rope car (CKHkTowedTelepher) had stopped short of its start. Let go, the car rolls back by
  itself (+0x30 speed goes negative; after a full ride too -- cablefull33: the rider gets off at the far end, the car
  rolls home), but a short pull's momentum only takes it part way (299 -> 301.8, start 307.6). Freed there it stays,
  +0x7C without bit 0x80 ("free"; 0x2431/0x2531), and never offers itself again. Now the car a lock boarded is
  watched (g_carWatch: car, rider, its start = where it stood at the lock); standing still (|+0x30| < 0.05) away from
  its start (> 1 unit), not free, rope not held (+0x7C bit 0x4000), nobody locked or held, for 1 s: sent back
  (0x512D + vf16, ResetCableCar), and its rider -- freed in the car's zone -- is kept off cable cars until it has
  walked 4 units away (same hold as round 94's lifter: KeptOffCars). Autotests cross33 (short pull, Obelix past the
  detector: sent back, Asterix not taken again, then a ride), liftcab33w (the same ending on the lifter),
  cablefull33; XXL_LIFTLOG's car fields now on a line of their own ("cars at").
- Round 96 (2026-10-21, user: xxl_coop.lv2-1_pillar_not_activatable.log, xxl_coop.lv2-1_car_ok_pillar_dark.log; and
  watching corkdown / lockagain: the pillar didn't fade the second time). (1) The spinning platform's circle
  (CKHkCorkscrew vf15) answers only the trio's leader, and its event 0050 hands the lead to Obelix (4801) before
  locking Asterix (4802). Every undo kept Obelix leading, so the circle stayed dead for Asterix -- after the lifter
  undo (lead kept on purpose) and after a fall (round 84's "led when it fell -> lead back after the revive" gave it
  back to Obelix). Now the lock's picture keeps the trio's leader (LockPicture.leader, taken with it, before the
  event's group runs); the lifter undo gives the lead back to it, 0.3 s later (GiveLeadLater: handed over in the
  put-back's frame the platform still saw Asterix on the circle and set off again), and a fall that undoes a
  sequence clears fellLeading when the fallen hero led only because of it (CoopUndoLockFor returns the leader
  before). (2) Obelix walking from the circle to the lifter crosses the turnstile's zone 03C8 (corkscrew 0x99C /
  0x99A, trio 4803): Asterix freed, the platform switched off until a turn that never comes -- the lifter undo only
  looked for a locked hero. A lifter waiting for the hero of a sequence still under way (g_seqHero,
  SequenceUnderWay) now undoes it like a fall does (UndoSequence: platform down, picture), puts the hero off the
  circle and gives the lead back. (3) A re-lock while the lifter still waits is undone 2 s after the lock
  (g_lock.since), not at once. Autotests lifterlock (now also walks back onto the circle), lifterlock2 (through
  03C8), corkdown / lockagain (the circle answers again after the revive).
- Round 97 (2026-10-21, user: xxl_coop.lv2-1_pillar_after_lifter_leave.log): Asterix on the circle, Obelix stepped off
  the lifter and crossed zone 03C8 (Asterix freed, the platform off until its turn reaches the top box), then walked
  away: the pillar stayed dark (no lifter waiting, so round 96 did not run). The user: the platform is turned by
  Obelix hitting CKHkTurnstile (2,45) with X from about -775.9 515.6 -983.6 (the turnstile 3.8 units from the circle,
  -775.6 515.5 -980.7) until Asterix crosses a box at the top (its frame +0x80 is not world: the turnstiles listed
  at 44.7 52.6 -67.8 etc.). Now WatchUnfinishedSequence: a sequence under way (g_seqLive, SequenceUnderWay) with the
  partner more than 25 units from where the locked hero stood (g_lock.at -> g_seqAt) for 3 s is set back
  (UndoSequence, lead back via GiveLeadLater) -- Asterix on the circle is locked again at once (user's choice:
  "unlock, then instantly locked by the re-activated pillar"). Round 96's lifter-only branch for this case is
  dropped (it moved Asterix off the circle). 3 s: walking through 03C8 to the turnstile is 1.5 s past 25 units.
  Platform +0x28 = its rise (0 -> 8 at the top). Autotests turnstile21 (the platform as meant: risen, top reached,
  the level's next zones 0528 / 0530 / 0058, nothing set back), lifterlock2, corkdown.
- Round 98 (2026-10-21, user: LB with the partner down confirmed; LV2-1 platform no longer sticks and re-activates;
  asked about the turnstile camera on P1's view, xxl_coop.lv2-1_turnstile_camera_gone.log). LV2-1 platform zones,
  from CKSrvDetection::sendEvent (0x14..0x18 enable / 0x1B..0x1F disable entry `low byte` of list +0x2C box-enter,
  +0x34 sphere-enter, +0x3C plane-cross, +0x44 box-level, +0x4C sphere-level): 0050 (circle) arms box #03 and
  cross #1B (= 03C8); 03C8 disarms itself, arms box #13 (= 0528, Obelix at the turnstile: 482D / 4827, then the
  ride camera, track vt 6244EC) and sends CKCameraSector (vt 624728) 0x700 (= +0x29 bit 1 off; 0x701 on, 0x702 /
  0x703 bit 3 -- not a toggle); 0528 disarms itself and re-arms 03C8. Pre-fix build 31a3734 run on turnstile21
  (old src + today's autotest.cpp in a worktree): the same zones and camera switches at the same times as now --
  during the ride P1's Asterix view (the copy) shows a close shot behind a post, P2's the wide shot. The user
  remembers the single-player camera on P1's view while P2 hit the turnstile but chose to leave it for now.
  Autotest turnstile21b (reset, then back to the turnstile); XXL_LIFTLOG adds box13 / cross1B and the corkscrews.
- Round 99 (2026-10-21, user: LV3-3 car comes back, but by a jump; after a full ride it drives home smoothly). With
  the rope let go a rope car travels as an automatic one (CKHkTowedTelepher::vf29 0x4BD020 -> CKHkTelepher::vf29
  0x548880 while +0x7C bit 0x4000 is clear): +0x7C bit 0x2 = on its motor (target speed from +0x94), bit 0x4 =
  constant (no slope term), bit 0x8 = backwards; +0x14 position along the path, +0x30 speed, +0x28 target, +0x2C
  acceleration. A finished ride leaves 0x241B (motor, backwards); a short pull's stop 0x2431 (no motor, bit 0x20).
  WatchStrandedCar now sets bits 0x2 | 0x8 and clears 0x20 instead of the reset: the car drives home (cross33:
  301.8 -> 307.6 in 1.6 s, then 0x2591, free) and rides again. Fallback: not home within 15 s, or stopped again on
  the way, or home but not free -> ResetCableCar.
- Round 100 (2026-10-21, user: xxl_coop.menu_unlock_car.log -- after a long stay in the pause menu Asterix came out
  of the cable car on his own and the car could not be used again). The mod's own safety valve (session 1): a
  borrowed hero whose scripted order runs 2 min is handed back to its player (g.held cleared, nothing else) -- it
  counted the pause menu (suspended 70.7 -> 215.6 s, released at 159.9 s) and left the car holding Asterix
  (+0x7C 0x2571, never offers itself again). The user did not ask for it. Now it applies to walk orders only
  (IsWalkOrder: 1 go to, 2 jump to, 7 ballista, 11 clear path, 13 hint man, 14 shop, 15 go to without PF -- a
  marker that can't be reached, LV5-5), never to the orders that wait for the partner (3 wait, 8 beam, 9 cable
  car, 12 external control: the partner can finish them; falls / lifters / stranded cars have their undos), and
  time suspended (pause menu, cutscenes) is not counted. Env XXL_HELDMAX=ms (test aid, default 120000). Autotest
  carpause (XXL_HELDMAX=15000: 20 s paused + play past the limit, Asterix stays in the car).
- Round 101 (2026-10-21, user: drop the 2 min release altogether; R3 + B should unlock like the lifter / revival
  undo -- a first step to a "freedom" feature where players are never locked and nothing softlocks). (1) The held-
  hero timeout is gone (no XXL_HELDMAX, no heldSince; carpause: 20 s paused + play, Asterix stays in the car). (2)
  R3 + B (UnlockByPlayer) on a hero locked by the level (g_lock: the light pillar's circle, a rope cable car) ->
  UndoLock (picture back, car reset, 4803, the lead back to who had it before, the hero on its last solid ground --
  off the circle, or it would be locked again at once); on the hero of a spinning-platform sequence under way
  (03C8 crossed) -> UndoSequence + put back + lead back; any other borrowed hero -> the old plain take-back. (3)
  Cable cars keep re-taking a hero put back in their zone: an undo / reset car offers itself (0x0C0E) in the same
  frame and the hero's next tick enters situation 2 and snaps into the seat (the seat is the car's CDynamicGround,
  never a safe spot -- user's XXL-Editor screenshot Snipaste_2026-10-21_05-52-29; the put-back spots are on the
  snow 5.5 units off, still within the car's offer). KeepOffCars(hero, car): the hero's situation 2 left after its
  tick and its boarding events held back until the car is free (+0x7C bit 0x80) for 1 s, or the hero has walked 4
  units from where it settled (its place over the first 0.5 s; time in situation 2 not counted). A rider freed
  while still standing on the stranded car (LV3-3 cross detector 0830) is put on its last solid ground when the car
  is sent home, or the car carried it back and took it again. Env XXL_P2HERO=asterix (autotests: player 2's virtual
  pad drives Asterix). Autotests unlockpillar / unlockcar / unlockseq (XXL_P2HERO=asterix), cross33 (Asterix now
  walks to the car first), carpause; regressions liftercar, lifterlock2, swingundo, cabled33 pass.
- Round 102 (2026-10-21, user: xxl_coop.lv2-1_r3b_car_camera.log -- R3 + B in LV2-1's cable car, P1's camera did
  not come back to Asterix). The camera service follows one target (+0xE8, a hero's +0x10). Boarding the car (while
  Asterix still leads) aims the game's camera at the car's node; the leader switch that follows hands that state to
  Asterix's copy (gamecam HandOver), so the copy kept following the car after any undo (also the lifter undo, and
  after a pillar unlock it followed another object). Now UndoLock and the stranded car's put-back call
  GameCamAimAtHero(hero): the copy gets a cut aim at its hero on its next tick (g_copy.cut), or the game's camera
  gets SetTarget(hero, 0, cut). LV3-3's car does not retarget (unlockcar33: the target stayed Asterix). Autotest
  steps S_UNLOCKHERO (what R3 + B does, for player 1's hero -- injected P1 input never reaches the rope / R3 combo)
  and S_CARPOS (the car's +0x14 along the rope; only tilts the car, no real move); XXL_LIFTLOG logs the cameras'
  targets. Autotests unlockcar21 (LV2-1: before, P1's view stayed on the rope; now on Asterix), unlockcar33.
- Round 103 (2026-10-21, user: xxl_coop.obelix_unlock_identity.log, Snipaste_2026-10-21_06-32-37 -- after the
  spinning platform, Backspace + G on Obelix walking to his marker made Obelix the leader again; Asterix's torch
  then showed player 2's "Grab F" and could not be taken). LV2-1's platform top zone 0058 (group 00B: trio 4800
  Asterix leads, 4828 Obelix walks to a marker = AI order 1, camera sector 0x700, detectors) is the level going on,
  but R3 + B ran the full undo on it: its picture (taken before 0058, leader Obelix) gave the lead back to Obelix.
  LockedByLevel now requires an order that waits for the partner (3 wait, 8 beam, 9 cable car); a scripted walk is
  only taken back (g.held cleared, the lead untouched) -- also not undone by a lifter or a fall. turnstile21 now
  ends with S_UNLOCKHERO on Obelix after 0058 (taken back, lead stays Asterix); liftercar, unlockcar21,
  unlockpillar, lockagain still undo.
- Round 104 (2026-10-21, user: Obelix leading on entering LV2's sector 2, where Asterix leads in single player;
  xxl_coop.sector2_obelix_leads*.log). The mod's own round-97 rule: the platform top zone 0058 walks Obelix to a
  marker (4828, order 1) and that hold was noted as a lock; when it ended (the level's 4801 + 4803 + 4878 as
  Obelix reached the marker), EndLock kept its picture as a "sequence under way" (the platform's flags differed
  from it), and WatchUnfinishedSequence, with Asterix 32 units from where Obelix's walk began, "set it back" -- 7
  detectors to their state before 0058 (level progress undone) and the lead to the picture's leader Obelix (mod's
  4801 0.3 s later; 73.8 s and 124.2 s in the two logs). Now the lock remembers whether it was a "wait here"
  (0x4802: g_lock.wait -> g_seqWait) and SequenceUnderWay is true only for those -- the platform rules (3 s away,
  R3 + B, a fall) never act on a walk's or a ride's picture. Autotest turnstile21c (platform done, Obelix's walk,
  Asterix walks off: nothing set back, Asterix leads); lifterlock2, unlockseq, corkdown still set back / undo.
- Round 105 (2026-10-21, user: "freedom mode"). [coop] freedom (default off; env XXL_FREEDOM): Hold() leaves a
  player's hero its player's for any scripted order but the cable cars' (9 rope, 12 external control -- the automatic
  CKHkTelepher); the mod's own holds (JoinGun) pass modOwn. LockedByLevel then only sees held heroes (undos only for
  cable cars). The trio's 4802 freezes nothing by itself: a player-variant hero with order 3 walks and jumps freely.
  LV2-1 spinning platform: rises with a free Asterix on it (turnstile21 with freedom); its "jump off at the top"
  orders (482D order 1, 484C order 2) are left to the player. LV4-2 rotary beam (CKHkRotaryBeam 2,57, vt 0x628FC0;
  +8 flags: 0x4 over the circle, 0x1 a hero hanging, 0x2 going back; it sends trio 0x0C02 -> companion order 8 "hang
  on beam" when over the hero standing on the circle): a player's jump never catches it, only the AI's order 8 does.
  WatchFreedomBeam: the player's jump (body 0x0E) with order 8 pending and the beam over him hands the hero to the
  game for that ride (Hold modOwn) -- caught (0x821), carried (0x825), let go at the far end, back to its player
  (beam42f: XXL_P2HERO=asterix, player 1's Obelix turns the turnstile with X). [experimental] freedom_romans (env
  XXL_FREEDOMROMANS; freedom.cpp): while CoopFreedomWaiting(hero) (a scripted order freedom left unfollowed, not
  leading, not held), every 6 s up to 6 Romans: the nearest active squad (group 4,26, +0x152 bit 1, not 0x2000) with
  a pool that has Romans left (a spent pool gets +1 to defeat, +0x18) sends one in with vf55 (thiscall(args {count,
  Vector3*, pool}), the script's "send in at a point"); the point: a squad spawn marker within 30 units, walkable to
  (PfReachable), 6+ from both heroes, in no view (KlusterSphereInAnyView) -- else PfSpotNear (new: a random reachable
  cell of the hero's own cell kind 3-10 units off) with ground at the hero's height under it (FUN_004493C0 on the
  collision world [[0x6621F4]+0x50], thiscall(params {0.3, 2, 0}, out, point, 1) ret 0x10: out +0x14 kind, 4 none;
  +0x18 the point) -- the graph's walkable cells take in shallow water (Romans there drowned). Stops when the wait is
  lifted. LV2-3 (freeromans23): the hearth (#423) gives Obelix 4826 (order 1); Romans come; the powder keg (#327) sends
  4803 -> no more. Autotest steps S_NEAROBJ for the keg (CKHkPowderKeg 2,77; #20 at -51.2 0.8 -521.4); test steps must
  be in time order (steps run in list order).
- Round 106 (2026-10-21, user: the beam's catch could come sooner; a carried torch burned on while Obelix led, unlike
  single player). (1) CKHkRotaryBeam::vf15 (0x53F170) state 4 (over the circle) waits out +0x20, then until it has
  stopped swinging (|+0x2C| < 0.2, |+0x28 - +0x3C| < 0.02), then sends 0x0C02 and goes to state 2 (bits 0/2: &~5 | 2).
  WatchFreedomBeam: a player's jump under the beam in state 4 sets +0x20 = 0; a jump up to 1 s before the order 8
  still hands the hero over (no "over" bit needed -- the pending order means the beam waits). beam42f: caught on the
  first pass. (2) CKHkTorch::vf15 (0x4AF790): carried (+0x3E & 3 == 1) and lit (bits 2-4), its timer +0x1C runs only
  while trio vf40 (0xA0: the leader is Asterix); the HUD reads the real leader. Objects are ticked as their nearest
  player's hero posing as the leader (proximity TickLife), so Asterix's torch burned while Obelix led. A carried
  torch now ticks with the real leader (as the corkscrew / plates). beam42f: timer 2.73 held while Obelix led.
  XXL_LIFTLOG logs carried torches.
- Round 107 (2026-10-21, user: freedom mode fine in LV4-2 and LV5-5; freedom_romans should rely on the level's
  placed markers, not pathfinding cells). The fallback (a random walkable graph cell + a ground drop with
  FUN_004493C0) is removed (PfSpotNear gone): a Roman comes only at a squad spawn marker within 40 units, 6+ from
  both heroes, out of every view, walkable to (PfReachable -- the reachability check stays, so none behind a shut
  door). LV2-3 by the hearth has no squad marker within 40 units at all: no Romans there (logged once: markers
  within 40 -- by a hero / in view / not walkable to).
- Round 108 (2026-10-21, user: LV2-3 has no squad markers by the hearth; use Obelix's marker 147 (where the level
  walks him after the doors shut), or squads 36 / 46 nearby?). Dump once a wait (DumpAround): Obelix's walk order (4826,
  order 1) targets (-54.6 0.3 -523.1) = the user's marker 147, 12 units off. Squads within 60: #25 inactive (4 placed),
  #36 active, box 82x97 holding Obelix, no markers, pool 2/2 sent (the Romans' source already, +1 to defeat each), #46
  inactive (10 placed of 20), no markers -- none of them has spawn markers. Now the second choice after a squad
  marker is the hero's walk-to marker (AI target hero +0x568: level-placed, on the hero's side by design), only when
  6+ from both heroes and out of every view (else the next try). freeromans23: four Romans from it, none drowned,
  stopped at the keg's 4803. Waking squad 46 for Romans was left alone (it would run its own script).
- Round 109 (user, LV3-6, xxl_coop.lv36-telepher.log): after the light pillar (-175.3 3.8 -219.2; its circle's 4801 gives
  Obelix the lead) Asterix rode the automatic cable car (CKHkTelepher 2,158, -159.6 11.8 -209.3) and stayed on it at the
  end, no helmets collected on the way. CKHkTelepher::vf15 (0x5485F0) takes only the trio leader (its +0x84 hero ==
  trio vf 0x74) and tells the trio 0x0C0D (from 0x548757); proximity ticks it with the nearest player's hero posing as
  leader, so the non-leader boarded. The rest of the ride follows the real leader: 0x481C walks the companion to the
  end (AI order 1), the end's 0x0C09 (0x549BA5) is the leader's jump off but the companion's AI order 2 (freedom: not
  followed -> stuck in situation 2), the helmets are the leader's. Fix: 0x0C0D from 0x548757 notes the leader its tick
  saw; next frame that rider gets the lead (WatchTelepherRider, 4800/4801), before the 481C 0.3 s later. Autotest
  pillar36 (-Level 3, XXL_FREEDOM=1; the pillar needs the fight won, so 4801 is sent directly): before, Asterix in
  situation 2 with order 2 to the end; after, he jumps off to (-159.7 0.8 -133.7) and picks up helmets on the way.
- Round 110 (user, LV2-10 boss, xxl_coop.boss210-telepher.log): in freedom mode Asterix, jumped onto the boss's
  button by the automatic cable car's end (situation 0x0F), was not locked by the boss's "Obelix leads, Asterix
  waits" (trio 4801 / 4802 sent from the boss's code at 0x4F3165 / 0x4F317E; LV3-6's boss runs the same code), and
  could walk off it. Its other wait (0x4F2E27 48B9 + 0x4F2E40 4802: the companion Obelix) stays free. Also, by the
  user's wish, new [coop] freedom_corkscrews (default on, env XXL_FREEDOMCORK): a wait for a hero on a corkscrew's
  circle (CKHkCorkscrew 2,44 node within 4 units horizontally / 4 vertically: LV3-6's light pillar 0.1, LV2-1's
  spinning platform 0.3) is held as with freedom off -- the pillar's launch (corkscrew vf15 0x0C09, AI order 2)
  follows while the hero is still held, aimed where the level wants it. Both via g_freedomLock (NoteFreedomLock at
  the 0x4802 in CoopNoteTrioEvent; Hold lets it through within 0.5 s). LV2-1's lock still ends at the game's own
  4803 (zone 03C8); the jump off the platform's top (484C, AI order 2) stays the player's. Autotests boss210t
  (-Level 2, XXL_FREEDOM=1: Asterix held on the boss after the car; skips the intro with CINE_SKIP), pillar36l
  (-Level 3: lock kept / with XXL_FREEDOMCORK=0 free; R3 + B frees), turnstile21 with XXL_FREEDOM=1.
- Round 111 (xxl_coop.pad_preset_issue.log, wrong pad layout again): another slow boot (menu at 5.8 s). The boot's saved
  options (vf10(1) -> FUN_00503B00) set preset 1 at 0x503B41 as a step while building the tables, and force_level's
  level started before the title's choice (vf10(6) -> FUN_00503AA0 -> 3 at 0x503AED, normally ~1 s later, e.g.
  bump2 / 20261015-031152 logs) -- the level ran on 1, "chosen with a pad", which the mod left alone. Now 1 from
  0x503B41 counts as no choice: a running level on it gets the pad layout file put back and preset 3. FUN_00503B00
  outside the title also rebuilds preset 3 as the default table (A0+0F/10/11/12, face buttons shifted), so the file
  is always restored. Autotest presetopts (S_SETPRESET a=-1 runs FUN_00503B00 in play): before, stayed on 1; now 3 with
  all 48 bindings as the file (no table change logged against the title's); preset1 (XXL_PRESETNOPAD=1) still OK.
- Round 112 (user, two P2 camera reports, game camera copy = gamecam.cpp):
  (a) Modded levels ("Caesar's Challenge", SPQR\Caesars Challenge, same exe build; log userlogs\xxl_coop.bad_camera_p2.log),
  LV1-4: Obelix walking 1000.2 200.1 -21.7 -> 1002.6 200.1 -17.6, P2's camera stuttered in and out close to him. The copy
  had switched to camera sector 04385A04's track 03F32148 and from the next frame on CopyStale said "the level was loaded
  again" 295 times: it only knew the tracks in the service's list (+0x20), and that sector's track is not in it (made with
  the XXL Editor, it seems). The game takes tracks from the camera sectors (+0x24, track +0x20; tick ~0x456xxx) and its
  default one (+0x10) as well. Now TrackKnown accepts all three, SnapshotTracks also snapshots the sector tracks, and the
  "loaded again" line is capped. Autotest cam14 (-Level 1 -GameDir "SPQR\Caesars Challenge"; run_test.ps1 has -GameDir now,
  the exe there is "Cesar's Challenge - Windowed.exe"; S_CAMDUMP lists the +0x20 tracks too) -- not run yet by me.
  (b) Vanilla LV3-6 boss: the rolling machine's camera shake showed for P1 only. A shake request (boss FUN_004F05C0, ~30
  other writers: stomps, explosions...) sets svc +0xC bit 0x1000, +0x668 length s, +0x66C elapsed, +0x670 strength,
  +0x674 floor, +0x678 frequency, +0x67C fade, and +0x680 = 0 (the tick's shake counter; the tick ~0x45A0xx counts it
  up, clears the bit when elapsed >= length). Requests outside a tick reached only the game's state. Now ShareShake (start
  of TickHook): bit on with count 0 = a fresh request -> bit and +0x668..+0x683 into the copy. The boss sends 0.01 s /
  0.08 every frame while it rolls. Autotest boss36s (-Level 3): both drop on #46 and glide to x -248; the log shows the
  shake handed to Obelix's camera each frame.
- Round 113 (user pad test of 112: modded LV1-4 camera confirmed fixed): the Twister shakes the camera too -- P1's
  Twister shook P2's screen as well, and longer than P1's; P2's Twister shook P2's screen only. The Twister asks for
  a 24 s shake (strength 0.05) from the hero's own Life tick and stops it when the spin ends (~12 s; bit cleared,
  elapsed 12.03). The leader's tick runs on the game's state, so round 112's ShareShake passed the start to the copy
  but not the stop; the non-leader's tick runs with its copy swapped in (GameCamBeginHeroTick), so its shake was only
  its own. Now a shake asked for during the leading hero's own tick (GameCamBegin/EndHeroTick compare +0xC bit
  0x1000 and +0x668..+0x683) is that player's (g_shakeOwn): ShareShake skips it. World shakes (the boss) still reach
  both. Env XXL_SHAKELOG=1 traces both states' shake. Autotests twistend (P2) / twistenda (P1): each shakes only its
  own camera and stops at the Twister's end; boss36s still shared.
- Round 114 (user: LV5-10, Obelix down, Asterix put back "the frame before" at the cross rectangle 525.7 -451.4
  -197.2 by the keg, #97 = zone event 0308: trio 4800 "switch to Asterix" + 4803 "enable sidekick movement"): not
  the dialogue case -- round 64's "zone that needs both heroes" (GroupNeedsBoth), which counted every group
  commanding the trio, plain leader switches included. Now a group whose only trio orders are switches to the
  standing hero (4800/48B7 Asterix, 4801/48B8 Obelix) and/or 4803, with no cutscene (GroupOnlyLeadsTo), is let
  through while a hero is down; the level's switch is kept for the revive (IndivNoteLevelLead clears / sets
  fellLeading of the fallen). A switch to the fallen hero, wait orders, marker walks, rides, the slide stay held
  with the put-back. Roman agents' talks (spy tick within +0x70, zone -> CKHkClueMan 0x5198, trio 0x511F/0x5120)
  are still held while a hero is down, but without the put-back (IndivBlockTalk replaces
  IndivBlockPartnerSpotFrameStart): the hero walks on; after the revive the talk starts as in single player (at
  once if the hero still stands within the agent's reach). Autotests: lead510 (-Level 5, XXL_DOWNAT=1:2000;
  Asterix through 0308 twice, let through), spyd2 (talk held, Asterix stays by the agent), spyrev (talk after
  the revive), slidecross3 / cabled2 / lockcircle / corkdown / beamundo / reach23g / reach26w as before
  (slidecross3 checked against the pre-change build: identical).
- Round 114b: IndivNoteLevelLead had cleared fellLeading, which BringToFallenLeader also reads -- a fallen leader
  out of reach would no longer have had the other hero brought to it once a lead-only zone fired. Now its own flag
  (levelLead: no lead back after the revive); fellLeading untouched. Autotest lead510o (-Level 5, XXL_DOWNAT=1:2000
  XXL_UPAT=12000): Obelix leads and falls, 0308 let through, the reach check still says "leading when it fell",
  revived, Asterix keeps the lead. reach23g / reach26w (brought to the leader, sent back, lead back) as before.
- Round 115 (user, LV2-1, userlogs\xxl_coop.lv21-lead-20261021.log: Asterix the leader before the iron crates,
  where Obelix should lead): not the revive code. Obelix stood east of the circle (-747 -978) when Asterix was
  locked again on it (137.8 s, after a fall's undo), walked straight to the turnstile, so zone 03C8 (its 4803 ends
  the circle lock as "sequence goes on") never fired; freedom_corkscrews launched Asterix to the top (0058, 146.1).
  The lock stayed open; the keg's group (Asterix blows it up, -782.8 518.0 -992.7: 4801 + 4803 + 4878, Obelix
  leads to the iron crates) ended it with EndLock(true) -> g_seqLive with the picture from before 0050; the
  platform's +8 differed from it, Obelix walked 31 units off -> WatchUnfinishedSequence "set back" (pillar,
  detectors) and gave the lead to the picture's leader, Asterix. Now a 4803 continues the sequence only while the
  locked hero still waits (AI order 3); else the lock just ends (log "lock ended after its sequence", with the
  platform's +8 now / at the lock). Autotests turnstile21d / turnstile21e (XXL_DOWNAT=1:6000 XXL_UPAT=9000: the
  user's history, Obelix not through 03C8, the ride to the top, Asterix down to the keg, S_TRIOEV 4801 + 4803,
  Obelix off): the lock ends at the keg event (order 0). The final "set back" itself did not reproduce (the
  platform's +8 equal to the picture, 0/0, in the test). turnstile21b's intended set back (Obelix off through 03C8
  before turning) still happens.
  Seen meanwhile (older, since freedom mode a43ba13): with Obelix through 03C8, its 4803 frees Asterix from the
  corkscrew lock 1 s after it starts (order done), and the walk onto the platform (482D) is not followed in
  freedom -- turnstile21 / 21c no longer reach 0058 (the autotest's P1 does not walk). Open.
- Round 116 (user: keep the freedom corkscrew lock until the launch). LV2-1's platform in single player: Asterix
  lands on the corkscrew's circle (its "on landing" event #10 = 0050: 4801, 4802, pillar 0901, detectors 1403 /
  161B -- among them zone 03C8 = #121); Obelix's way to the turnstile crosses 03C8 (4803 frees Asterix, corkscrew
  99C/99A); the turnstile's hits (#12: corkscrew 0996 "move up slightly", camera sector 0701) raise it; zone 0528
  walks Asterix onto it (482D), at the top zone 0530 jumps him off (484C), the top's box 0058 gives him the lead.
  In freedom mode (since a43ba13) 03C8's 4803 freed Asterix (order 0, "order done") 1 s after the corkscrew lock
  (round 110), and 482D / 484C were "the level taking a player's hero" -- not followed: he rode up and stayed
  (turnstile21 / 21c never reached 0058). Now the hero a corkscrew's circle locks in freedom (g_corkRide, set in
  NoteFreedomLock, cleared when it is let go -- leads again / order done --, on an undo, at a level change) keeps
  it: a game 4803 (caller in the exe) while it is held, order 3, within 4 units of the corkscrew's node is held
  back (CoopDeferTrioEvent), the lock's bookkeeping run as for any 4803 (EndLock(true): the sequence goes on).
  With the lock already ended, a fall's undo (UndoLock / CoopUndoLockFor) and R3 + B (UnlockByPlayer) now undo
  the sequence's picture (g_seqPic) and report its leader (corkdown had lost the circle's re-arm after the revive:
  Obelix got the lead back, the circle answers only the leader). Autotests: turnstile21 / 21c reach 0058 with the
  4803 held back; turnstile21b still set back and Asterix locked again; corkdown restored, no lead back, Asterix
  locked again after the revive; turnstile21e as round 115; pillar36l (LV3-6, R3 + B) as before.
- Round 117 (2026-10-21, user's designer: dialogue text and icons ~30% too wide at 16:9). CText2d widths are
  aspect (2D manager = yellow pages +0x34, +4: width / height, 1.778 at 16:9) times the float 0.75 at 0x623C64,
  read only by the text layouts 0x42DF40 / 0x42E840 / 0x42EF80 and the glyph draw 0x42F430 (Rt2dCTMScale(aspect *
  0.75, 1) per glyph; positions are x * aspect): 1 at 4:3, 1.33 at 16:9. Separate coop\widescreen2d\
  xxl_widescreen2d.asi keeps 0x623C64 at 1 / aspect (a thread, every 10 ms; .rdata made writable): true letter
  shapes, lines hold more words. hints.cpp reads that factor (GlyphWidthScale) instead of assuming 0.75, so the
  co-op hints place right with or without it. CBillboard2d has its own switch (+0x1C: width in screen shares,
  i.e. stretched, vs square units). Not covered: the HUD's 3D models (shields, helmet; HUD camera renderer +0x14,
  view window 0.5 x 0.5 over the whole screen) and stretched billboards. Autotests spy21a / hintjoin / shop
  before and after: subtitles and hints narrower, centred / right-aligned, round icons round.
- Round 118 (2026-10-21, user: the HUD shields and helmet are still wide; then: the shields far apart). The HUD's
  3D models come from the three HUD lists (view +0x4C/+0x50/+0x54 = statics 0x662DDC / 0x662BC0 / 0x662E00;
  vtables 0x622B24 / 0x622B94 / 0x622BB0, slot 3 draw-and-empty 0x412C00 / 0x4130D0 / 0x4131D0, thiscall, no
  args; list +4 first entry; entry +8 RpAtomic (header 0x00050001), its frame atomic +4, next entry +0x24; drawn
  by the atomic's render callback +0x48 from the frame's LTM +0x50 -- entry +4 is not a frame). The HUD camera
  (0x662EBC, ortho 0.5 x 0.5) spans the viewport, so models are a third too wide at 16:9. xxl_widescreen2d.asi
  wraps those slots: squeeze = window x / y * viewport h / w * 4/3 (D3D GetViewport, vtable 48: the co-op split
  sets the viewport on the device, not the raster), each atomic's LTM axes squeezed along the camera's right axis
  about its own position, put back after the draw. Rows (same line within 0.03, neighbours within 0.15) also get
  their spacing squeezed towards the row's outer end; single models keep their centres (they sit in 2D plates).
  The co-op mod's P2 HUD pass calls the same slots, so both HUDs get it. Autotest hudlayout -Level 4 before /
  after: shields and combo columns narrow and in place, rows closed up.
- Round 119 (2026-10-21, user: the see-through HUD plates -- Romans to beat, helmets, laurels -- still wide; then: the
  co-op hints need a fix). CBillboard2d draw 0x42D6F0 (thiscall(bb, float pos[2], ?, char alignX, char alignY, ?),
  ret 0x14; inline at element +0x20, called by 0x427060; width +0x14, height +0x18, +0x1C set = width as a share
  of the screen, i.e. stretched; alignX 0 left / 1 middle / 2 right edge at pos). xxl_widescreen2d.asi detours it
  and the CText2d draw 0x42F430 (same arguments): a stretched billboard under 0.95 of the screen is drawn
  (4/3) / aspect as wide (the game's sums keep its anchor) and its box kept per frame (frame id 0x6464C4, this
  frame's and the last's); text, small billboards and HUD models (camera space -> D3D viewport -> back buffer
  share) on a plate are drawn that much nearer its anchor, and are no row. The co-op hint's plate (interface
  +0x140, width +0x34) is sized for the true-shape text, so hints.cpp PlateDrawScale gives it 1 / that factor
  when the asi is in (seen by 0x623C64 != 0.75). The hint's text is not on its plate's box (laid out in a 2.0
  box, words moved), so it is not moved. Autotests hudreq -Level 4 (plates narrower to the right, 0/0 and 3 inside,
  icons at the ends), hintjoin and down2a (plates fit the hints), spy21a (bars and subtitle untouched).
- Round 120 (2026-10-21, user: with xxl_widescreen2d.asi the side-by-side HUD's 3/4 size-downs are no longer
  needed). hud2.cpp MiddleScale(): 1 while xxl_widescreen2d.asi is loaded (GetModuleHandleA), else 0.75 as before:
  the helmet counter group in the top middle (counter, x3 / x10 sign, spy's helmet requirement), the fight gate's
  counter and its sparkle's place. ScaleHealth (more than four shields) is skipped under it: six shields at full
  size close up to 0.06 apart (the asi's rows) and end by 0.39 of the screen, short of the middle pieces (from
  0.43). The group stays 0.1 down, under the shield rows: at the top, player 2's six-shield row (to 0.61) would
  meet its right end (0.60). Autotests shields6, hudreq / hudx3 -Level 4.
- Round 121 (2026-10-21, user's pad test of 120: the 3/4 middle pieces and the six-shield shrink look less cluttered
  -- keep them). Round 120 undone: hud2.cpp as before it, the same layout with or without xxl_widescreen2d.asi.
  Autotests shields6 (shields shrunk again), hudreq -Level 4 (middle pieces at 3/4).
- Round 122 (2026-10-21, user: upscale the text font from their Figma "English table" -- Arial Bold 24 px nodes
  over the 256 page -- for every language). Packs 00-04GLOC each have their own page 0 (same layout, different
  extra characters: 00 French oe/a-grave..., 01 English en dash/ellipsis, 02 German umlauts/sharp s, 03 Spanish
  inverted !/?, n-tilde..., 04 Italian E/I-grave...); page 1 is icons only. Glyph UVs sit on texel centres (cell
  + 0.5); the user's nodes are cell - 0.5 (ink centres within 0.8 px of the old glyphs). Figma's text placement
  = GDI+ GraphicsPath of Arial Bold with baseline y + 0.9167 * size (outline bounds equal to 0.03 px), so the
  pages are rendered locally (coop\widescreen2d\fonts\tools\render.cs) from the node positions; the Figma MCP
  has no Arial (server side), the other tables there use Arimo Bold (Arial's metrics). Other packs: shared
  characters keep the English node's offset from its cell; new ones the baseline and the x leaving the least old
  ink bare (render.exe check). English X / Y moved 1 px right (0.5 px out of their cells); quotes straight (the
  original's shape; the user's curly ones read "It's" with an opening quote), ' 1 px right. Output 1024x1024 per
  pack (Game Files\xxl_widescreen2d\arial24-windows0_LL.png): icon cells upscaled from the original, text cells,
  green borders and their corner marks cleared (the old text showed faint cell lines between letters).
  xxl_widescreen2d.asi swaps it in from its CText2d draw hook (render thread, once a frame): CManager2d (yellow
  pages +0x34, vtable 0x623A28) fonts +8 + id * 4; the pack by a character only it has (+0x124 / +0x128 / +0x12C
  wide table) and its "A" glyph's u0 (+0x24 ASCII glyph shorts, +0x134 glyph list, glyph +8 u0, +0x1C page 0);
  page texture = glyph +4, its raster (+0) 256x256 -> RwRasterCreate(1024, 1024, 32, 0x9504) filled at level 0,
  put in the texture, the old raster destroyed at the next swap (RwRasterDestroy 0x590FB0). The D3D9 driver
  drops the mipmap flags (raster +0x23 format byte comes back 05; RwRasterGetNumLevels 0x5910F0 answers 1
  without bit 0x80), so the filter stays linear (texture +0x50 = 2). No conflict with the co-op icons (their own
  page in a free slot, glyphs appended). Autotests spy21a (English: page swapped, subtitle sharper, no cell
  lines; straight apostrophe) and spy21a with 03GLOC.KWN copied over 01GLOC.KWN for the run ("pack 03"
  swapped, "!Grrrr! Que frio hace aqui." with its accents), 01GLOC restored.
- Round 123 (2026-10-22, user: side by side, the helmet counter group no longer looks centred under
  xxl_widescreen2d.asi). hud2.cpp centred it by its plate's middle from the stretched width (place +0x14 minus
  half of +0x34); the asi draws that plate (4/3) / aspect as wide towards its right edge, so the drawn middle sat
  right of the screen's. PlateDrawScale(): that factor while xxl_widescreen2d.asi is loaded (GetModuleHandleA),
  else 1; the pivot is the drawn middle. Sizes stay at 3/4 (round 121). Autotests hudreq / hudx3 -Level 4: the
  helmet plate's middle at 481 of 960 px (was 498), the requirement and x3 plates with it.
- Round 124 (2026-10-22, user: xxl_coop.crash-10-22.log / -10-22b.log, LV2-3: cross the detector -55.9 -> -48.1
  at z -569.6 (a fight), light the powder keg at -40.2 1.9 -549.7 -> crash ~3 s into the keg's cutscene). A game
  bug, not the mod's: crashes in single player with the x2 squads, skip_cutscenes and freedom off. Crash at
  0x531CE5 (FUN_00531B60, a Roman's move, from FUN_005339B0 with this = Roman +0x54 (its CK*EnemyCpnt, from the
  pool record +4; cpnt +0x60 = cpnt +0x10, its move parameters) + 0x60) reading address 5/6: the Roman's +0x54
  held a squad pointer (CKGrpSquad +0x5C/+0x60/+0x64 = marker list / capacity / count). Write watch on that
  Roman's +0x54 (autotest keg23, CPNTCHECK a=class b=index): written by 0x45C1F7 (FUN_0045C010, the path slot step
  of the pathfinding service yp+0x64: 20 slots of 0x514 at +0, +0xC5 == 1 free, +0xC8 state, +0x5A8 point) called
  from FUN_00467830 with slot 255. CKPathFindingCinematicBloc (class 8,4, vtable 0x624D04): +0x98 slot (-1
  none), +0x2C bit 0x40 = has a path, +0x118 tries; vf13 0x4672B0 start (slot -1 -> FUN_0045BFC0 request; none:
  bit 0x40 off, 10 tries), vf15 0x467660 tick (bit 0x40 off: request, counted down; on: FUN_00467830 steps the
  slot, and steps slot -1 too), vf14 0x467560 stop (slot -> -1, bit kept). The level stores most blocks with bit
  0x40 set; LV2-3's block #16 was ticked so with slot -1 and wrote its path point 0x50FEC past the service, over
  a Roman. Fix: Game Files\xxl_cutscenefix.asi (source coop\cutscenefix, own asi per the user's rule for game
  fixes): vtable slot 15 hook -- a block ticked with bit 0x40 and slot -1 gets the bit cleared and 10 tries, so
  it requests a slot first (vf13's own path). Logs to Game Files\xxl_cutscenefix.log. Autotests keg23 / keg23s
  (single player): crash before, with the asi block #16 takes slot 1, the cutscene ends (25.8 s), co-op resumes.
  Round 124b (user's pad test: no crash; but the game without xxl_coop.asi and the fix asi does not crash there,
  so the mod sets it off): the fix moved into xxl_coop.asi (hooks.cpp PathBlocTickHook, "hooks: cutscene path
  block ... asks for one first", 20 logs), xxl_cutscenefix.asi removed. keg23 sequence: block #16 gets slot 2 at
  the cutscene's start, vf14 drops it at 21.08 s while it is still ticked; the fix takes slot 1, cutscene ends.
  The fix also fired for 7 other blocks in the user's play: running without a slot is common, the stray write
  usually harmless. How the mod sets off the keg's case (stop then tick in one cutscene, or only the heap layout
  under the write) is not known.
- Round 125 (2026-10-22, user: centre the x3 / x10 sign and the spy's helmet requirement too, with or without
  xxl_widescreen2d.asi). Side by side they took the helmet counter's transform (right ends lined up). Now each is
  centred on its own plate's drawn middle at rest: place +0x14 less how far it has slid, width +0x34 times
  PlateDrawScale(), alignment byte +6 (0 drawn rightwards from the place, 1 about it, 2 leftwards -- these two
  are not drawn leftwards like the helmet counter's). Scale 3/4 and the vertical pivot (the counter's) as before.
  Autotests hudreq / hudx3 -Level 4, with the asi and with it moved aside: all three plates' middles on the
  split line.
- Round 126 (2026-10-22, user: the x3 / x10 sign and the spy's requirement a bit tighter under the helmet counter,
  keeping their unequal spacing in the same ratio). Side by side, the pieces shown (place not slid all the way out)
  keep the game's order; each gap, plate edge to plate edge as round 125 drew them, is cut to kGapScale = 0.5.
  Plate middles from place +0x18, height +0x38, alignment byte +7 (0 downwards, 1 about, 2 upwards); the counter is
  measured before its own Xf2d (it had been read after, which also made round 125's pieces pivot on its moved
  place -- kept as the pivot so the look is round 125's, tighter). Autotests hudreq / hudx3 -Level 4: helmet to
  requirement 35 -> 17 px, helmet to x3 35 -> 17 px (960 x 540).
- Round 127 (2026-10-22, user: redraw the font's action icons from the designer's Figma left arrow). The 13 icons
  (U+E000-E003, E005-E00B, E00E, E00F; E004 / E00C / E00D are empty cells) are vectors in the Figma frame "Action
  icons": the designer's 24 px plate (gradient fill, bevel stroke) with the glyphs traced from the originals and
  the arrow's glows. Where an icon lives depends on the pack: the first page holds as many as fit after the
  pack's extra characters (French 8, English 13, German / Spanish 9, Italian 10), the rest are on the second
  page (font +0x18, the second of the four page textures at +0x14), so the second page differs per pack too
  (02 = 03). The plate sits at the glyph cell + (1, 2) texels (the exporter's banker's rounding of u0 / v0 made
  the 28 px crops look offset by 0 or 1). The asi now swaps both pages (arial24-windowsN_LL.png); page 1 first,
  since page 0 at 1024 is what marks the font done. Autotest icons21: both pages logged swapped (xxl_coop.asi
  still draws its pad prompts over the game's icons there); with xxl_coop.asi moved aside, the title screen's
  "Press Validate" confirm icon at 1920 x 1080 is the new one.
- Round 128 (2026-10-22, user's answers to the design doc's open questions). (1) Fallen leader: the heroes are
  equal and either can set off a leader switch, so the level's intended switches are the ones made with both
  players standing -- nothing is kept for after a revive. Round 114b's levelLead (individual.cpp, set by
  IndivNoteLevelLead from proximity.cpp's lead-only zone pass) is removed: a hero that led when it fell gets the
  lead back after the revive whatever lead-only zone fired meanwhile (the zone is still let through, round 114).
  Autotest lead510o (-Level 5, XXL_DOWNAT=1:2000 XXL_UPAT=12000): 0308 let through with Obelix down, revived at
  12 s -> trio 4801, "Obelix takes the lead from Asterix (revived, it led when it fell)", no switch after.
  (2) [coop] freedom now defaults to on (config.h, dist ini); freedom_corkscrews stays on, [experimental]
  freedom_romans stays off (not final). (3) The ini's revive time comment said 8 s; it is 5 s since round 69.
  (4) Round 113's rule (a hero's own Twister shake on its player's camera only, world shakes on both) is
  user-confirmed.
- Round 128b (2026-10-22, user: undo 128 (1)): the level's switch kept for after the revive is needed -- many
  detectors that switch the leader are one-shot (they disable themselves), so a lead handed back to the revived
  hero would leave the rest of the section on a leader the level never meant, with nothing to set it right.
  levelLead / IndivNoteLevelLead restored as in round 114b (individual.cpp, individual.h, proximity.cpp,
  autotest.cpp from 5bc702f~1). Autotest lead510o: "the level gave the lead to Asterix with Obelix down", no
  lead back after the revive. Freedom on by default and the 5 s ini comment stay.

- Round 129 (user, userlogs\xxl_coop.car-lead-20261022.log, LV2 sector 4's rope cable car): Asterix boards (his
  own tick: 4801 from 0x49ACB9 gives Obelix the lead, then 4812 from 0x49ACC7 holds him, AI order 9); R3 + B undid
  the lock but the lead stayed with Obelix. The lock's picture (NoteLock, "as now FFFF": no pre-event picture) is
  taken after the car's 4801, so its "leader before" was already Obelix; UndoLock's GiveLeadLater(before) gave it
  to him. Now the leader just before a boarding switch (4800/4801 from 0x49AC80..0x49AD00, CoopNoteTrioEvent runs
  before the trio) is kept (g_carSwitch) and becomes the picture's leader when the lock follows within 0.5 s.
  The user also saw P1's camera on Obelix when finishing a ride later: with Obelix leading, the ride's shot (both
  cameras take the sector track, vt 6244EC in LV2-4) is the game's camera's; not reproduced in LV3-3 (P1's view on
  Asterix in every shot). Autotest carunlock33 (-Level 3): board, P1's R3 + B -> 4800, Asterix leads again
  (11.17 s); boards again, Obelix pulls to the end. cabled33: Obelix (leading only for the car) falls -> no lead
  back after the revive; swingundo / cross33 as before.
  Round 129 also fixes LV2-1's rope car (unlockcar21: no lead back on Oct 21, 4800 after R3 + B now). Circles
  (LV2-1 corkscrew, unlockpillar / unlockseq) were already right: their 4801 comes from an event group, so the
  lock's picture is taken before it (g_prePic). pillar36l sends 4801 / 4802 directly, so it does not show that.
- Round 130 (user: a hero boarding a cable car while not leading -- the camera bug was that; make both cars work
  for a non-leader rider, and put the lead switch on boarding behind an option, default off). New [coop]
  cable_car_rider_leads (default off, env XXL_RIDERLEADS): on = round 109's WatchTelepherRider (the automatic car's
  rider takes the lead). Rope cable car (CKHkTowedTelepher): its boarding (the rider's own tick, 0x49ACB9 4801)
  already hands the lead to the puller in single player, so there is no lead to force; boarded by the hero not
  leading, the 4801 runs inside that hero's tick with its game camera copy swapped in (GameCamBeginHeroTick), and
  the trio's switch follow-up (CKSrvCamera::SetTarget from kSwitchFollowup) aimed that copy at Obelix: P1's camera
  followed Obelix once the car's end freed Asterix (zone 0830 in LV3-3, 48B7 in LV2-4). SetTargetHook now drops a
  switch follow-up aimed at another hero while a copy is in. R3 + B after such a boarding gives back the real
  leader (g.savedLeader during the slice), not the rider posing as leader. Automatic car (CKHkTelepher, vf15
  0x5485F0): +0x84 is its own ground; it checks the leader's ground (trio vf29 = leader +0x280; event 0xB00) each
  tick and drops the rider otherwise -- proximity's tick slice keeps it going. Trio 0x0C09 (FUN_00481d50 case 9):
  a leader in situation 2 gets +0x3F0 |= 0x200 and its jump target (+0x568, +0x574); anyone else's becomes 0x4805
  (the companion's AI order 2: freedom left the rider on the car). Now with the option off a non-leader rider is
  noted (g_autoRide, from 0x0C0D 0x548757 via WatchTelepherRider; over when its situation leaves 2) and the car's
  0x0C09 (from 0x549BA5) and game companion orders (0x4813..0x4876, 0x48BB..0x48EC: LV3-6's 481C from zone 03C8)
  run with trio +0x2C/+0x2D set to the rider / the other (CoopRideSliceBegin/End around the trio's sendEvent).
  Helmets: the bonus pools' leader check uses trio vf27 = the leader's node position (+0x80 of its frame); the
  non-leader's check used the hero's own +0x6C, which stays at the boarding spot during a ride -- now the node
  (CollectPos). Autotests: carlead33 (-Level 3: Obelix leads, Asterix boards the rope car, pulled through zone
  0830 -- P1's view on Asterix after; before, on Obelix), carleadu33 (R3 + B leaves Obelix leading), carunlock33 as
  round 129; pillar36 (-Level 3): Asterix rides while Obelix leads, 481C to Obelix, the end's 0C09 makes Asterix jump
  off to (-159.7 0.8 -133.6), 9 helmets his; XXL_RIDERLEADS=1 as round 109; XXL_FREEDOM=0: Obelix handed to the
  game for 481C as with Asterix leading; boss210t as before.
- Round 131 (user: LV4-4, Obelix hits the lever at -733.742 13.944 -240.739 and the magic potion goes on both
  heroes; the flask should be Asterix's). The lever (CKHkActivator, event #178) sends 0996 "spit item" to a
  CKHkBonusSpitter (2,194, vtable 0x6294B8). Its tick (vf15 0x54A780) spits the next item -- a bonus pool type,
  +0x18 (one shot, -1 none) else +0x1C -- through CKGrpBonus_SpawnBonus (0x54BE20, pool +0x24 == type) when the
  trio leader (trio vf25) is within +0x24, and sends it after that leader (CKHkBasicBonus_FollowHero 0x54E2C0,
  bonus +0x20). Proximity slicing ticked it with Obelix as leader -> the flask flew to Obelix, the gauge filled
  (one global gauge; obelix_potion puts it on both). Now a spitter whose next item is type 3 (potion) ticks with
  Asterix as the leader whoever is nearer (proximity.cpp SpitsPotion): it waits for Asterix, then goes to him.
  Second half: CKHkBasicBonus::vf15 hands a collected bonus to pool vf40 (0x506E90), which first asks vf39
  (0x507320): type 3 only while trio vfA0 (leader is Asterix). With Obelix leading, the flask reached Asterix and
  was thrown away -- also true of ordinary potion flasks drawn to a non-leading Asterix. CollectHook
  (individual.cpp) now runs vf40 for Asterix's potion with trio +0x2C/+0x2D = Asterix/Obelix. Autotests (-Level 4,
  sector 4): spitter44 (Asterix leads, far away: before, spat at once and collected at Obelix; now spat when Asterix
  is placed beside it 8 s later, collected at Asterix), spitter44o (4801 first) and spitter44l (4801 after the
  hit): gauge 20 with Obelix still leading. A 4801 sent right after placing Obelix at the lever left his hits
  missing it (as in the pre-fix build); the hit itself is position-sensitive, the tests press X five times.
- Round 132 (user: a debug option showing who leads). [debug] show_leader (default off; on in the user's ini; env
  XXL_SHOWLEADER): "LEADER: OBELIX (P2)" drawn just before the frame is shown -- detour of the renderer's present
  FUN_00414780 (thiscall, ret 0; prologue 8B 41 04 85 C0), which both the game's frame and the split frame call. In
  the leader's half when split (player 2's: right / bottom), else top left; player 1's hero red, player 2's blue,
  white before player 2 joins. Letters are IDirect3DDevice9::Clear rectangles (vtable 43, one per run of lit pixels
  of a 5x7 block font, black box behind), with the viewport (47 / 48) set to the whole screen and put back: no
  render state touched. Screen size from the renderer's camera raster (+0x10 camera, frameBuffer +0x0C / +0x10).
  Autotests carleadu33 / carlead33 with XXL_SHOWLEADER=1 (tag in P2's half while Obelix leads, P1's after zone 0830).
- Round 133 (user: LV2-9, heroes 50+ units apart -> the non-leader's shadow goes, ~60 apart both; a lead switch
  brings it back; boxes maybe different; boxes / enemies popping in at the corner of a half). (1) Shadows:
  CKSrvShadow (vt 0x62520C) lists: +0x30/+0x34 shadows with a mesh slot ({next, shadow, ...}, shadow +8 def, +0xC
  node), +0x40/+0x44 waiting ({next, def, node, ...}), +0x2C free slots, +0x3C free list nodes, +0xC most drawn
  (30). Tick vf17 0x470A70: waiting shadows get a slot (FUN_00471050 -> shadow vf2 0x46F890: colour = def +0x1C,
  full) when their node is stamped this frame (+0x14 == 0x6464C4) and within def +0x14 of camera svc +0x5BC (look
  point); then each slotted shadow's update (vf3 = 0x46F980) returns 0 drawn, 1 dropped -> slot back, shadow
  waits (FUN_00471160), 2 removed. Update drops on node not stamped or distance to svc +0x5B0 >= def +0x14; fade
  from def +0x18. Heroes: range 50, full 40. In split the game's tick (leader's camera) dropped the other hero's
  shadow at 50 and the halves' passes (round 2 of the shadow fix: tick per half with svc +0x5B0 = the half's eye)
  dropped the leader's too in the far half, re-admitted next tick -> at 65 neither had a shadow. Now the game's
  tick measures each shadow from the nearer half's camera (last pass's eyes, same frame id), a half's pass turns
  result 1 into 3 (vf17 ignores it: not drawn, slot kept), and the passes set svc +0x5BC to the half's eye so a
  waiting shadow gets its slot in the half whose camera is near. Autotest shadow29d (-Level 2, XXL_SHADOWLOG=1: per
  walk result changes and list state; =2 also every shadow's node): before, Obelix waiting from 50 apart, both from
  65 with Obelix leading; now both slotted at 50/55/65 either way. LV2-9's boxes have no shadows (only heroes,
  Idefix, Romans). (2) Boxes: no difference with distance found (crates29: 16 live crate objects near the boxes at
  10/55/65 apart, either leader; CKCrateCpnt has no camera-distance test). But the kluster frustums (crates,
  helmets, squad keep / marker spawn tests) were built with the full-screen view window (renderer svc +0x18 -> +0x10
  RwCamera +0x68/+0x6C window, +0x80/+0x84 near/far, read by FUN_00457170), while with split_fov = 20 a half is
  0.686 x 0.773 against 0.934 x 0.525: each half shows ~47% more vertically -> crates / Romans in the top or bottom
  band of a half weren't live / kept. Now P2's and P1's own views are built with their half's window (stand-in view
  object with a copy of the camera), and the game camera gets a second frustum with the window of the half showing
  it (g_gameHalfSvc; in the union, KlusterSphereInOtherViews / InAnyView). Autotest crates44 (-Level 4, sector 4,
  user's spots -709.3 6.0 -232.7 / -712.1 12.0 -142.4): "half only" log -- crate klusters (handler 3, flags 7) at
  (-712 14 -137/-141) and helmets (handler 12) in P1's half but outside the old frustum. Not seen on screen in the
  autotest. User pad test (Oct 22): LV4-4 boxes no longer pop in, floating collectibles no longer vanish at the
  view's edges, shadows fine; their "boxes act differently far apart" was this pop-in.
