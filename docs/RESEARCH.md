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
