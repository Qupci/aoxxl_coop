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
(autotests `sas`, `sectorzone`). Since round 159 either hero's Sas box streams (vf17's position patched).

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

- Round 134 (user: with Obelix leading, the light pillar's circle (corkscrew) can't be set off by Asterix, and
  Obelix on it locks Asterix where he stands -- the user's go-ahead for the suggestion of round 132).
  CKHkCorkscrew::vf15 (0x53C3F0, ret 4): armed (+8 bit 1) and not landed (bit 0), it flags its landing event
  (+0x40; LV2-1's 0050: trio 4801 + 4802, pillar 0901, detectors) and sets bit 0 when the trio's leader (trio
  vf25, its node +0x80) is within +0x3C of its circle; at the top of its turn it asks the trio 0xB01 (FUN_00481c90
  case 1: the companion's ground, trio+0x2D's +0x280; 0xB00 is the leader's) and, on its own ground (+0x30), sends
  0x0C09 (-> 4805: the companion's AI order 2, the launch) and its end event (+0x42). So only the landing check
  wants "the leader", and the event makes the hero on the circle the companion. Now proximity's TickLife ticks an
  armed, not landed corkscrew whose landing event both switches the lead and tells the companion to wait (4800 /
  48B7 -> made for Obelix, 4801 / 48B8 -> for Asterix; 4802 / 4849) with that hero posing as the leader when the
  real leader is the other one (TickCorkscrew; not when that hero is out or not a player's): he lands on it as in
  single player, the leader standing there sets nothing off. Autotests corklead21 (level 2: 4801, Asterix onto
  the spinning platform's circle -- before: nothing; now 0050, the freedom corkscrew lock, 03C8's 4803 held back,
  the ride to 0058) and corkobelix21 (Obelix leading onto the circle, Asterix 4.2 units off -- before: 0050, Asterix
  told to wait; now nothing, Asterix walks off). turnstile21 / 21b / corkdown as before. LV3-6's pillar (live only
  after its fight) not run: same class and code.
- Round 135 (user, LV2-8, userlogs\xxl_coop.lv2-8_freedom_on.log / _off.log): with freedom on, once Asterix stood
  on the light pillar on the platform (42.06 57.0 -120.10; its event: 4801 + 4802, Asterix free -- the user: like
  the rotary beam, he may move and jump), player 2's Obelix got no "Pull" at the platform (37.92 51.0 -120.16);
  with freedom off he did. The platform is a CKHkPushPullAsterix (2,147, vt 0x626110) for Obelix only (+0x14 bit 7;
  bit 6 = Asterix). Its tick (vf15 0x4B5140) offers the push / pull (0x900 to trio vf25) only while trio vfA0 says
  the leader is a hero it is for (and bit 12 of +0x18, the leader near its push spots +0x1D8/+0x1E0). The mod ticks
  objects with the nearest player's hero posing as the leader (proximity TickLife) unless a hero is out: freedom
  off held Asterix (HeroOut -> no slicing, Obelix the real leader); freedom on left him free on the platform, the
  nearest, so the platform ticked as his and never offered Obelix anything (switching leaders did not help: then
  the offer is Asterix's own and he is not one it is for). Now a block for only one hero, not being pushed, ticks
  with that hero as the leader whoever is nearer (BlockHero). Autotest pull28 (level 2 sector 8; the user's ini
  had freedom off, so XXL_FREEDOM=1): before, no 0901 after the pillar; now 0901 and 151 pull steps, the platform
  carries Asterix -- the same as with XXL_FREEDOM=0. pushkeg5 (-Level 5) as before.
- Round 136 (user, LV2-8, userlogs\xxl_coop.lv2-8_rotarybeam.log: in freedom mode the rotary beam by the turnstile
  (Asterix on the cutscene platform 41.0 58.4 -89.6, zone 02B0; Obelix at the turnstile 45.5 52.6 -66.6 hitting
  it) did not take Asterix when he jumped; LV4-2's still did). The beam's catch is trio 0x0C02 from the beam
  (0x53F348): FUN_00481d50 case 2 hangs the companion (trio+0x2D) on it -- AI order 8, +0x874.. its target, beam
  0x0C04 -- only while the companion's order (+0x564) is 3 and it is within trio +0x50 of the beam; freedom's
  WatchFreedomBeam then hands a hero that jumped within 1 s to the game for the ride. The 3 comes from the
  platform's pillar ("Obelix leads, Asterix waits", round 135's), kept as a value while freedom leaves Asterix
  free; any lead switch since sets the other hero's order to 0 (trio 4800/4801) -- in the user's run their
  R3 + X, and they hit it without the cheat too (which switch then is not known). Now in freedom mode a companion
  player's hero that jumped in the last second gets order 3 for the 0x0C02 (CoopBeamCatchBegin/End around the
  trio's sendEvent; put back if not caught). Autotests beam28 (level 2 sector 8, the natural order: the
  platform's pillar, Asterix onto the cutscene platform, Obelix kept at the turnstile on the ice with S_NEAROBJ
  (2,45) hitting it, Asterix jumping: caught, before and after) and beam28b (the lead to Asterix and back after the
  cutscene: before, 0x0C02 took nobody; now "taken as waiting", caught). Not covered: a jumper who is the leader
  (the catch only ever takes the companion). The user's other report -- Obelix could not be teleported to the
  turnstile after the cutscene unless leading -- did not reproduce with the autotest's placement (tele28: he
  stays there); open, waiting for how the user teleports.
- Round 137 (user, LV2-8, userlogs\xxl_coop.lv2-8_freedom_on_camera_fail.log: freedom on, Asterix off the platforms
  on purpose -- the lead came back to him, but player 1's camera kept the platforms' shot). CKSrvCamera tick
  (0x454620): each frame the first camera sector (service +0x24, CKCameraSector 12,8: +0x29 bit 1 on, box +8..+0x1C,
  track +0x20) whose box holds the camera's target (+0xF0..+0xF8) is taken; only a different one changes the track
  (+0x4EC the last) -- leaving a box changes nothing unless another holds the target. With the second block pushed
  into place (zone 02E9; the user: moving the platform starts the camera script) the platforms' sector (15.5 36.7
  -126.5)-(46.5 84.1 -98.8), before the area's own (-10.6 29.8 -197.6)-(81.6 88.6 -77.5) in the list, was switched
  on; its box covers the ground below too. The level's start over when Asterix falls (zone 03B8: trio 4800 + 4803,
  the blocks back 09A2/09A3/0996/0998, detectors, the light pillar 0x900) does not switch it off -- in single player
  Asterix waits on the pillar there and never falls. Now LockPicture keeps the camera sectors' +0x29, and a flagged
  group that lights a light pillar (0x900) and frees the companion (4803) while a wait's picture is held (g_lock with
  wait, else g_seqPic) switches off the sectors switched on since (CamerasBack, in NoteTrioEventFlagged); the mod's own
  undos (RestoreLockPicture: R3 + B, a fall) do it too. Autotest camfall28 (level 2 sector 8: the pillar, Obelix's
  pull, new step S_CAMSECTOR switching on the platforms' sector as 02E9 does, Asterix off the platform through 03B8):
  before, both cameras stayed on its track; now "03B8 starts the wait's sequence over -- 1 camera sectors ... off",
  both back on the area's track. turnstile21b / corkdown / lockcircle / carunlock33 / pull28 as before.
- Round 138 (user, LV2-8, userlogs\xxl_coop.lv2-8_lead_switch_romans.log: the platforms done, Asterix waiting at the
  rotary beam's spot, Obelix fighting the turnstile's Romans -- the lead went to Asterix out of nowhere; the user:
  Obelix had jumped onto the turnstile). Zone 03B8 (Asterix off the platforms: trio 4800 + 4803, the blocks back,
  detectors, the light pillar 0x900 -- the level's start over, bound to Asterix) fired in the other hero's pass
  (DetectTickHook: hero-bound tests run again with the heroes' nodes swapped) for Obelix up on the turnstile (44.8
  55.5 -69.3), inside its height volume. ZoneMeansItsHero (the swapped pass's filter) let it through: its only trio
  orders were a lead switch and 4803. Now a zone whose group lights a light pillar (CKHkLightPillar 0x900) means
  the hero it is bound to: a wait's sequence started or started over is the waiting hero's. Autotest lead28 (level 2
  sector 8: the pillar, Asterix at the beam's spot, Obelix put up on the turnstile hopping): before, 03B8 "(the
  other hero's zone)" and 4800; now none in two runs. camfall28 (Asterix's own fall still starts over, cameras off),
  pull28, beam28 as before.
- Round 139 (user, LV3-3, userlogs\xxl_coop.lv3-3_machinegun.log: Asterix taking the machine gun while not leading
  took the lead until he left it -- by design since round 21 (CoopAfterLifeTick: a mode change started in the
  hero's own sliced tick kept the slice as the lead, groupTookLead); asked for the cable cars' equality). The gun
  itself never asks the trio (no trio call in CKHkMachinegun / its Life but sendEvent); the trio's 0x4809 runs in
  Asterix's tick with his camera copy in. Now a mode 2 started by Asterix's own tick (gunAlone) restores the slice:
  the real leader stays, Asterix keeps mode 2 in the player variant (f18 08) and turns and fires it (gun 0x4A02),
  the gun's camera in his half. Obelix joining (JoinGun: held, AI order 7 to the handles, then 0x480A) needs Obelix
  as the companion -- a leading hero is handed back to its player at once -- so while Obelix leads, joining gives
  Asterix the lead for the joined part (one screen, co-op suspended) and GiveBackLead returns it. GiveBackLead had
  skipped a held previous leader -- Obelix is still held when 0x4806 resumes co-op and let go later that frame --,
  so after a joined gun the lead never went back (the pre-change build too); a held hero whose order is already
  0 now counts as free. A leader who falls while Asterix is on the gun alone: IndivFrame's "the lead stays with a
  hero still standing" and BringToFallenLeader wait while the standing hero is not in a playable mode (the switch
  refuses a mode 2 target; teleporting a gunner) -- once off the gun he gets the lead and is brought to the fallen
  leader out of reach, and sent back by the gun after the revive. Autotests mgun33l (Obelix leads: solo use, Obelix
  joins, leave -- Obelix leads again), mgun33d (XXL_DOWNAT=1:9000 XXL_UPAT=20000: Obelix down at the rope cable car,
  across the river; Asterix untouched on the gun, brought once off it, back by the gun after the revive);
  mgun33j / mgun33o / mgun33a as before.
- Round 140 (user: Asterix leading should ride the rotary beam too -- no difference with him as the companion).
  The beam's catch (trio 0x0C02, FUN_00481d50 case 2) and its drop at the far end (0x0C03 from 0x53F4C3, case 3:
  the companion's order 8 -> 3; anyone else's -> 0x0C05 to the beam) both act on the companion (trio +0x2D).
  CoopBeamCatchBegin/End now: only Asterix (Obelix has no ride); jumped in the last second and not held; if he is
  not the companion for the event (he leads, the beam ticked with him as the leader) the trio's leader / companion
  are swapped for the event and put back after; his order 3 for it unless already 3 as the companion (the game's
  own catch). The drop is run with him as the companion while he leads and hangs (order 8). The held release
  ("leads again") waits while a leading hero's order is 8, the beam's ride. Before the drop slice, a leading rider
  kept order 8 after the drop, stayed held, and the arrival zone's 4800 switched to a held (AI variant) leader --
  Obelix ended up in mode 1 and co-op was suspended. Autotest beam42l (-Level 4, XXL_P2HERO=asterix XXL_FREEDOM=1:
  beam42f with 4800 / 4801 / 4800 after the circle -- Asterix leads, order 0): "taken as waiting for its catch",
  carried across while leading (the leader tag on him), "the rotary beam drops Asterix (leading)", his player's
  again, landed where the companion lands (-414.3 1.1 -99.2). beam42f / beam28 / beam28b as before.
- Round 141 (user, LV5-3, userlogs\xxl_coop.lv5-3_corkscrew.log: after the corkscrew's launch Asterix stayed locked;
  corkscrew -259.71 10.49 353.69, turnstile -255.87 9.54 358.30; the user's editor shot: box detector #3, list +0x2C,
  (-263.909 15.993 352.549)-(-261.909 19.993 354.549), not on at start, event #35 = 0118: trio 4800, camera sector
  0x700, detector 1D31, trio 4803). The corkscrew's landing event 0110 (4801 + 4802, detection 1403 = box #3 on,
  camera 0701, pillar 0901, 1631) locks Asterix (freedom_corkscrews); the turnstile's hits raise it (turnstile 0x300
  -> FUN_0053E900: trio 0907, its hit group, corkscrew 0x996); at the top the corkscrew's 0x0C09 (0x53C5F4) -> 4805,
  the companion's AI order 2 to its marker, inside box #3 -- whose 0118 gives Asterix the lead, and the lead frees
  him (held release "leads again"). In the user's run 0118 never fired: Asterix ended the jump at the box's centre
  (-262.63 16.14 353.85), order 3, held for good. Not reproduced: in every test run the box fires as the jump
  crosses its edge (-261.98 16.10 353.82). Now the corkscrew lock has its own end in freedom mode: after the
  launch (0x0C09 from 0x53C5F4 while g_corkRide is held) the hero is its player's again once the launch's jump
  (order 2) is over (WatchCorkLaunch; 3 s if the jump never shows). Zone 0218 (the turnstile's Roman fight: turnstile
  090D, corkscrew 099A off, squad 0D17) and the corkscrew's arming (0x999) skipped in the autotests. New test steps:
  S_DETOFF (detector b of the detection service's list +a off). Autotests cork53 (-Level 5 sector 3: zone 0368,
  corkscrew 0x999, Asterix onto the circle, the turnstile's 0x996 every 150 ms: launch, 0118, lead, free), cork53b
  (box #3 off after the circle: before, held for good at the user's spot; now "landed from the corkscrew's launch
  -- back to its player", walked by player 1). turnstile21 / corklead21 as before (LV2-1 has no corkscrew launch).
- Round 142 (user, LV5-3, userlogs\xxl_coop.dis_split_screen.log / xxl_coop.launcher_split_screen.log). (1) Player 2
  leaving (Start; "split: off") after the level's switch to Obelix (zone 4801 at 28.9 s) gave the lead back to
  Asterix: Release switched to player 1's hero whenever that one led when player 2 joined (joinLeader). Now the
  leader stays (single player from there: player 1 drives whoever leads, as after the game's own switches); a lead
  the mod only lent (groupTookLead) goes back first (GiveBackLead). joinLeader removed. (2) A launcher started by the
  hero not leading (situation 0x0D in its sliced tick; the trio 0x4804 from 0x49C699 -- lifters send none) keeps
  the lead after landing: the flight asks the trio for its leader, so the mod had lent it and gave it back on
  resume -- to Obelix in the user's run (their R3 + X before). No level switch happened there; in single player
  only the leader uses a launcher and still leads on landing. g.groupLauncher marks it (launcherAt in
  CoopNoteTrioEvent); GiveBackLead keeps the lead for it. Autotests leave2 (level 2: 4801, player 2 leaves --
  before Asterix, now Obelix, walked by player 1), launch53 (-Level 5 sector 3: Obelix leads, player 1's Asterix
  launches -- before Obelix after landing, now Asterix), launchp2 / launch48 (player 2's Obelix launches: he keeps
  it, the landing ground's switch hands it to Asterix), p2basic as before.
- Round 143 (user: the hero not leading took the lead while at the merchant -- seen with Asterix; Obelix did the
  same, the show_leader tag in his half under the shop's screen). The merchant no longer moves the lead: its X in
  the non-leader's sliced tick (situation 0x0A) sets g.shopHero (until the shop's state is 0 and the hero is out of
  0x0A, or a forced close). CKHkAsterixShop (vt 0x6263CC) ticks with that hero as the trio's leader (proximity.cpp;
  its states use the game manager, yp +0x8C, whose +0x14 is the trio -- 0x5122 offer, a purchase's 0x907). The game's
  camera, which the shop's states take over (state 1's shop track, state 5's hand-back), is that hero's for the
  visit: gamecam's live owner is CoopShopHero() when that is the non-leader (TickHook; GameCamHandTo at the X, right
  after the hero's tick, so a camera set up in that tick -- in its copy -- comes along). Player 2's half took the
  game's camera only when player 2's hero led (splitscreen.cpp p2Leads): now when it is the camera's hero
  (GameCamLiveHero) -- else it showed player 1's camera shifted (the sea, LV2-1). CloseShopFor clears shopHero: a
  launcher's flight (shoplaunch4: Asterix leading at the merchant, Obelix launches) ran on Asterix's camera kept for
  the shop -- both halves fog after the landing. Also: TrioSendEventHook sets g_trioEventCaller again after
  CoopDeferTrioEvent -- the shop's close (0x5124) sent in there overwrote the launcher's 0x49C699, so round 142's
  "launcher keeps the lead" was lost with a shop open. Autotests shopa (new: Obelix leads, Asterix shops), shopo,
  shopsplit, shopspy, shopzone, shopboss, shoplaunch4 + new 4b (Obelix leads, Asterix shops, Obelix launches), 4c
  (Asterix leads, Obelix shops, Asterix launches), 4d (Obelix leads and shops, Asterix launches): the lead stays,
  the shop in the shopper's half, each half on its hero after the landing.
- Round 144 (user, LV2-1, userlogs\xxl_coop.torch_nonleader.log: with Obelix leading, Asterix could not take the
  torch at -744.2 513.8 -982.1; the prompt was player 2's and Obelix, across the map, went into the take situation
  05). The torch's tick (proximity: Asterix nearest, posing as the leader; vf40 "leader is Asterix") offers it with
  trio 0x0C06 -> trio +0x4C (what X takes: FUN_00499CF0 / FUN_0049A080; the trio's life tick, FUN_004817F0, clears
  it unless +0x3F4 bit 0x10). The non-leader's sliced tick hid +0x4C (offers meant for the leader -- the potion),
  so only the leader saw it. Now TrioSendEventHook notes a +0x4C an event put there with the trio's leader byte
  of that moment (CoopNoteOffer; an object ticked as that hero's): the non-leader's tick keeps an offer made for
  it, the leader's tick has one made for the other player's hero hidden (restored after). Hearths need nothing
  more: CKHkHearth::vf15 (0x4AEBB0) asks trio vf94 (the leader's carried object: class 0x20 = torch) and the
  leader within +0x18, then trio 0x511D ("Light", prompt kind 8) -- sliced to the nearest, it is Asterix's.
  Autotests torchnl2 (Obelix leads) / torchl2 (Asterix leads): take the torch, light it at the lit hearth
  (-745.1 513.9 -978.6), light the hearth from -785.7 526.8 -983.0, then LV2-3's keg from -50.7 0.8 -522.0 (both
  user spots): the same in both. potion, flask unchanged.
  Also: spitter44o (LV4-4's lever, Obelix leading, Asterix still at the level start) no longer hit the lever since
  round 135 (bisected: cb0a78a; 0/5 runs vs 4/4 without its rule). The one-hero block rule sliced an Asterix
  block at -913 37 -469, 860 units from both heroes, as Asterix every frame -- what it changed for the lever's hits
  is not known. The rule now applies only within the slicing range of that hero (pull28 unchanged: Pull for
  player 2). New autotests spitter44n (Asterix on the ledge behind Obelix at -733.0 14.0 -239.3 when Obelix hits)
  and spitter44a (Asterix, not leading, hits the lever himself): the flask goes to Asterix, 2/2 each. A spot
  off the ledge (-731.5 -244) drops a hero to y 6, outside the loaded geometry (the user: "stuck in the unloaded
  sector").
- Round 145 (user, LV4-4, userlogs\xxl_coop.lever_potion_dash.log: Obelix leading hit the spitter's lever, the
  flask went to Asterix -- and Obelix ran on by himself, situation 0x10). The potion's dash: game manager +0x5C
  bit 19 sends every player-driven hero on the ground into situation 0x10 (Hero_ChooseSituationState 0x49C900);
  only the drinker gets the flag that ends it (+0x388 bit 0x4000). The mod hid bit 19 from the non-leader's tick
  only -- right while the leader drinks; Asterix, not leading, drank: the leader Obelix dashed for good and
  Asterix not at all. Now the dash is its drinker's: CollectHook notes a potion flask's hero (IndivPotionDrinker,
  3 s), else the leader when bit 19 goes up (g.dashHero), and the bit is hidden from the other player's hero
  (CoopBeforeLifeTick, either hero). Autotest spitter44d (Obelix leads, Asterix behind him; lever 14 at -734.1
  13.9 -241.6, event 0590 = group #178, worked as a punch does: new step S_PULLLEVER, CKHkActivator's FUN_0053B610):
  before, Obelix in 0x10 to the end (47 rows); now Asterix 0.4 s, Obelix walks. spitter44a / dash / potion /
  flask as before. Punches on that lever miss it in some runs (spitter44n 2 of 5): CKHkActivator::sendEvent
  0x301 needs the punch's shape on the lever. New step S_FLAGEV (an event flagged in the event service).
  Line endings: src/autotest.cpp and this file are LF (this file since b5e1b50); rounds 143-144 added CRLF lines
  to both -- converted back to LF. The other sources are CRLF.
- Round 146 (user: a cutscene skipped with Enter / A lost what its later parts would have done -- level 1's music
  after the intro and after finding Obelix; LV2-10's boss: Asterix not brought along). The skip (CKSrvCinematic::vf17
  -> FUN_00461A80 on a scene with 0x1000; skip_cutscenes gives it to all) calls +0x44 on the running nodes (an empty
  function for most blocs), clears the running list (scene +0xC) and fades out (FUN_00461B10); the scene's own end
  event (+0x64, FUN_004609E0) still fires, nodes not reached yet never run. Cinematic graph (XXL-Editor
  CKCinematicNode.h + the game's code): scene +4/+8 the edge list (target nodes), +0x10/+0x14 scene datas, +0x20
  flags, +0x28 start door; node +4 next running, +8 / +0xA / +0xC first "start" edge / first "stop" edge / count,
  +0x14 flags (1 started, 4 done, 0x40 its edges count, 0x80 once), +0x18 group, +0x1C scene; blocs +0x10 scene
  data, +0x20/+0x24 the sekens line they wait for; doors (And 0x62495C, Or 0x6249A4, Rand 0x6249EC, StartDoor
  0x624A34) +0x20 inputs of +0x24, done bit kept from the load (only blocs clear it), run again whenever reached.
  vtable +0x34 start, +0x38 end (start-edge targets +0x34, stop-edge targets +0x40; FUN_00460D90 mode 1 / 2), +0x3C
  tick, +0x40 stop (sets done, then +0x38: its edges follow; a StartEvent bloc stopped flags nothing; a door's does
  nothing). CKStartEventCinematicBloc (0x624E70): event +0x2C flagged in its tick (FUN_00460C20) -- the level's
  hooks into a cutscene: e.g. 0501 to a CKManageEventCinematicBloc (0x624C2C, starts its node: a music stream) and
  0500 to a CKManagerEventStopCinematicBloc (0x624C74). Groups (0x624B54/0x624F48): +0x34 first inner node, +0x38
  the inner node they end with. Path / camera blocs run with bit 0 up off the running list. Now FUN_00461A80 is
  detoured (not for CKHkInterfaceMain::vf18's call, return 0x524359; not for scenes of kind 0x80): the graph is run
  on from its started nodes without time -- doors count inputs, each node ends once, stops follow the game's rules
  -- and every StartEvent bloc reached has its event flagged, in order (RunOnSkippedScene; XXL_SKIPLOG traces it).
  Level 1: the intro skipped 1.5 s in flags 0020 -> music stream 10, as at its end (57 s); before, no stream. The
  Obelix cutscene skipped flags 0050 -> stream 0 (34 s when played; the squad 4D0E too). LV2-10's boss cutscene
  (scene 5) has no StartEvent bloc (its end event 0B50 fires either way): boss210u / boss210v (the user's spots,
  userlogs xxl_coop.lv2-10_asterix_no_tp.log: Asterix 9 units behind when Obelix wakes the boss) bring Asterix
  along with and without the skip on this build; the user's run (Oct 22 build, freedom on) logged the boss run but
  no bring-along -- not reproduced. Autotest steps: S_CINEDUMP (a scene's graph and events), CINE_ENTER (skip as
  Enter does). Autotests skip1, intro1, obelix1s, obelix1f, boss210s, boss210f, boss210u, boss210v; lvl1obelix,
  boss210t unchanged.
- Round 147 (user: LV2-10 closed -- the round 146 skip fixed it. force_level -1: since round 146 the game's start
  skipped past the title -- no logo; before, A after ~10 s skipped the intro, the film after it came up (its
  camera off the level), a second A landed on the title with its logo). Level 0 (autotest boot0, XXL_LEVEL=-1:
  the timeline starts with level 0's first cutscene, nothing pressed): scene 0 the intro (10.2 s after launch, 16 s;
  skippable only through skip_cutscenes), scene 1 the film (5901, the game's own Enter skip -- "after ~25 s"), the
  title ~76 s in: its flight's And door sends the title music (0028: ManageEvent 0501) and game manager
  (CKAsterixGameManager 0x627F6C) 5309/5305, a later one stops the music (0030) and sends 530A as the flight goes
  on over Gaul (scene 2, kind 0x80). Run on, a skip of scene 1 fired both at once: past the title. Skipped the
  game's way, scene 1's end starts scene 3, the title (music stream 1, the logo). Now level 0 skips the game's
  way, and a skip of its first scene (scenes[0], the intro) also skips the film as it starts (CoopFrame watch,
  6 s): one press at either time lands on the title. Autotests boot1 (skip 2 s into the intro, ~12 s after
  launch), boot2 (17 s in, ~27 s, the film): logo and title music ~4 s later. Test steps S_WATCHCINE (scene
  flags as they change); "boot..." scenarios start with level 0's first cutscene and press nothing (the WAIT
  phase's Enter for the no-controller notice is keybd_event). skip1 unchanged.
- Round 148 (user request: the squads' and combo bar's rules follow who is fighting). [experimental] coop_only (on by
  default; env XXL_COOPONLY): squads and combo bar only while player 2 plays. Modes (experimental.cpp ModeNow): GAME
  (single player with coop_only), APART (player 2 in; a hero held -- CoopHeroHeld --, fallen, or the heroes apart by
  the graph), ON. Apart: pathfind.cpp PfConnected (one graph load, a flood each way; either way open = together),
  every 0.5 s, 3 noes in a row -> apart, the first yes -> together, not known (a place in no node: LV4-1's level
  start -288 124 is outside the graph) -> as it was; ~0.5 ms a look-up in LV4. Romans at once (script numbers, U1
  +0x10 always, U2 +0x11 while the squad has not woken and still holds one of the two values) and the combo bar
  (thresholds gm +0xB8..BA/+0xC8, drains +0x9C/+0xA0) are the rules' only in ON. A bar under way keeps its share:
  hits x new/old full (not full: below it), gauge +0xA4 and decay +0xA8 x their ratios, for the hero in the game's
  place and individual heroes' copies (IndivEachComboCopy; also the kept stats of a player 2 who left). Romans to
  defeat are no longer scaled at load: the squad's records keep the level's numbers, the mod's additions per pool
  (own = its doubling, extra = others' shares) go in each frame (UpdateSquads). A squad is under way once a pool has
  sent in, queued or fallen (+0x16/+0x14/+0x15; vf34 un-sends the Romans it returns, so a squad put away before
  any fell is fresh again; FUN_0052D460 / vf33's idle-restart zero +0x16). Fresh: follows the mode. Under way:
  raised (Raise) when the mode is ON, lowered (Lower: what is not sent in yet) only in GAME (player 2 gone). Raise
  mid-fight gives each scaled pool still in the fight (left to send, queued, fallen or alive -- vf42 0x4C81E0, 0
  while the squad is inactive) its share; a beaten pool's share goes to the pool with the most left to send of the
  squad, else of the gate's squads (GateFrame), never to kept pools (placed-only, turtles). GateFrame gives the kept
  pools' doubling only to a scaled squad of the gate (the share waits while the fight runs at the level's numbers;
  a Lower hands it back). Cond 16 (sent in so far) scaled only for a scaled squad. Safety net (TrimStuck): a gate
  squad's pool with the mod's Romans left to send but none queued / fallen / alive for 6 s while active loses them.
  Autotests (-Level 4; LV4-1's lock squad 94 + hiding 6, box x -361.6..-245.8 z -6.3..156.0, wakes at the level's
  start): rules4t (together: 200), rules4d (XXL_DOWNAT=1:1000 XXL_UPAT=20000: squads put away at 2 s -> fight
  starts with Obelix down: 100; revive: 185 left + 15 beaten = 200), rules4p (Obelix on a ledge -303.9 2.8 46.5
  cut off both ways -> apart 1.4 s later; fight 100; brought beside: 195 + 5 = 200), rules4l (player 2 leaves at
  15 s: 90 left + 10 beaten = 100; back at 21 s: 180 + 20 = 200), lock4 (single player: 100; XXL_COOPONLY=0: 200).
  Test steps S_APARTSPOT (pathfind PfSpotCutOff: the nearest walkable cell cut off both ways), S_SQUADS (boxes,
  active, gate, pools), S_SQUADRESET (every active squad's vf34). Test lesson (user): a player's view on the fight
  turns down the reinforcement points behind the other's camera (proximity SpawnPointHook, retried each frame) --
  with Obelix standing or lying by LV4-1's lock and both cameras on the arena no Roman came back (~1600 refusals);
  the tests keep Obelix's view off the fight. Not changed in the game (a fallback after a while of refusals was
  written and reverted).
- Round 149 (user, after a pad test of round 148: "fine so far"; options). The [experimental] section is now
  [coop_difficulty] (squads, merchant, combo bar and the modes); freedom_romans moved to [coop_difficulty.experimental];
  coop_only is difficulty_coop_only. New: difficulty_static (off; env XXL_STATIC): ModeNow never APART in co-op --
  the rules on all through (difficulty_coop_only still counts). difficulty_pathfinding (on; env XXL_APARTPATH): the
  graph check as an apart reason. The 0.5 s PfConnected look-up (ApartFrame) runs only with g_pathCheck = some squad
  or combo factor != 1, not static, pathfinding on (logged at install: "rules none / static / dynamic; the heroes'
  path looked up / not looked up"). difficulty_to_defeat_stays (off; env XXL_DEFEATSTAYS): a squad whose fight began
  APART (SquadOrig startApart, set while it is fresh, cleared in GAME) is not raised when ON comes. Also: a fight
  gate's squads latch as one -- a squad behind a gate (+0x154 >= -1) that has not sent a Roman in yet, while another
  of the gate's is under way (last frame), counts as under way (not lowered when the heroes come apart, its startApart
  the gate's); before, such a squad followed the mode and the gate's number could drop with a hero going down (LV4-1's
  gate has no such squad: both wake with the lock). Test aid XXL_FACTORS1 (the five squad and combo factors at 1).
  Autotests (-Level 4): rules4d + XXL_DEFEATSTAYS=1: 100, stays after the revive (76 left at 42 s); rules4d +
  XXL_STATIC=1: 200 with Obelix down, path not looked up; rules4p + XXL_APARTPATH=0: Obelix cut off, 200; rules4p +
  XXL_FACTORS1=1: no look-up; rules4d (defaults): 100, revive: 184 left + 16 beaten = 200. Not done (user): a
  reinforcement fallback for the views-on-the-arena stall.
- Round 150 (user, LV3-4: Obelix left in the library 126.8 -1.2 -325.8, Asterix in the courtyard's fight 103.3 -1.3
  -313.6 -- the gate doubled). The graph saw it (reach no / no; the library's doors are slide doors, CKHkSlideDoor
  2,100: +0x18 open, +0x28 steps, +0x2A step, +0x2B flags, +0x2C / +0x38 the two ends -- not read by pathfind.cpp,
  the graph's own transitions part the rooms). The cause: a squad counted as under way from its first Roman sent,
  and LV3-4's courtyard squad (box x 23.2..170.4 z -378.9..-258.5; pools 1, 1 placed guards, then 2, 6, 40, 10)
  places its two guards as the sector loads -- latched doubled with the heroes together, long before its fight.
  The fight: the user's editor shots -- sphere detector 99.9 -4.0 -315.3 r 25 (not on at start; a cross rectangle
  at x 117.4, z -317.6..-280.9, crossed from the x 125.9 side, switches it on), its event list #175 (event id =
  list * 8 + bit: 0x578): activator 090F fight lock with icon, squad 0D17 add all pools to the lock (+0x154 = -1),
  detector 1C06 off, trio 4821 sidekick to a marker, a cinematic 0501. LV4-1's lock squad is the same case (wakes
  at the level's start, 6 placed). Now (experimental.cpp GateStartFrame): a fight behind a gate begins as the gate
  takes its squads (some squad +0x154 >= -1, none before) and is set 2.5 s later (kGateSettleMs: the path look-up's
  three noes after doors the lock shuts) with the mode then -- the gate's squads as one; before that a squad
  follows the mode whatever it has sent. Without a gate: at its first Roman beaten of a scaled pool (Beaten: +0x15,
  or sent > alive). Also (user): Raise / Lower scale the Romans still to defeat (Remaining = count - sent + alive)
  instead of the level's numbers -- doubled from where the fight stands (91 left -> 182; player 2 leaving 191 ->
  96, back 92 -> 184); a pool with none left to send passes its share on (BestPool / gate); kept pools' share in
  GateFrame from their Romans still to defeat; Lower takes the others' shares first, then halves the pool's own.
  Script cond 16 (sent in so far) scaled by the pool's (level + own) / level. Test steps: S_DOORS (swing / slide
  doors near a hero). Autotests (-Level 3): lock34 / lock34f (exploration: doors, graph dump, squads), lock34g (the
  user's case via event 0x578: 60, Obelix brought beside at 21 s: 120), lock34t (together: 120); (-Level 4) rules4n
  (rules4d without the squad reset: 100; revive: 91 -> 182), rules4t 200, rules4l (191 -> 96 -> 184), rules4p
  (gate 200, Obelix cut off 1.9 s later: 100 within the settle; reunion doubled), rules4n + XXL_DEFEATSTAYS=1 (100,
  stays), lock4 (single player 100).
- Round 151 (user, after round 143's merchant without a lead switch; userlogs xxl_coop.merchant_issues.log,
  xxl_coop.LV2_10_merchant_save.log, xxl_coop.LV2_10_merchant_save2.log). (1) What the hero not leading bought went
  to the leader: the merchant's item menu (FUN_004BFC50, its state 3) writes the health and shields straight into
  the game manager's +0x40 block (items 0-2 a ham: +0x50 += shop +0x114[code]; 3-5 a shield: +0x51 + 1, health
  topped up by shop +0x111[code]; 6+ a combo: block +0x10 bit (code - 6), shared) and reads them for its "full"
  refusals; helmets +0x68 are shared. Individual heroes kept the leader's there outside any one hero's business.
  Now the shop's tick (proximity.cpp, kShopVtbl, already sliced to g.shopHero) runs inside IndivEnter(shopper);
  a purchase is logged ("bought from the merchant: helmets, health"). (2) Asterix leading steered by the shop's
  camera while Obelix shopped (stick up -> sideways): player 1's tick took its own camera's basis only when player
  2's hero led (coop.cpp CoopBeforeLifeTick, P1CamBeginHeroTick); round 143 made the game's camera the shopper's
  for the visit. Now: when the game's camera's hero (GameCamLiveHero, else the leader) is player 2's. (3) The save
  menu (bars) closing the shop of the hero not leading left the leader's view on the shopper after the menu
  (Obelix's half on Asterix / Asterix's on Obelix): CloseShopFor handed the shop's camera back aimed at the shopper
  in the game's camera (the shopper's for the visit), then co-op suspended the same frame -- the copy (the leader's
  own camera) dropped before the camera tick's "visit over" swap. Now CloseShopFor, after that hand-back, gives the
  game's camera to the leader at once (GameCamHandTo: the two states change places), as a natural end does. The
  launcher's close (4804, camera already the launcher's: "not up" branch) is unchanged. (4) Not changed: the
  leader switch after the save menu (save2, Obelix leading) is the level's: LV2-10's zone 05A0 by the save point
  (group 0B4: detector 1622, corkscrew 0999, trio 4800 + 4803, light pillar 0900; one shot) sent "Asterix leads" as
  Asterix walked to the druid; round 70 holds it until the visit ends (without the shop it switches at once).
  Test aids: S_STEERCHECK (a hero's walk against its player's view: copy, else the game's camera), S_CAMTARGET
  (whom the game's camera and the copy follow: svc +0xE8 vs a hero's +0x10), S_BIND (up to three bindings of the
  game's values for x frames). The save menu's "Cancel save?" message box reads its own bindings: 54 / 55 left /
  right, 56 A (the user's preset: D-pad); the save menu itself 21 (cancel). The druid's X needs Asterix facing it
  (a glide keeps the old facing: X punched). Autotests (all -Level 2; before -> after): shopbuyo (Asterix leads,
  Obelix buys a ham and a shield, both at 5 health: before A 5/3 -> 20/4 and O 5/3 kept, after O 5/3 -> 20/4
  and A 5/3 kept; Asterix's walk in the visit
  -0.10 / -1.00 -> 1.00 / -0.05 along / across his own camera), shopsavea (sector 10; Obelix leads, Asterix shops,
  Obelix saves: after the menu "the game's camera (Obelix's) follows Asterix" -> "follows Obelix"), shopsaveo
  (Asterix leads, Obelix shops, Asterix saves: "(Asterix's) follows Obelix" -> "follows Asterix"; walk in the visit
  -0.03 / 1.00 -> 0.98 / -0.19). shopsplit, shopzone, shoplaunch4b / 4c (-Level 4) as before.
- Round 152 (user, Caesar's Challenge level 1, userlogs xxl_coop.caesar_challenge_24_10.log, at its end: P2 Obelix's
  hits shown on P1 Asterix's HUD from his second hit on, P2's HUD frozen). The hits were Asterix's, not only shown:
  the trio's area hit (event 0x4C14, FUN_004829D0, case 0x14: a sphere x, y, z, r, the source +0x10, "hit" byte
  +0x14) is the mobile tower's shockwave (CKHkMobileTower 2,176, FUN_004E8DD0 from its vf30: 0x4E944C / 0x4E9667,
  3600 of them in the log); it calls the hit itself, FUN_00495FA0 (thiscall(hero, source, sphere, 0, 0), ret 0x10;
  the damage asked of the source with 0x0C0A: byte 0 points, dword 1 kind -- the tower answers from its parameters
  +0x54: +9, +4 & 7), on the leader in the sphere, then the companion, outside both heroes' sendEvents -- with the
  leader's stats in the game's place. Single player never sees it (no damage for an AI hero, +0x18 bit 0). Obelix
  stayed at 8 (healed to 9 later) while Asterix was drained to 0 with no fall noted: at 3830 s his HUD read "no
  health" and he died (sit 0E, the game's own game over: respawn at the checkpoint, 3837 s). Now FUN_00495FA0 is
  detoured (coop.cpp HitHook): the hit hero's own stats (IndivEnter), none for a fallen hero, and a fall from it
  noted (IndivHeroDown / NoteDeath, "a hit from 482AE4") unless the hero's own sendEvent is under way (it notes its
  own: g_inHeroEvent). A level's own tower stays in its pool with its parameters unset until its squad sends it
  (LV5's at 590.1 -441.6 -184.4; its 0x0C0A crashed, +0x54 null): the test step S_AREAHIT sends 0x4C14 at a hero
  with a stand-in source (answers 3 points). Autotests (-Level 2): towerhit (Asterix leads, ten shockwaves on Obelix
  5 units ahead: before A 15 -> 0, game over, O 15 kept; after O 15 -> 0, "Obelix is down", A 15 kept), down2a as
  before (Roman hit, "hero event 0300", revive). towerscan (a level's towers, Obelix by the nearest): LV3/4/6 none
  with a node, LV5 one.
- Round 153 (user's recording, screenshots Oct 27: P1's shields "jumping" -- the last of five shields drawn at
  ~800 px of 1920 beside the helmet counter (1985) for a few frames as the health row and the counter slid). It is
  xxl_widescreen2d.asi's plate rule (CloseRows): a HUD model whose unsqueezed 2D place lies in a stretched plate's
  unsqueezed rectangle (this frame's or the last one's) is moved with the plate's squeeze towards its anchor, and is
  no row's. hud2's layout puts the counter (plate +0x134, drawn leftwards from its place, width text + 0.075 -- it
  grows with the digits in state 2 and only shrinks when hidden) at 3/4 size in the top middle; at four digits its
  recorded left edge is 0.364 (960 x 540: plate 0.273 wide natively, 0.205 after hud2), and five shields fitted to
  0.28 by ScaleHealth put the last one's unsqueezed centre at 0.368 (the user's 0.361). The counter rests at y 0.815
  (native 0.915 - 0.1), under the shields' 0.90, and slides in / out from 0.975: passing the row (+-0.028), the last
  shield was "on" its plate -> 0.423 (the user's 800 px). An empty last shield is not drawn in the list, so only a
  part-full one jumps (the user's was "slightly damaged"). Now (widescreen2d.cpp CloseRows): a model on a plate with
  another model, on no plate, beside it on its line (kRowLine 0.03, kRowGap 0.15) is a row's end reaching onto the
  plate and stays the row's; the counter's helmet model (0.511) has no such neighbour (the off-plate shields are
  0.2+ away), so it still moves with its plate. Logged ("rows: a row's end ... stays the row's", 4 lines). The
  counter shows from its hidden state when the helmets (+0x68) differ from its shown count (itf +0x344), gm +0x5C
  bits 0x1000800, x3 time +0x98 > 0, or bit 6 (kept set by the game: an XOR poke toggles it). Autotest shieldjump
  (-Level 2: 1985 helmets poked, five shields, a hit then points back; slide-out ~6.2 s): before, frames s32-s34 the
  last shield beside the counter; after, in its row. hudreq / hudlayout (-Level 4) and shopsplit: the rule never
  fires, same pictures as Oct 22.
- Round 154 (user: a fallen hero's combo bar, full state and all, stays through the fall and the revive -- reset it
  on the defeat, individual heroes). The game's own respawn empties it (CKAsterixGameManager::vf13, 0x501940: gm +0xB4
  chain, +0xB0 hits, +0xC4, +0xA4 gauge = 0, +0xCC bits 9-10 level cleared, +0x160 bits 4 and 6 cleared); the full
  gauge's run-out in the gm tick (FUN_00504080) clears chain, hits, +0xC4, gauge and the stats' +0x10 bit 14. Now
  IndivHeroDown (individual.cpp) empties the fallen hero's Stats the same way -- hits, chain, c4, gauge, decay,
  level, full, barFlags (+0x160 bits 2 / 4) -- and commits them (logged "combo bar emptied (down)" when it had any).
  A game over is the game's respawn as before. Autotests (-Level 2): downcombo (Asterix, P1, full bar from 40 "hit
  landed", downed by area hits, revived by P2's grab: before, hits 38 / gauge 30 down and 19.9 after the revive --
  the "COMBO" bar still lit on the fallen hero; after, 0 / not full from the fall on, empty after the revive),
  downcomboo (Obelix, P2's HUD copy: 38 / full -> 0 / not full). A note from the first try: NEXTTO 5 units ahead of
  Obelix put Asterix in LV2's sea (swimming heroes take no hits) -- move Obelix instead.
- Round 155 (user, userlogs xxl_coop.combo_bar_full.log / _partial.log; shots XXL-Editor\screenshots
  Snipaste_2026-10-30_01-01-33 / 01-04-06: after round 154 the fallen hero's bar still showed full, running down,
  or kept one or two levels lit). The HUD's bar (FUN_00518190, interface +0x24 bits 12-14): full (state 4) it draws
  its own fill (+0x2E8 toward +0x2E0 = 0 at +0x2E4 = height / gm +0xA0 a second), reads the gauge only on gm +0x160
  bit 2, and while its "Combo" word plays (node +0xB4 shown) sets gm +0xA4 back to +0xA0; it leaves state 4 when the
  fill is 0 (hits, chain 0, FUN_0048C860 = level from the hits). Part full (state 2): a level's glow goes out only
  when the hits go under it with some left (+0x2C0 the last seen); hits 0 skip that. Now IndivHeroDown only clears
  chain / c4 / decay and sets g.drain; DrainCombo (IndivFrame, after RunDownCombo): full -> gauge down in 0.5 s with
  bit 2 each step, at 0 the run-out (hits, full off); else one hit a frame; then level 0. Autotests downcombo (full
  in two gives so the bar enters state 4: state 4 -> 2 0.44 s after the fall, hidden 5 s later), downcombop (25
  hits a second before the fall: drained in ~0.9 s, glows out, hidden ~6 s later). OPEN: (a) at the fall P1's bar
  sees hits 0 for one tick (combodbg: "seen 0 hits 19"), then the drain's 18 -> it replays its level-ups (bounce,
  white bursts) -- find who zeroes the hits in the fall's frame (the death's entry? the lead switch?); (b) after the
  full drain a white bottom segment stays lit (node +0xC0, the level marker shown by the state-4 bit-4 branch, not
  hidden by the exit?) -- compare with a full bar running out with no fall.
- Round 156 (round 155's OPEN items; user's pad test: both bars drain to 0, no white segment after a full one --
  (b) was a test artifact: 40 hits in one frame never put P1's bar in state 4 before the fall). (a) The level-up
  replay at the fall: in P1's interface tick (IndivEnter(P1 hero)) the health part's refresh (FUN_0051B550,
  ShieldsShownHook: a scope of its own) ended with IndivLeave -> Use(StandingOr(prev)) -- a fallen P1 hero's tick
  went on with the standing hero's stats, and the combo bar (FUN_00518190, after the health part in vf18 0x5180E0:
  0x51CD80, 0x51A140, 0x51C160, 0x51BA70, then the bar) saw Obelix's 0 hits (+0x2C0 = 0, the hits-0 path at
  0x5188FC), then the drain's 18: level-ups replayed (bounce bits 27-28, white bursts). Only in frames where the
  health part refreshes. Now IndivLeave restores the outer scope's hero exactly while depth > 0 (StandingOr only at
  depth 0). Also seen: [coop_difficulty] going APART at the fall rescales the fallen hero's copy (25 -> 19 hits,
  full at 38 -> 30) -- as designed. Autotests downcombop (seen 25 -> 19 -> 18 ..., no bounce), downcombo (state 4
  -> 2 0.47 s after the fall), down2a, towerhit, shopbuyo as before.
- Round 157 (user, Game Files\xxl_coop.catapult_shared_combo.log, LV3-1 at 106.0 1.0 11.5: player 2's Obelix, not
  leading, hitting the Roman catapult put the hits on both heroes' combo bars; Asterix not leading does the same;
  with the hitter leading it is fine). CKHkCatapult (2,130, vt 0x628238; Life CKHkCatapultLife 3,108, tick
  FUN_00508E70). Its sendEvent (0x508190) on a contact 0x0300 with its kind-3 volume (+0x19 == 3): two attacker
  slots (volumes +0x94 / +0x98, owner at +0x20; timers +0x30 / +0x34, a slot dropped at > 0.05, dt added at each
  tick's end); a new attacker takes the older slot, its timer 0, gets "a hit landed" (0x4C0E to the owner's
  sendEvent -- the hero's own scope) and +0x14 |= 0x42. The next tick with bit 0x40 runs FUN_005093A0: health
  +0x1E down, and the hit on the combo bar *again*, straight into the game manager (+0xB0 / +0xB4 += 1, or 3 under
  the potion; decay +0xA8 from +0x9C; FUN_0048C860; full: gauge +0xA4 += +0xA0 / +0xC8, Bits8 2) -- two hits a
  blow in single player. That tick ran with the leader's stats in place: one for the hitter, one for the leader.
  Now TickLife (proximity.cpp) ticks a catapult with bit 0x40 inside IndivEnter(hitter): the slot with the smaller
  timer, its volume's owner matched to a hero (CatapultHitter; log "catapult ... hit by X (not leading)", 20 max).
  Autotests catapultcombo (-Level 3; P2's Obelix by the catapult, 6 blows: before A 1 / O 1 a blow, after A 0 / O
  2) and catapultcomboa (-Level 3, XXL_P2HERO=asterix, Obelix leads via 4801: A 2 a blow, O 0). Same direct-add
  pattern, not looked at in co-op yet: CKHkJetPackRoman::vf28 (0x4E25A0) and CKHkMobileTower::vf28 (0x4EB170),
  Roman state code that runs in the squads' tick (the leader's stats, or the posing hero's slice).
- Round 158 (user, screenshots\attachments\Pasted image 20261026204437/38.png: LV1-2, P1's Asterix at 2.6 -7.4
  117.2, P2's Obelix at 24.4 -2.4 122.1 -- the helmets at -45.8 -14.4 140.0 came and went on P1's half as either
  camera turned; and Caesar's Challenge level 1: crates on Asterix's way to -257.7 29.8 609.2 came in only as he
  walked up, some as Obelix's camera turned). Two capacities.
  (1) Helmets. CKGrpBonusPool vf27 (0x54C380) frees every beacon bonus (+0x34) each frame and places them again
  from the free list +0x38 in the kluster query's order until the cap +0x2C (20 here) or the free list runs out;
  drops take from the same free list (shared vf29 0x54C790, thiscall(pool, arg), ret 4: +0x38 head -> +0x30,
  vf17 0x44 places it). Round 22's split cap (x 7/4 = 35 of 50) was too small: the two views saw up to 46 beacons
  and the far group (9 beacons, 7th in the list, 31 before it) got 4-8 (45 of 188 log samples). Now the split cap
  is 0xFFFF (the free list is the limit) and vf29 is hooked on both pool vtables (slot 29): in split, with the
  free list empty, it takes back the head of +0x34 (placed last = ranked last) that is not flying to a hero
  (vf19), freed the way vf27 does (vf22, onto +0x38), then spawns. Autotests helmets12 (-Level 1; XXL_KLOG=
  -45.8,140.0; new step P1R = player 1's right stick through the camera service's input view +0x580 remap, as
  CoopP2CameraValues): after, 188 of 188 samples 9 of 9; helmets12d (new S_POOLHOLD holds a pool's free bonuses
  as if in flight; P1's Asterix beats a beach Roman): "taken back" twice, the drop spawned, far group still 9.
  (2) Crates. CKCrateCpnt::vf13 (0x542AB0, thiscall(cpnt, handler, ?), ret 8; slot 13 of vt 0x6291E0) queries
  flags 7: loop 1 frees the objects of the outside klusters' beacons (live bit cleared, FUN_00544BC0/CE0 +
  FUN_00544B00), loop 2 gives free objects (CKHkCrate, list (cpnt +4)->vf12, next +4, beacon +0x58, -1 free) to
  the inside beacons nearest first and stops at the first it cannot serve. An object stays with its kluster while
  it is in any view: far crates in P2's view held objects that nearer ones in P1's waited for. Beacon bits: the
  beacon service (yp +0x78) +0x18 records (0x40 each; entry +0x13 the sector) have a bit array at +0x28; beacon
  i's bits at entry +0x11 * i + entry +0x16: three of pieces left (FUN_00542E40), +6 live (FUN_00542E90);
  FUN_00505260 resolves a descriptor (entry +0x13..+0x15, index). CC level 1: handler 3 has 26 objects, handler 4
  10; before, 0 of 26 free and 4-10 crates in view without an object for the whole run. Now (klusters.cpp, split
  query only) a crate query's sorted list is cut where the beacons holding or wanting an object reach the object
  count, the rest passed as outside (loop 1 frees theirs). Without hysteresis two klusters at the cut traded
  objects every frame while P2's camera swung after the warp (76 hand-overs that second, 4 without the cut), so:
  klusters with crates standing rank a quarter nearer (kCrateLiveRank 0.75), one handed objects is not cut for
  1.5 s (kCrateHoldMs), and the fair merge counts the crates' needy beacons instead of +0xC -> 24 (handler 4,
  untouched, 11-16 the same second). Bounded logs: "N in view without an object", "objects handed to another crate
  in the last second", "klusters past the N objects passed as outside". Autotest crates1cc (-Level 1 -GameDir
  "SPQR\Caesars Challenge" -NoDeploy after deploying to Game Files: the CC asi is the user's hardlink; P2's R3 + A
  warp, Asterix placed a step ahead, glide; S_CRATE b = -3 lists every crate object): a near crate by the river
  missing during the walk before, there after; the same at the target. crates44 (45 objects, no cut) unchanged.
- Round 159 (user, LV2-1: Asterix leading in sector 1's Sas half at -802.5 526.9 -970.3, player 2's Obelix through
  sector 2's half at -811.8 528.6 -970.5 and over the rectangles at -810.6 531.2 -985 -> -989: Obelix was brought
  back to Asterix; LV2-5: Obelix in the half that streams sector 5 at 97.6 2.5 -524.2, Asterix at 138.0 2.6 -564.7,
  R3 + A unloaded sector 5 and left both stuck). (1) CKLevel::vf17 (0x40FD10, ret 4) takes its Sas position from
  game manager vf28 -> CKGrpTrio::vf27 (leader's node frame +0x80) at 0x40FD9A (17 bytes: mov edx,[0x6621F4];
  mov ecx,[edx+8C]; mov eax,[ecx]; call [eax+70]), checks the current sector's Sas list (FUN_00410DA0, ECX =
  sector), then the loading one's, and requests a different answer (FUN_0040FE60: +0x34 = n, unload, load ->
  +0x2C; the loader's callback 0x40F9A0 sets +0x24, +0x34 = -2). Obelix's half did nothing, his zone 0839 asked for
  2, and the next frame Asterix's half asked for 1 (S_WATCHRW x = 8: writes to +0x34 with stacks; 40FE00 = vf17).
  Now that call site goes to SasPosition: in co-op the hero whose own tracked sector moved by a Sas box into a
  sector not streamed becomes g_sec.streamHero and vf17 takes its node frame; a zone switch makes its hero the one,
  an unknown switch the leader again (-1), as does P2 leaving. A left-behind hero is brought over in a Sas box too
  (before: never while inside one), and the "heroes within 8 units share a sector" rule skips heroes in Sas boxes
  (the halves are ~2 units apart: it gave Asterix sector 2 in his sector-1 half, his half then took the streaming
  back). (2) The detection service tests the node's world frame (node +4 -> +0x80, copied to its last positions
  +0x1C at the end of its tick); the mod's teleports wrote only Pos and frame +0x40, so the world frame showed the
  old spot for a tick and the jump crossed planes (LV2-5: 084B, which requests sector 4). SetHeroPosition now
  writes +0x80 too and forgets the move (ProximityForgetMove); R3 + A (WarpToPartner) goes through it, so do the
  sector catch-up, cutscene regroups and spy walk. Autotests sasnl21 (glided: a glided hero is held in the air,
  and when the old sector went out the game put Obelix back on his last ground in sector 1's half for ~17 frames,
  which asked for sector 1 once -- test artifact), sasnl21w (walked: one switch 1 -> 2 by Obelix's Sas, Asterix
  brought 1 s later), warp25 (before: 0908 + 084B, sector 4; after: no zone, sector 5 stays); sas, sectorzone,
  shopsas0 as before (shopsas0's switch now by Obelix's Sas instead of the rectangles).
- Round 160 (user: round 159's open item -- the put-backs -- and "does R3 + A / the sector catch-up check for solid
  ground, as the revive beside a fallen hero does?"). (1) individual.cpp PlaceHero (every put-back: zone and
  partner-spot put-backs, R3 + B's last solid ground, brought beside a fallen leader and back, a pit fall laid on
  its last ground, the revives at a launcher / a sector change) now writes node frame +0x80 as well as +0x40 and
  calls ProximityForgetMove itself (three callers never did; the five that did dropped their copies). Called from
  inside the detection tick (IndivBlockPartnerSpotFrom), the tick's later tests now see the put-back spot too.
  (2) No: R3 + A, the catch-up and the sector-change revive put the hero 1 unit above the partner -- over the pit
  when the partner was jumping it, and onto it at a ledge (the two push apart). New CoopJoinSpot (coop.cpp) ->
  IndivJoinSpot (individual.cpp): the partner on its feet (+0x384 & 7 == 0) on static ground (CGround 0x624258, not
  a death pit), one of its own safe spots / trail points 1-3 units away (SpotBesideFallen's choice); off its feet
  (no ground, jumping, swimming), its newest safe spot within 12 units; on a CDynamicGround (0x6241BC: lifters,
  platforms) or with no spot (individual heroes off: none are noted), 1 unit above it as before. A spot in a Sas
  half of the streamed sector that streams another sector is refused (the hero would take the streaming,
  round 159). Autotests warppit26 (-Level 2: Asterix over LV2-6's pit, Obelix R3 + A; before: put at 97.0 7.8,
  fell in, game over; after: on Asterix's edge spot 93.2 2.7 -391.1, revives him), warplift58 (-Level 5: Asterix on
  sector 8's lifter floor, CDynamicGround; Obelix lands on it with him, as before). As before: slidecross3
  (XXL_DOWNAT=1:1000, all put back short of z -903.82), reach26 (XXL_DOWNAT=0:5000), unlockpillar, warp25;
  sasnl21w's catch-up now lands 1.7 units beside Obelix on his own ground.
- Round 161 (user: in freedom mode the rotary beam's ride can end in a lock -- LV3-2, Asterix on the light pillar
  266.18 11.07 -97.09 (lit by the hearth 252.74 9.07 -90.54), Obelix hitting the turnstile 251.26 6.07 -97.85 until
  the beam drops Asterix; LV2-5 likewise; then add an option that locks the beam's pillar as with freedom off, off by
  default; and rename freedom). (1) The ride's hold (WatchFreedomBeam -> Hold modOwn after the player's jump) was let
  go only on "order done" (0) or "leads again"; the beam's drop (0x0C03 from 0x53F4C3, FUN_00481d50 case 3) leaves
  the rider order 3, so only a landing event freed him: LV4-2's landing zone sends 4800 + 4803 in the drop's frame,
  LV3-2's landing (247.7 7.1 -118.5, on a CDynamicGround) sends nothing (zone 0718 is a camera), LV2-5's sends 4814
  (AI order 1, a walk to a mark: 131.0 2.6 -457.6, then order 3). Now g_beamRider marks that hold and it ends when
  the order leaves 8 ("the rotary beam let go"); the landing's walk is then "not followed". (2) Freedom mode is now
  no-lock mode: [coop] nolock, nolock_except_corkscrews (was freedom_corkscrews: on = corkscrews still lock -- named
  "except" so on doesn't read as "no lock for corkscrews"), [coop_difficulty.experimental] nolock_romans; env
  XXL_NOLOCK / XXL_NOLOCKCORK / XXL_NOLOCKROMANS / XXL_NOLOCKBEAMS; freedom.cpp -> nolock.cpp; log lines "nolock:".
  The rounds above say "freedom" for it. (3) New nolock_except_rotarybeams (default off): at the 4802, a waiter in
  line with a rotary beam's arm over its circle is kept (g_keptLock "a rotary beam's circle", as for corkscrews).
  CKHkRotaryBeam: pivot = node frame +0x80; angles +0x28 now, +0x38 target, +0x4C rest / +0x50 over the circle /
  +0x54 far end; its arm points along (sin, cos) of the angle in x-z (vf15 state 5 compares the hang point's
  direction with them); 0x0903 to the beam (a turnstile hit) steps +0x38 by (next - this) / ((+8 >> 3) & 0x1F).
  RotaryBeamCircle: within 0.3 rad of +0x50, 1-60 units from the pivot, 10 in height. Measured: LV3-2 (pivot 246.5
  12.3 -98.5) 19.2 units / 0.09 rad, LV4-2 (-414.7 2.9 -118.7) 20.4 / 0.03, LV2-5 (133.5 4.7 -481.3; light pillar
  153.4 2.5 -484.4) 20.4 / 0.06, LV2-8 (47.5 58.9 -70.3; the pillar on the platform Obelix pulls under it) 50.1 /
  0.24. The beam then takes the waiting hero by itself (0x0C02 with order 3) and the landing's wait / walk is the
  level's, as with nolock off (LV3-2: held with order 3 after landing, exactly as nolock = off; LV2-5: walks to its
  mark, then waits). Autotests beam32 (-Level 3, XXL_P2HERO=asterix XXL_NOLOCK=1 XXL_LIFTLOG=1: before, Asterix
  held with order 3 after the drop and P2's walk did nothing; after, "let go", jumps and walks; XXL_NOLOCKBEAMS=1 and
  XXL_NOLOCK=0 give the same locked flow), beam25 (level 2: LV2-5's nearest hearth / light pillar; the beam turned by
  S_OBJEV 0x0903 -- its turnstile is up on the beam's pole, S_NEAROBJ put Obelix in the water). As before: beam42f
  (now let go at the drop, the landing's 4800 in the same frame, same landing spot), beam42l (leads again), beam28.
  XXL_LIFTLOG's car line is 1024 chars (the beams were cut off) and logs each beam's angles and Asterix's distance.
- Round 162 (user: LV4-9, player 2's Obelix on the boat at -165.213 5.424 -651.691 with player 2 on the keyboard --
  the boat's "Use" prompt showed the pad's button). The hero prompt FUN_0049A1A0 writes kind 9 ("Use", gm +0xCC bits
  4-7) only for a hero whose vtable +0x3C returns 0 -- Asterix -- whenever trio +0x48 holds a machine, so PromptHook
  always credited it to Asterix's player. The boat (CKHkBoat 0x627EB8; its offer, FUN_004FB1D0 from CKHkBoatLife::vf17: trio
  0xB00 = the leader's ground, hero +0x280 if hero +0x384 & 7 == 0 and not bit 0x20000000 (FUN_0049D560), compared
  with boat +0x120, the deck) ticks as its nearest hero's (proximity slicing), so it is offered to whoever stands on
  its deck, leading or not: trio 0x4901 (boat) / 0x4A01 (gun) with the machine as parameter -> trio +0x48, +0x3F4 bit
  10 / 11 (FUN_004823D0 / FUN_00482300 case 1; ignored while +0x3F4 bit 2). Now CoopNoteTrioEvent notes the machine
  and the leader byte at that event (g.machineOffer / machineOfferHero, log "coop: boat ... offered to X"), and
  PromptHook credits kind 9 to that hero; both heroes on the boat's deck -> both players' buttons ("X/F"). Either
  player's X still takes the machine (Obelix's through ServeMachineRequest), unchanged. The LV3-3 machine gun had
  the same misattribution (Obelix at the seat: "for player 1"; now player 2). Autotest iconsboat49 (-Level 4,
  XXL_ICONP2KB=1 XXL_ICONLOG=1): Obelix on the deck with Asterix 12 / 7 units off on the jetty -> "Use F", Asterix
  alone on it -> "Use X", both on it -> "Use X/F". Placing a hero at the user's y 5.4 sticks it in the hull: drop it
  from 7.5; the deck is small (1.6 units past the spot is water; z -649.5 is still the jetty). Regressions: iconsgun
  -L3 (now "Use F" for Obelix at the gun), boat49o (Obelix's X still boards: 4807). A one-frame "both" right after a
  teleport next to a machine is old (seen before the fix too).
- Round 163 (user, LV3-6's fight at -185.14 2.64 -225.10, userlogs xxl_coop.lv3-6_roman_spawns.log: a batch of Romans
  popped up at once in player 1's view, though round 22 keeps spawn markers in a player's view out). Cause: round 82's
  fallback -- with every marker of squad 4 (03BA4A04 in the log; markers -177.4 2.6 -238.3, -195.2 2.6 -219.3 and a
  third) in a view within 40 units (KlusterSphereInAnyView) vf38 got the marker farthest from both heroes anyway; the
  user's log has "3 of 3 spawn markers in a player's view" for 188-209 s. Now (user: keep the fallback as an option)
  [coop] romans_spawn_in_view, default off (env XXL_ROMANSINVIEW): off = the call is skipped and its Romans wait --
  member group +0x14 (asked) only goes down as vf38 places one, and vf37 calls vf38 every frame while it is > 0 (the
  game's own vf38 places nobody either when no marker suits its camera), so they come at the first call with a marker
  out of view; on = round 82's farthest marker. Vanilla's vf38 (0x52FB80) first picks a marker INSIDE its camera's cone
  (1.5 x FOV, > 2.5 units) whose facing (FUN_004622F0: chars +0xC yaw / +0xD pitch, 2 pi / 254, axes 0x662180 /
  0x6621E8 / 0x662138 filled at runtime) points at the camera (< -0.7), then any marker outside the cone -- single
  player does bring Romans in on screen at such markers; the co-op rule is stricter.
  Why round 82 needed the fallback (the wait alone stalled LV5-6's gate): the Twister counts Romans still to come as
  beaten -- CKGrpSquadEnemy slot 41 (0x4C8CB0, (count, group or -1, bonus)): +0x16 += n up to +0x18, event 0x4D09
  (one beaten, the gate counter) and a helmet each (write watch: 0x4C8D94 from 0x4984CE in the Twister handler); +0x16
  is "taken" (FUN_00530380 +1 per Roman placed, slot 41 per one beaten unseen), +0x11 alive, +0x14 asked, +0x15 to
  place at the reinforcement point, +0x18 to defeat (< 0 endless). vf37 drops +0x14/+0x15 once +0x16 >= +0x18. The
  squad's end (FUN_00530570 from vf31: event 0xD16 via vt+0x14 (thiscall, ret 8), byte +0x154 = 0xFE) runs only while
  the squad has members (vt slot 12 list) and all of them are dead -- beaten unseen with nobody there, it never ran.
  proximity.cpp EndSpentSquad (from EnemyTickHook, both paths): a squad whose Romans waited (NoteWaitedSquad, 8
  slots) that has nothing left to send / beat and no member -- and did not just lose its last one while spent (the
  game's own end, same tick: hadMembers) -- gets 0xD16 and +0x154 = 0xFE. Tried and dropped: a point behind the camera
  (FUN_00530060 via SpawnPointHook, as a one-off marker) when all markers are seen -- in a fight it lies by the other
  hero, in that player's view (always refused in fight36v). Autotests (-Level 3) fight36v (the user's spot, both
  fighting): XXL_ROMANSINVIEW=1 -> 6 Romans at -195.2 2.6 -219.3 in view at the fight's start; default -> none in view,
  waits up to ~22 s while both views keep the three markers, then the asked ones (up to 10) come together at a marker
  out of view; (-Level 5) gatedone / gatedonew (XXL_GATESHOTS=1; gatedonew logs the squad records and has
  S_WATCHRW x = 9, writes to "sent"): stalled before EndSpentSquad, now ended at 15.1 s by "the squad's end (0D16)"
  (fallback on: 18.9 s). Regressions lock34g (same counts as Oct 29), rules4n with XXL_DOWNAT=1:1600 (100, apart).
  Open: a fight can stand still while both cameras keep all of its markers in sight (the option's trade-off).
- Round 164 (user, after round 163, userlogs xxl_coop.lv5-6_marker28.log: LV5-6's fight, both players looking the same
  way at -119.4 9.6 35.9 -- no Romans in co-op from 29 s ("all 3 spawn markers in a player's view") until player 2
  left at 67 s; then single player brought them at marker 28 (-128.537 9.891 54.691), behind the rock plateau: in
  front of the camera but hidden). The levels hide markers behind rocks and walls and the game's vf38 prefers one in
  front of its camera facing it, so frustum + 40 units was too strict. Now a marker counts as seen by a view only with
  no collision wall in between: the game's wall sweep FUN_00449C90 (CKSrvCollision = yellow pages +0x50; thiscall
  (segment float[6] {start, delta}, height, result float[14], 0, gather 1), ret 0x14; returns 1 on a hit, result +0x24
  = hit fraction, 2 = none; with gather it runs FUN_00449730, which collects the walls (CGround vf19, slot +0x4C) whose
  boxes meet the segment's box in the current sector and sector 0 -- at most 40 a call, records 0x3C bytes at
  collector +0x14, count +0x18; walls with +0x38 set have a top, the others reach up for ever). The camera uses it
  (FUN_00457600 from CKSrvCamera::vf17), so do bombs and bonuses. proximity.cpp WallBetween (pieces of 8 units),
  SeesMarker (feet y+0.5 or head y+1.8; cached per marker and eye), MarkerSeen (klusters.cpp KlusterViewsSeeing: the
  eyes of the views with the sphere in their frustum within 40 units); the other view's cone check asks SeesMarker
  too. Log "spawn marker (...) in a player's view behind a wall". Autotest marker56 (-Level 5, the user's spot; before:
  Romans waited 24-39 s with all three "in view"; after: marker 28 behind a wall, no wait, 6-7 alive throughout,
  61 fps). fight36v: LV3-6's grass marker -195.2 2.6 -219.3 still counts as seen (no wall), a batch of 12 there came
  out of both views (shot 0.5 s later); gatedone ends at 22.3 s by the game's own end (Romans from marker 28).
  Limit: an infinite wall the camera looks over (a low ledge built as one) would count as hiding what is behind it.
- Round 165 (user, userlogs xxl_coop.lv3-6_nolock_on.log, nolock_romans on: Asterix teleported into LV3-6's fight in
  single player (gate 100 at 9.3 s), player 2 joined in the fight with Obelix left at the level's start -- the fight
  stayed 200 instead of the level's 100 while apart). The join doubles a fight under way (rules GAME -> ON, Raise);
  "apart" (difficulty_pathfinding) needed three noes 0.5 s apart, so it came ~1 s after the join: the gate's fight
  begins kGateSettleMs (2.5 s) after the gate takes its squads, and once begun a fight never drops for "apart".
  A join early in the settle window got "apart" before the fight began (Lower: 100 -- the user's good runs), a later
  one did not (the log: begun 11.797, apart 11.813). nolock_romans did nothing in that run (no "nolock:" lines).
  Now ApartFrame's first answer after player 2 joins (g_apart.fresh) counts at once and is asked without the 0.5 s
  wait: autotest join36 (-Level 3, XXL_NOLOCK=1 XXL_NOLOCKROMANS=1; Asterix into the fight in single player, player 2
  joins 1.5 s after the gate) -- before 100 -> 200 at the join, apart 1 s later; after apart 32 ms after the join,
  fight begins apart, 100 counting down. The 197 -> 200 the user saw at 19.8 s (logged as a jump up, nothing of the
  mod's logged) was not reproduced (join36 with a step out of the fight zone, grabs) -- user: not needed for now.
  Likeliest: squad vf34 (0x52F5A0, every listed Roman back to the pool, "sent" -1 each) -- beaten Romans stay in the
  pool's member chain (+8 head, +0x50 next) until all are down, so a reset un-counts them; who would reset a squad
  holding Asterix unknown. Bounded diagnostic kept: a jump up of the gate's count logs each pool (to defeat, sent,
  alive); XXL_GATELOG=1 logs every change that way (test aid). Enemy vf42 (0x4C81E0): alive = listed members with
  +0x24 bit 2 clear. The no-lock Romans' "in view" test is now the wall-aware one (MarkerSeen, public wrapper of
  proximity.cpp SpawnMarkerSeen; SeesMarker's cache keyed by position values, not pointers). Regressions rules4t
  (200), rules4l (leave: 194 -> 95), rules4d (XXL_DOWNAT=1:1000 XXL_UPAT=20000: 100 apart, scaled at the revive),
  freeromans23 (XXL_NOLOCK=1 XXL_NOLOCKROMANS=1 XXL_LIFTLOG=1: four Romans from Obelix's walk-to marker).
  Test lesson (user): with the potion Asterix knocks Romans out in one punch -- no stun, so no grab; grab tests without it.
- Round 166 (user: no-lock Romans seen stuck in the air -- wants other squads' spawn markers behind an option, default
  off). [coop_difficulty.experimental] nolock_romans_any_markers (default off; env XXL_NOLOCKROMANSANY; in the user's
  ini and dist). Off: nolock.cpp SquadAtOwnMarker -- a Roman of an active squad with Romans left to send (not +0x152
  0x2000) at one of that squad's own markers (within 40 units of the waiting hero, 6+ from both heroes, not seen --
  MarkerSeen --, PfReachable), the nearest of all squads'; none -> as before: the nearest active squad (SquadFor) at
  its own markers (SpawnPoint now takes the sender), else the hero's walk-to marker (AI target +0x568; unchanged,
  user's choice). On: the nearest squad at any squad's marker, as before. Log "from its squad's / another squad's
  spawn marker"; the spawn-marker tally counts "other squads'". Autotest nolockmark36 (-Level 3, XXL_NOLOCK=1
  XXL_NOLOCKROMANS=1; LV3-6's fight, the level's 4802 for Obelix sent by S_TRIOEV): the nearest squad #3 (box up to y
  35) has its three markers on the walls (not walkable to) and the fight's squad #4 its three on the ground -- off,
  before SquadAtOwnMarker: no Roman at all; any markers on: #3's Romans sent at #4's marker -195.2 2.6 -219.3 and vf55
  placed none (alive 0 -> 0: #3 has none free); off now: #4's at its own marker (queued, +0x14, as #4 is at its at-once
  number; they come at its markers out of view). freeromans23 unchanged (walk-to marker, no squad markers near).
- Round 167 (user, userlogs xxl_coop.nolock_romans_play*.log: no-lock Romans came one at a time; wants groups of 1-6,
  a 20-90 s pause (about 30) after a group is beaten, Romans that chase the hero out of their fight zone, the big
  Romans too, and no fight music from them; LV3-4 asked about -- the non-leader-only rule already covers it: only the
  hero not leading can wait, CoopNoLockWaiting, so the one in the locked fight never gets them). nolock.cpp SendGroup:
  1..6 Romans of one squad at one point, each from a pool picked at random by Romans left to send (endless 8, spent 1
  with +1 to defeat -- never in a squad a gate counts, +0x154 >= -1) among pools with a free Roman (CKGrpPoolSquad
  members: vf12 -> head, hooks chained +4) of a kind not kept (EnemyKindKeptAsIs = experimental's KeptAsIs); never
  queued (vf55 with an empty free list queues, +0x14: taken back). Before: always the first pool with Romans left ->
  never LV3-6's big ones (pool 1). Each Roman tracked (hook, squad and its index -- checked against GetLvlObject, a
  level change frees them) until it leaves its pool record's list (+8, chained +0x50) or its hook +8 bit 0 is set
  (finished: FUN_00530570 takes such members off with FUN_00530300, thiscall(member) ret 4 -- off the list, back to
  the pool, alive -1); the hero's last one gone -> next group after 20 + 70 u^3 s. NoLockSteer after each squad's tick
  (EnemyTickHook): a tracked Roman's slot (cpnt 0x46: +0 slot, +0x18 step, as SplitSquad) on a 2.5-unit ring round its
  hero, a downed hero too (they stay on its side); squads with tracked Romans are neither reset (vf35) nor freed
  (vf36). Music: the trio's fight mode (FUN_00481950) counts cached enemies (hooks) whose node +0x14 equals the frame
  count (short at 0x6464C4) within trio +0x400 -- FightHook gives tracked Romans' nodes another stamp around both its
  passes (NoLockHideFromFight); the user's log had "fight mode (music) started by Obelix" 0.7 s after a no-lock
  Roman. A gate counts a squad's count - sent + alive: Romans from the squad Asterix's gate counts kept it shut until
  Obelix beat his. Now a gated squad that is spent (FUN_00530570's test) with only tracked members alive is ended the
  game's way (EndFightsLeftToOurs: vt[5] = sendEvent 0x4C7E70, thiscall(ev, arg) ret 8, 0D16; +0x154 = -2); the
  game's 0D16 again as the last of them goes is dropped (CKGrpSquadEnemy vtable 0x6266B0 slot 5 hook) until 2 s
  after -- also for a spent squad whose end came already that sends a group. experimental UpdateSquads skips such a
  squad (NoLockEndedFight): the Raise at the heroes' meeting had doubled its tracked Romans as left to defeat (1 -> 4).
  No groups for a downed hero (IndivFallen: one went for downed Asterix). The walk-to marker fallback needs
  PfReachable too. LV3-4: lock sphere 0x578's 4821 sends Obelix to marker 121.4 0.4 -328.1 in the library; the
  library's lever (lever 4, 115.9 -1.3 -321.5, events 0210) lifts the wait (order 0). Romans in the library hit
  Obelix there, outside the courtyard squad's fight zone (the test potion had hidden it: the gauge is shared,
  obelix_potion). Test aids: S_ENDFIGHT -- ENDFIGHT (gated squads put away, spent, 0D16), ENDFIGHTKEEP(hero) (members
  away from the hero taken off with FUN_00530300, spent; the end left to the game / mod); S_PULLLEVER a < 0 = the
  lever nearest (x, y, z) (PULLLEVERAT); env XXL_OBELIXPOTION. Autotests: nolock34 (-Level 3, XXL_NOLOCK=1
  XXL_NOLOCKROMANS=1 XXL_OBELIXPOTION=0): 5-6 Romans of mixed pools at the library marker, Obelix hurt 15 -> 0, the
  fight ended for Asterix with them out, the lever lifts the wait, they stay round downed Obelix, the second 0D16
  dropped; freeromans23 (2-4 Romans, the keg lifts the wait); nolockmark36 now sends none (the fight's squad #4 has no
  free Roman, all at its at-once number -- before, one was queued).
- Round 168 (user, seeing round 167's no-lock Romans stay by downed Obelix: wanted in all play -- the Romans after a
  hero in a fight stay near it once it is down). guard.cpp: as a hero goes down (individual heroes, IndivFallen;
  GuardFrame from CoopFrame) the members of active squads within 10 units of it become its guards (16 max; each
  checked as no-lock's: in its pool record's list, hook +8 bit 0 clear, the squad still GetLvlObject's) until it is
  revived, they are beaten or put away. With a hero down the lead goes to the standing one and EnemyTickHook takes its
  plain path (the non-leader HeroOut: the game's CKGrpEnemyLife tick) -- every squad runs for the leader and its
  slots took everyone over to it. GuardSteer after each squad's tick (both EnemyTickHook paths; NoLockSteer now in
  the plain path too): a guard's slot (cpnt 0x46) on a 3-unit ring round the downed hero. They leave the downed hero
  alone (out of the objects' slicing) and go for the one who comes to revive it. Squads put away out of view (vf35,
  0x52F6D0, thiscall() ret: vf34 once the box is out of the game camera's frustum) are kept while guards or no-lock
  Romans of theirs are out: CKGrpSquadEnemy vtable 0x6266B0 slot 35 hook (replaces round 167's branch in
  EnemyTickHook, which missed the plain path); SquadFreeHook (vf36) keeps them too. Test lesson: a hero walked out of
  LV3-6's fight zone (z < -234.8) had squad 4 put 9 of its 13 Romans away (flags 06B7 -> 0602, target 0) -- glide
  inside the zone. Autotest guard36 (-Level 3, XXL_DOWNAT=1:20000 XXL_OBELIXPOTION=0; sq36c, Obelix down, Asterix to
  the zone's far corner -192 -211): 7 guards stay 3-4 units round Obelix to the end, the other 6 go with Asterix;
  with GuardFrame off (a local build): all 13 at Asterix's formation, none by Obelix. rules4d (XXL_DOWNAT=1:1000
  XXL_UPAT=20000): 1 guard; the gate scaled at the revive (85 -> 170) as before.
- Round 169 (user, userlogs 20261031-lv34-xxl_coop*.log: LV3-4 with the heroes apart -- Obelix got a Roman of
  Asterix's fight; "who knows where those Romans can spawn from"). nolock.cpp: the courtyard squad (#5, pools 1, 1,
  2, 6, 40, 10) was gated by sphere 0x578 (0D17, +0x154 -1) as Obelix's wait (4821) began, and SquadFor's nearest
  squad that can send was that same squad: 1 Roman of pool 4 at the library marker. Now squads a gate has taken
  this level are never used (NoteGatedSquads each frame: +0x154 >= -1 seen once -> remembered by squad index until
  the level number changes; -2 is both "never gated" and "its fight over", so the past has to be kept); SquadFor
  and SquadAtOwnMarker skip them. LV3-4 has no other squad awake by the library -> no Romans there for Obelix
  ("no active squad with Romans to send"); EndFightsLeftToOurs stays (a squad gated after it sent Romans). The
  user's "Asterix alone should have had 30, not 60": two fights -- the library's own (2 squads 03BE1D7C 16+9,
  03BE1EE0 4+1 = 30, the user's earlier session teleported into the library) and the courtyard's via 0x578 (one
  squad, 60 in the level file: kept at 60 with the heroes apart, 118 together) -- the 60 was the level's number,
  the Roman sent did not change it (sent +1, alive +1). Autotests nolock34 (-Level 3, XXL_NOLOCK=1
  XXL_NOLOCKROMANS=1 XXL_OBELIXPOTION=0, any markers 0 and 1): "squad #5 is a gated fight's" at the gate, no Roman
  for Obelix; freeromans23: a group of 4 at the walk-to marker, the keg lifts the wait (unchanged).
- Round 170 (user, after round 168: guards closer (2-3 units), at most 5, a shield for guards without one, a hit
  that lands makes a guard "mad" (it breaks off), the last guard goes for the standing hero, never lions, pirates,
  leaders, archers, jumping / jetpack Romans or turtles; an ini option, on by default). guard.cpp:
  - Kinds: only CKBasicEnemyCpnt (pool record +4 vtable 0x627BD0: leaders 0x627C30, archers 0x627CF0, jumping
    0x627CC0, jetpack 0x627C90, rocket 0x627C60, tower 0x627D20, turtles 0x627D50.. are other classes) and no model
    texture with "lion", "pirate" or "normand". Lions and pirates are CKBasicEnemyCpnt like Romans (LV5's pirates
    have the regular Roman's component values, flags 03007EA9 health 3): the model's textures tell them apart.
    Path: hook +0x10 node (CAnimatedClone 0x6230D4), node +0x54 its team player (RpTeam, 0xF8 each, the clone
    manager +4 team's +0x58 array; CCloneManager::deserialize 0x422AA0 sets CClone +0x24 bounding sphere / +0x28
    player and CAnimatedClone +0x50 / +0x54, +0x28 the skeleton = FUN_00574000(player +0xD8 frame), its +4 bone
    count); player +0xD0 the team dictionary (+0xC slots, +0x50 -> +4 elements 0x24 each, +0 the atomic, +0x1C
    flags; +0x54 per-element visible counts), +0xD4 an element index per slot (-1 none); atomic +0x18 geometry (+0x20
    materials, +0x24 count); material +0 texture, name +0x10. Names seen (the level files' strings): Romans romnl01 /
    rmnar01 (archer, romar_arc) / rmspe* / rompo01 / romaq01 / romjp01 (jetpack), lions lion01 + spec1_t_lion (31
    bones; Romans 28, archers 29), the tamer dompt01 (a CKBasicEnemyLeaderCpnt), pirates pirate01 (26 bones), their
    leader barberousse, centurion cntnl_01, Norman villagers villageois_normand01 (LV2), tower marteau*. The user's
    "LV2-1 squad 7, 15 lions" is the game's level 3 (LVL003): squad #7 pool 1, the only lion textures besides LVL006.
    The level files also hold the sounds' source paths (...\ennemis\lion\lion_attaque1.wav; pirates use "base").
  - Shield: the component is shared by the pool's Romans (one per squad pool record), so a guard's hook +0x54 is
    swapped for a copy of its own (0x180 bytes, CKBasicEnemyCpnt deserialized to +0x174; copy +0x60 = copy +0x10,
    the move parameters' pointer the deserialize sets) with +4 |= 0x10 (Has shield) and shield points (+0x110) 2 if
    0; put back as it stops guarding (FUN_00530380 sets +0x54 from the record as a Roman is sent, FUN_00530460 clears
    it as it goes back to its pool; records are found by list membership, never by +0x54). The hit code (Class_2_84,
    0x4CF990's switch on hook +8 bits 1-4, the hit kind: 0 front, 1 behind, set by sendEvent 3,0 from the hitter's
    side): kind 0 with +4 & 0x10 and the reaction (+0x1A4) not 1..5 counts hits in a row (+0x148); up to +0x110
    blocks (+0x24 |= 0x10000: state 6, cover, reaction 0x1B then 0x1C-0x20), beyond +0x110 + +0x111 only with +4 &
    0x40 (infinite); the count drops back as cover ends (~0.5 s). Hit landed: state (+0x21) 2 (or 1: stunned,
    seized, flying +0x24 0x402) with a hurt reaction below 0x1B (0x02, 0x03, 0x05; 0x07 flying) -> "mad": dropped,
    its own component back. From behind (kind 1) a hit lands at once.
  - 5 guards per hero (nearest first; fewer than 2 near: none -- a lone one goes for the standing hero), ring 0.8
    (they stand 1.5-2.8 off in the open; LV3-6's walled corner kept them at 2-3.3), the last guard of a hero dropped.
    [coop] romans_guard_downed (default on). Test aid XXL_GUARDMAX (guards per hero). Diagnostics (bounded): guards'
    distances every 2 s, their hit state as it changes. Autotest steps: S_POOLKINDS (every squad pool: component,
    flags, shield points, a Roman's bones and textures, may it guard), S_NEAROBJ buttons = nearest to hero
    buttons - 1 (pooled hidden Romans skipped, quiet). Autotests guard41 (-Level 4, XXL_DOWNAT=1:13000
    XXL_OBELIXPOTION=0: both heroes out where LV4-1's lock fight Romans come in, Obelix down there, Asterix faces the
    Roman nearest Obelix and hits, single hits then 4-hit combos): 5 guards 1.5-3.4 off, single front hits blocked,
    three broke off on hits landing from behind, one blocked 2 of a combo and broke off on the 3rd, the last went for
    Asterix; with XXL_GUARDMAX=2 the first hit landed and the other went for Asterix at once. poolkinds -Level 3 / 5
    / 2: lions, the tamer, archers, turtles; pirates, jetpack, tower, leaders; Norman villagers, archers, leaders,
    turtles -- no; the Roman pools -- yes. rules4d: 1 Roman near (none guard), the gate 85 -> 170 as before.
- Round 171 (user, userlogs 20261031-xxl_coop.guards_too_close.log: round 170's guards 1.1-1.7 off downed Obelix,
  walking all the time -- the ring 0.8 put their places inside its body; shields, "mad" and the last guard confirmed on
  the pad; asked: 1 unit farther, a spot a guard can't stand on lost rather than moved, and a range round the downed
  hero in which the guards go for the standing hero, back to their spots once it is out). guard.cpp: 5 spots on a
  1.8-unit ring fixed as the hero falls ((dx, dz) per guard, nearest free spot each), each one PfReachable from the
  hero (PF_YES; no graph there: all) -- a lost spot is one guard fewer (Spots; spots26 -Level 2: 4.5 units along
  LV2-6's pit edge from Obelix's spot 3 of 5, the two east ones lost). Engage: the standing hero within 5 units of the
  downed one (1.8 + a basic Roman's range beRange +0x14C, 2.8) -> GuardSteer leaves the guards to their squads' own
  slots (they fight it), out past 6.5 -> back to their spots (log "its guards fight it" / "go back"). Not causes:
  the squad's +0xC0 (3.0 in LV3-6 and LV4-1 alike: the step's slow-down radius; +0xB8 speed, +0xBC turn, +0xC4 0.5)
  and the component's "position accuracy" (+0x4C, the editor's attackCooldown, 2.0; 0.5 in a guard's copy changed
  nothing) -- LV3-6's corner guards stood 2.4-3.9 off because the fight lock's dome round Obelix kept them out (the
  graph doesn't know it); they stand still there. Test step S_GUARDSPOTS (GuardSpots export). Autotest guard41 (-Level
  4, XXL_DOWNAT=1:13000 XXL_OBELIXPOTION=0) now in three parts: Asterix 12 units off (guards 1.4-2.6 off, standing:
  the same distances 6 s on), beside Obelix hitting (they fight him: "fighting", 1.6-4.3), 15 units off (back,
  1.5-2.3).
- Round 172 (user, after round 169's LV3-4 gave Obelix no Romans: use squads whose fight was won earlier in the level
  -- "revenge"). nolock.cpp: Completed(sq) = not gated now (+0x154 < -1), spent (SquadSpent), no member alive but
  ours, a pool beaten to its count (count > 0, sent >= count). A gated squad is skipped only until it is Completed
  (GatedFight). SquadCanSend takes an asleep squad (+0x152 bit 1 clear) if Completed; SendGroup wakes it with vf33
  (0x52EF70, thiscall(): with nothing to place, its set-up, the flag, event 0D13 -- what a hero coming back into its
  box does) -- the flag alone (no set-up) had the Romans sent off in its first tick; such a squad is put away again
  (vf34 0x52F5A0: flag off, members back with sent -1 each, alive += queued + fallen, no events) 2 s after its last
  one goes (Ended.woke). +0x154 is the squad's pool phase: 0..7 one pool, -1 all (0D17, "add all pools"), -2 none /
  ended (0D16, FUN_00530570; vf32). Found on the way: a squad's Romans out of its box (x/z: max +0x11C, min +0x128)
  are knocked out as they come in -- squad #19 (box at the level's start, z -1.7..58.6) woken for Obelix 380 units
  off: its Romans got state 1 (+0x24 bit 1, the 4C02 path: Class_2_84 vf29 state 1 -> vf42 death sound, 0x4D09
  "a Roman beaten" to the squad) and hook +8 bit 0 in their first tick, at their marker. So every sender must have
  the waiting hero in its box (SquadFor, SquadAtOwnMarker) and the spawn point too (SpawnPoint: markers / walk-to
  marker outside the sender's box left out, tallied); round 167's chase "anywhere" holds within the squad's box (the
  courtyard's is 147 x 120). LV3-4: only the courtyard squad's box holds the library (SQUADS r 0.1); the user's
  "library fight of 30" was by the level's start (the teleport's log line had x moved, z not yet). ini texts:
  nolock_romans (revenge, the squad's area), romans_guard_downed (round 171's spots and range). Test step
  ENDFIGHTWON (S_ENDFIGHT b 2: every member taken off as beaten -- FUN_00530300, sent kept --, count = sent, 0D16,
  +0x154 -2). Autotest nolock34r (-Level 3, XXL_NOLOCK=1 XXL_NOLOCKROMANS=1 XXL_OBELIXPOTION=0): none for Obelix
  while the courtyard fight lasts; won at 20 s and put away (SQUADRESET) -- the game wakes it again (Obelix in its
  box) and 3-4 of its Romans come at the library marker and stay by Obelix to the end. freeromans23: a group of 5
  (squad #36, its box holds Obelix).
- Round 173 (user's pad test of rounds 171-172, userlogs 20261031-0525-xxl_coop.log, LV2-1: the range is right, but
  guards fighting the standing hero kept their squads' distance from it as it moved -- 4.4-7.2 off downed Obelix --
  so they drifted off the downed hero (LV4-1 too); once a guard walked to its spot for good (the same log: one 5.6
  off Obelix, 3.8 off its spot, for 10 s). guard.cpp: engaged, GuardSteer puts a guard's slot 1.5 units off the
  standing hero on the guard's side, kept within kEngage (5) of the downed hero (the squad's own slots ring the
  hero it runs for at its formation's distance). Unstick: a guard 0.4 units or more off its spot that has not got
  0.2 nearer it in 2 s (not engaged, not reacting: +0x1A4 below 0x22) takes the place it stands on as its spot
  (log). +0x1A4 as seen: 0x22 / 0x23 standing, 0x24-0x2A walking and turning (walking on the spot against the
  downed hero's body: 0x27-0x29; a Roman walks even for a small step, or slides along what it presses on, so "not
  nearer" rather than "not moved"). Diagnostics: the distances line has each guard's distance to its spot and
  [reaction], engaged its distance to the standing hero. Test aids: XXL_GUARDRING (the ring, 0.8 = round 170's:
  spots in the downed hero's body), GLIDEBY (S_GLIDE b -2: a glide to (x, z) off the other hero). Autotests:
  guard41m (-Level 4, XXL_DOWNAT=1:13000 XXL_OBELIXPOTION=0: Asterix 3 units from downed Obelix, round him at 3.5,
  5.5 off, then 14): fighting guards 1.1-3.3 off Obelix and 0.3-4.9 from Asterix; at 14, back at their spots,
  standing [22]. With XXL_GUARDRING=0.8: four walked into Obelix ([27]-[29]) and took their places 2 s after the
  fall, then 0.1-0.4 off their spots, [22] / [23]. guard41: shields block, a hit landed (mad); the rest as before.
- Round 174 (user: revenge from any won squad with its zone moved over the waiting hero's reach, back as the heroes
  reunite -- its Romans vanish, fine -- or once they are beaten, the next squad picked anew for variety; no revenge
  from a squad for 20-90 s after its fight is won -- LV3-4's courtyard Romans at the library the moment it was won
  felt off; LV3-6's no-lock Romans "spawning behind a wall, dying over and over", userlogs
  20261031-xxl_coop.lv3-6_romans_spawning_behind_wall.log). nolock.cpp:
  - The Roman's tick (Shared_Class_2_84_vf23 0x4CEE00) tests hook +0x2C against the squad's box in x, y and z (max
    +0x11C.., min +0x128..): out with its last place (+0x38) inside -> put back there, +0x24 bit 14 (also for a place
    off the enemies' pathfinding cells: FUN_0045caa0 / FUN_0045c560; with bit 14 vf49 adds no movement); out with
    +0x38 out too -> +0x24 bit 1, knocked out. InBox tests y too.
  - LV3-6: not reproduced as such (nolock36w / nolock36x, any markers on, XXL_NOLOCKCORK=0 so 4802 makes Obelix
    wait: squad #3's Romans come in at (-177.5 2.6 -227.1) / the user's (-195.2 2.6 -219.3) and reach Obelix). The
    user's Obelix went east to x -116 for 30 s, out of squad #3's box (x -200..-163): nolock36e (Obelix glides to
    -152 -235.6 and back) had its Romans pile up at x -163.0, held back over and over (bit 14 on, off, on).
    NoLockSteer keeps their slots 0.5 units inside the sender's box (x / z): they wait at its edge, no hold-backs, and
    come on as he comes back. Asked the user for a screenshot if it happens again.
  - NoteWonSquads (every 0.5 s): a squad turning Completed gets a pause (PauseMs, 20-90 s; log "may come back for a
    waiting hero in N s"), CooledDown after it. SquadFor(at, idx, left): with Romans left, else spent and not
    Completed. Revenge(hero): any squad in the level Completed, cooled down, SquadCanSend, not brought over for the
    other hero, not one waking would start over -- at random, another than the hero's last one when there is
    another. vf33 (0x52EF70): with +0x13C (the level's come-back time, 300 s for LV3's squads) 0 or more and +0x138
    (time asleep, zeroed by FUN_0052d4c0 at the wake) at least that, every pool goes back (sent 0, alive = its
    starting count +0x10) and is placed; else alive += queued + fallen and the alive count is placed per pool (what
    can't be, queued) -- a squad asleep with its starting Romans counted alive placed those, queued 4, and vf55 had
    none left to send ("placed no Roman" log: squad flags, phase, pool counts, free Romans).
  - A revenge squad whose box does not hold the hero is brought over (MoveBox): its box = PfReachBox(hero, 60) plus
    2 in x / z, -3 / +6 in y (Romans stand on the nodes' tops, jump, fly when hit); grown every 1 s round where the
    hero is (GrowBox); its own box back (RestoreBox) once none of its Romans are out for the hero, when the wait
    ends (those still out are knocked out at once), with no-lock Romans off / player 2 gone; forgotten on a level
    change. It may use any squad's spawn markers (SpawnPoint anyMarkers) or the walk-to marker. pathfind.cpp
    PfReachBox: the box round the cells flooded from a place within a radius, the nodes' heights.
  - Diagnostics: WatchOut (a Roman out for a hero: +0x24 bits 14 / 1 as they change, its squad's box; cap 40);
    DumpAround lists each squad's markers within 40 units (in view, walkable to) and its box's y range.
  - Test aids: XXL_NOLOCKPAUSE (ms: the pause between groups and after a win), SQUADWON(t, i) (S_ENDFIGHT b 3:
    squad #i's fight won wherever it is -- members taken off as beaten, alive / queued / fallen 0, sent = count, at
    least 1, +0x154 -2, no 0D16; logs +0x13C / +0x138), S_SQUADS shows the box's y.
  - Autotests: nolock34m (-Level 3, XXL_NOLOCK=1 XXL_NOLOCKROMANS=1 XXL_OBELIXPOTION=0 XXL_NOLOCKPAUSE=3000):
    squad #19 (the level's start, box x -18..109 z -2..59) won at 3.5 s; Obelix waits in the library: 6 of #19's
    Romans brought over (its box x 115.5..184 y -5.4..40.5 z -356.5..-317.8), at the walk-to marker, fighting him;
    taken off -> its box back, the next 2 from #19 again; the courtyard (#5) won -> "in 3 s"; #19's taken off -> 3
    from the courtyard (no move: its box holds the library); those taken off -> 2 from #19 brought over; Asterix
    pulls the lever -> the wait over, #19's box back, its 2 Romans knocked out at once. nolock34r (no pause aid): the
    courtyard won -> "in 23 s", none came to the run's end (round 172: at once). freeromans23: 1 from squad #36.
- Round 175 (user's pad test of rounds 173-174: the guards are fun, no backing off; asked for the stuck rule at 1 s;
  userlogs 20261031-xxl_coop.lv4-1_inf_walk.log, LV4-1: at the end a guard walked on the spot for good against
  downed Asterix's body, its spot behind it). guard.cpp Unstick: a guard walking (+0x1A4 0x24 on) that has not got
  0.2 units nearer its spot in 1 s takes the place it stands on as its spot. Reset by: progress, at its spot (0.1),
  engaged, reacting (below 0x22), more than 6.5 units from the downed hero, the standing hero within 4 units of it
  (kBusy: a Roman goes for a hero near it on its own, facing it, 0x2A). Standing a moment between (0x22 / 0x23)
  does not reset it. Round 173's rule reset on every frame a guard was 0.4 off its spot or less: the one pressed on
  the body slid in and out of that (15 re-spots in the log, none at the end). The same log had guards re-spotted
  at 6.1, 7.9 and 8.4 units from Asterix -- after Obelix (a hit landed on one there). Test aid XXL_GUARDTRACE: the
  distances line every 15 frames (cap 400). Autotest guard41a (-Level 4, XXL_DOWNAT=0:13000 XXL_OBELIXPOTION=0
  XXL_GUARDTRACE=1): Asterix down among LV4-1's lock fight Romans -- three guards walking against his body (0x2A,
  0.5-1.5 off their spots, 1.3-2.8 from him) stop 1 s after the fall and stand (0x22); Obelix 3 off and round him
  (they fight him), 7.5 off (the first run, without kBusy: four stopped 3.0-4.7 from Asterix facing Obelix; with it
  none, back at their spots 1.1-2.8 from him, standing), 14 off. guard41m with XXL_GUARDRING=0.8: five stop 1 s
  after the fall (round 173: 2 s), then stand.
- Round 176 (user: LV3-6's squad #3 -- its Romans come once the keg at (-183.8 5.5 -207.5) blows, in single player
  (userlogs 20261031-xxl_coop.lv3-6_sp.log) and with nolock_romans off (_nolock_off_squad3.log) -- was there
  before it with nolock_romans on (_nolock_squad3.log), one Roman "dying out of nowhere"; no jumping / jetpack
  Romans, mobile towers or turtles for no-lock). Squad #3 before the keg: awake (Obelix in its box), gate -2, one
  pool [alive 0, sent 0 of 60], at once 0/0 (none placed by the level): its script sends its Romans in at its
  markers up on the walls once the keg blows, and its gate takes it then. nolock.cpp NotBegun(sq): +0x154 -2 and
  every pool's sent +0x16 and alive +0x11 0 -> SquadCanSend false (log once per squad, cap 20; DumpAround tags
  "(not begun)"). The user's log: 6 of #3's sent at #4's marker (-177.5 2.6 -227.1) at 84.8 s, beaten by Obelix
  (+0x24 bit 1); one was put back by #3's script at its own marker (-183.3 5.7 -204.9) 0.1 s later, still taken
  for ours (in its pool record's list, hook +8 bit 0 clear) and steered, knocked out up there 10 s on. StillOut:
  +0x24 bit 1 counts as gone (WatchOut now runs before Prune so it still logs it; the line says "beaten, out of its
  squad's box or on a deadly floor": the tick's last branch -- hook +0xB8 the floor, its +0x2A bit 2 -- sets bit 1
  too). FreeRomans: no CKJumpingRomanCpnt (0x627CC0) pools either (jetpack, mobile tower and the four turtles were
  out already: KeptAsIs). Autotest nolock36w (-Level 3, XXL_NOLOCK=1 XXL_NOLOCKROMANS=1 XXL_NOLOCKROMANSANY=1
  XXL_OBELIXPOTION=0 XXL_NOLOCKPAUSE=4000 XXL_NOLOCKCORK=0): before, 4 of #3's at #4's marker; now "#3's fight has
  not begun", 3 of #29's (the pool of the 3 spear Romans the level places on the ledge: begun) at #4's marker,
  round Obelix. nolock34m as in round 174. ini text for nolock_romans.
- Round 177 (user's "strange" LV3-6 run, userlogs 20261031-xxl_coop.lv3-6_nolock_off_strange.log: a sector jump at
  9 s left Obelix at the level's start with sector 1 unloaded -- "everyone moved along" -- and Asterix by the keg;
  squad #4's fight began but its Romans did not come until Obelix was brought over at 51 s). Squads #18 / #19 (the
  level's start; #19 shares a pool with #4) were awake for Obelix in their boxes, their Romans out. Squad #4 coming
  alive (vf33 0x52EF70; with the pool's free list empty it calls vf36 0x52F850, which puts away -- vf34 -- every
  awake squad of its group whose box does not hold the camera's look-at) found the pool dry, and proximity.cpp
  SquadFreeHook kept every squad with a hero in its box (LV5-8's squads round Obelix freed each other every frame):
  #4 placed 3 of its starting Romans, and the 11 it sent in next waited (+0x14 asked 8 / 3) for good. Now, for a
  squad coming alive round the leader only (not g_squadForNonLeader -- EnemyTickHook's non-leader pass swaps the
  trio's leader, so t.Leader() is the other hero there: the first try had the two sides freeing each other for 4
  frames), a squad with only the other hero in its box, not fighting (ProximityHeroFighting), is put away (log, cap
  20) and held back (HoldBack): EnemyTickHook's non-leader pass does not run its vf31 while it is asleep -- a hero in
  an asleep squad's box wakes it the next frame, and it took the pool's Romans straight back before the fight's next
  ones were sent in (second try: 4 + 1 out, 6 + 3 asked for good) -- until the squad it was put away for is asleep,
  gone or has nothing left to send (MoreToSend: a pool endless, sent short of its count, or some asked), or that
  hero fights (log "may come alive ... again", cap 20). Never the other way round. Autotest pool36 (-Level 3):
  sector 6 loaded, Obelix left at the start (#18 / #19 awake for him), Asterix taken into #4's fight: before, #4 2 +
  1 out and asked 8 / 3 to the end; now #18 / #19 put away and asleep, #4 10 + 3 out, 1 asked (a marker in view).
- Round 178 (user's pad test of rounds 175-177: guards good, LV3-6's squad #3 no longer early, Romans keep coming
  while a hero stands idle; userlogs 20261031-xxl_coop.lv3-6_wierd_squad.log: LV3-6's fight won, then Romans of
  "an unknown squad" -- the ledge squad #29, never fought, at 2 s into Obelix's wait, while #4's 21 s pause after
  its win ran -- walked backwards. Asked: revenge first, a nearby squad not yet beaten only as a fallback; the first
  group 5-25 s after the wait begins). nolock.cpp SendGroup: Revenge (brought over if need be) first; with none
  cooled down, RevengeSoon (a won squad still in its pause that could send then, as Revenge takes them) -> w.next
  at the end of the shortest pause (log "Romans for X from squad #N's won fight in S s", 3 a wait); only with no
  won fight's Romans to come: SquadAtOwnMarker, SquadFor(left), SquadFor(spent). FirstMs: the first try 5000 +
  Random(20001) ms after the wait begins (kFirstMs 2 s before; logged with "would be kept waiting"); test aid
  XXL_NOLOCKFIRST (ms). Autotest nolock36w (XXL_NOLOCKPAUSE=20000 XXL_NOLOCKFIRST=4000): #4 won at 13.3 s -> at
  18.4 "from squad #4's won fight in 15 s", 4 of #4's Romans at 33.3 s (before: #29's at 16.4). freeromans23 (no
  aids): "the first in 24 s" (the test's wait ends at 35.8: none came). ini text for nolock_romans.
- Round 179 (the same log: #29's spear Romans walked backwards after Obelix). The squad's tick (vf31 ->
  FUN_0052B670) gives each Roman a slot (+0) and a facing (+0xC, the formation's slot facing turned by the
  formation's frame: a direction, unit, y 0) as well as the step (+0x18); NoLockSteer moved the slot and step round
  the waiting hero and left the facing: #29 is run for Asterix on the ledge (in its box; EnemyTickHook's leader
  pass), so its Romans faced as their formation round him while following Obelix below. NoLockSteer sets the facing
  to the direction from the Roman to its hero (x / z). Diagnostics: once a Roman, the squad's facing more than 90
  degrees off the hero, logged with how the Roman faces it then (its frame +0x70, forward; cap 20). Autotest
  nolock36f (nolock36w's env, XXL_NOLOCKFIRST=2000): the fight below ended, not won (ENDFIGHT) -- no revenge to come
  -- so #29 sends its spear Romans at #4's marker; Obelix walks round as the user's did. Built without the facing:
  their squad's facing 90-99 deg off Obelix, they faced 59-90 deg off him, screenshots with them facing the camera
  while walking to his side; with it they face him as they follow and stand round him. nolock36w: #4's revenge
  Romans had their squad's facing 92-140 deg off Obelix too (run for Asterix on the ledge). ini nolock_romans: "they
  face and go after that hero".
- Round 180 (user's pad test of rounds 178-179: revenge after the pause, the first group's delay and Romans facing
  the waiting hero all fine; no-lock Romans "finally start to feel good"; asked: no archers for no-lock Romans).
  nolock.cpp FreeRomans: no CKRomanArcherCpnt (0x627CF0) pools either (LV3's squads with an archer pool: #1, #5,
  #8, #19, #31, #32, #33 -- testruns poolkinds). Test aid XXL_NOLOCKGROUP (a fixed group size). Autotest nolock34m
  (-Level 3, XXL_NOLOCK=1 XXL_NOLOCKROMANS=1 XXL_OBELIXPOTION=0 XXL_NOLOCKPAUSE=3000 XXL_NOLOCKFIRST=3000
  XXL_NOLOCKGROUP=6): 24 Romans in 4 groups from #19 / #5, none from their archer pool 1 (earlier runs: pool 1 in
  most groups). ini nolock_romans: "never archers".
- Round 181 (userlogs 20261031-xxl_coop.lv3-6_roman_stuck_running.log, LV3-6: Obelix waiting, Asterix launched far
  off by the corkscrew -- squad #4 run for Obelix alone -- Obelix walked east out of #4's box (x max -132.0) onto
  the pier (-123.5 -4.0 -230.8); one of #4's 3 revenge Romans held back at (-132.0 -1.9 -233.6) from 85.3 s to the
  level's end at 94 s. The user saw Romans "stuck running to a place they couldn't reach" (single player has it
  too) and asked for the guards' rule: give up after 1 s until a player is somewhere reachable). Test aid
  XXL_NOLOCKTRACE: each Roman out every 15 frames (place, hero / spot distance, +0x1A4, +0x24 bits 14 / 1, stuck;
  cap 400). Findings:
  - A Roman a wall stops stands on its own: nolock36s's at the box edge, their spots (x -132.5) on wall cells
    (PFDUMP: the pier's row z -233.7 walkable past x -132, the rows by it walls), 0.5-4.8 units off them, 0x22;
    nolock36u's (Obelix dropped into the water by the ledge) 18-26 off theirs, 0x22.
  - A Roman by a hero who walks off keeps still 1-2.5 s in 0x29 / 0x2A before it follows (0x28).
  - Running on the spot = held back every frame (+0x24 bit 14 on end, reaction 0x28): the user's log; nolock36s
    with XXL_NOLOCKNOCLAMP=1 (the spots not kept in the box) has all 3 held at x -132.0..-132.1 to the run's end.
  - The user's run itself not reproduced (2, 3 and 5 Romans, Obelix stopping by the edge, Asterix off: hold-backs
    of 0.1-0.2 s at the edge only).
  nolock.cpp GivenUp (from NoLockSteer): a Roman held back 1 s on end, neither 0.2 units nearer its spot nor 1 unit
  from where it was, not reacting (below 0x22), no hero within 4 units -> its slot = where it stands (it stands,
  facing the hero; log, cap 40); it goes on again once the spot it gave up on has moved 1.5 units and
  PfReachable(Roman, spot) is not PF_NO (checked every 0.5 s at most), or with a hero within 4. The first tries
  (walking -- 0x24 on -- and not 0.2 nearer in 1 s; then also not 1 unit on) stopped Romans following a hero
  faster than they walk and the ones keeping still after a fight -- one left 13-19 units behind for 7 s, its spot
  (clamped at the box edge) moving under 1.5. Autotests nolock36s (nolock36w's env, XXL_NOLOCKPAUSE=3000
  XXL_NOLOCKFIRST=8000 XXL_NOLOCKGROUP=3 XXL_NOLOCKTRACE=1): no give-up, one 0.2 s hold-back; with
  XXL_NOLOCKNOCLAMP=1: built without GivenUp, held to the end; with it all 3 stand 1 s after (0x22, bit 14 off),
  go on again as Obelix moves (held, standing again 1 s on), and with him back in the box go round him. nolock36u.
  ini nolock_romans: "one held back from where it is going for a second stands still until the hero moves
  somewhere it can walk to".
- Round 182 (user: no-lock Romans on by default). config.h noLockRomans true; dist ini nolock_romans = on. The
  user's ini had lost [coop] romans_spawn_in_view (an older copy saved over it from Notepad): put back as in dist
  (off -- the default when the key is missing too, so the Romans the user saw come in in view were not from it).
- Round 183 (user's pad test of round 181: the LV3-6 pier Roman still ran on the spot; userlogs
  20261031-160145-xxl_coop.lv3-6_roman_stuck_running2.log + screenshots/Snipaste_2026-10-31_16-01-35.png). The log:
  squad #4's first Roman (of 2; its spot clamped to x -132.5, 4.5 north of it on the pier's wall cells -- PFDUMP:
  2x2 cells, the row z -233.7 walkable, the one north of it walls) held back at (-132.0 -1.9 -233.2), the box's x
  max, given up at 147.5 s ("it stands where it is") and held back still (+0x24 bit 14 on, no change logged) to
  the level's end at 169 s, Obelix standing at (-124.7 -4.0 -231.1). nolock36s with XXL_NOLOCKNOCLAMP=1 (trace now
  with the frame +0x80, +0x38 and the slot): one given up at x -131.99, its slot its own place, stayed held in
  0x27 (turning) for 2.7 s, to Obelix's next move; ones given up at -132.01 / -132.04 stood (0x22). Turning to
  face the hero -- out of the box -- a Roman on the edge steps out, is put back (0x4CEE00: out of the box with
  +0x38 inside -> +0x2C = +0x38, bit 14, return), and never turns round. The tick read: bit 14 also comes without
  a put-back (FUN_0045c560 false, or FUN_0045d7e0 true). nolock.cpp NoLockSteer: the stand spot of a given-up
  Roman is its place kept 0.5 units in from the box's x / z edges (KeepInBox, the slots' clamp). nolock36s +
  XXL_NOLOCKNOCLAMP: 12 trace samples held and given up before, 0 after (all 3 step 0.3-0.75 in, 0x22 / 0x23).
  Autotest nolock36v (nolock36s's env, XXL_NOLOCKGROUP=2): the user's walk timed from the group's coming (log
  offset 10.5-17 s here, not 5.9): both Romans stop at the edge on their own (0x22, x -132.5..-132.8), no give-up,
  follow Obelix back -- the user's x -132.0 not reached in 3 runs.
- Round 184 (user request, screenshots/horizontal_flask_overlap.png: the potion's flask on player 2's combo bar in
  the stacked split; a left prompt box coming). The flask is in-game interface model +0xAC, part FUN_0051BA70
  (state +0x24 bits 15-16; it rises from below to modelling y +0x280 / +0x294, current +0x29C; shown at 2D (0.08,
  0.10), HUD camera scale 0.2). hud2.cpp LayoutFlask (Hud2BeginP1, any co-op frame, one screen too, not at the
  merchant): moved for the pass to under the health of Asterix's player's HUD at 0.6 size -- (0.15, 0.80), (0.85,
  0.80) for player 2 side by side, (0.15, 0.30) for player 2 stacked (its shields there at y 0.42, its combo column
  x 0.07, y 0.03-0.39). The gate counter (LayoutGate) uses the side-by-side layout in the stacked split too (middle,
  3/4 size, sliding up from the bottom), its sparkle with it. Autotest hudhints (XXL_P2HERO=asterix XXL_HUDPOS=1;
  player 2's Back held shows its shields); crowd36 shows the counter on the halves' border.
- Round 185 (user request: a prompt / hint box per player, per-player hints, combo hints). Facts found:
  * HUD 2D: FUN_00414070 draws the menu manager's element lists first (FUN_0042A420, call at 0x4140B3; CMenuManager
    vf12 -> FUN_0042CE00 -> each element's vf23) -- the prompt's text +0x15C and plate +0x140 are drawn there --
    then the root container (FUN_0042A470, call at 0x4140F4). An element's vf23 (thiscall(arg), ret 4) draws it if
    +5 bit 1 (value 2) is set, from its place +0x14/+0x18 (x a share of the width from the left, y of the height
    from the bottom) by its alignment +6/+7 (0 rightwards / downwards, 1 middle, 2 leftwards / upwards).
  * CText2d is 0x6DC bytes (array factory at 0x42CF4C), constructor 0x42CE60; its layout object at +0x20 (vtable
    0x623BCC): +4 the localisation manager, +8 box width, +0x14 string id, +0x18 the string (inline, ~350 chars),
    +0x2D4 words, +0x2D8 word pointers into it, +0x3C8 + 8i word x / y (text units: the screen is the aspect wide,
    1 high), +0x5A8 a ushort per word (its line), +0x620 per-word skip, +0x660 font, +0x664 size, +0x6A8 line
    spacing, +0x6B0 first line shown. FUN_0042E590 (thiscall(layout, id), ret 4) takes the string and lays it out;
    a word draws at (place.x * aspect + x, place.y - 1 + y [+ valign]). No heap: a byte copy is a working text.
  * xxl_widescreen2d.asi (TextDrawHook / BillboardDrawHook) records each stretched plate (+0x3C set) with its
    unsqueezed width, and draws any text or billboard whose place is inside one (this frame's or the last) nearer
    that plate's anchor by (4/3) / aspect -- two wide boxes' records reach into each other past the middle.
  * The prompt kind is written per hero by FUN_0049A1A0 every frame (coop.cpp now keeps each hero's own:
    CoopPromptKindFor; kind 9 machines mapped to the offered hero, both on the boat's deck: both).
  * Combos: merchant item k >= 6 sets game manager +0x40 -> +0x10 bit (k - 6) and gm +0x400 + (k - 6) = 1 ("bought
    this level", cleared by FUN_004FF1C0, undone by FUN_004FF180 from the pause menu): bit 0 Club, 1 Power Hammer, 2
    Twister, 3 Fusion, 4 Mole, 5 Rage. Rage: Idefix's AI state 6 (bite, 0x49E4C0) on entry, with the bar full (stats
    +0x10 bit 0x4000), bit 0x20 and trio (gm +0x14) vf53 / vf54 both 0 (combos not switched off), bites +0xB90
    times (+0xB91), sets stats +0x10 bit 0x1000 and spends the bar with FUN_00493810(5). FUN_00493810 (thiscall(gm,
    kind), ret 4; gauge -= cost gm+0xBC+kind of the full bar; empties it when < 0) is every combo's spend: Club 0
    (0x493788, bit 0x80), Power Hammer 1 (0x493D3A), Twister 2 (0x494109, 0x200), Fusion 3 (0x494028, 0x400), Mole
    4 (0x494316, 0x800), Rage 5 (0x49E648, 0x1000). The game's six combo pictures: Game Files\Combos\Combo00-05.png.
  * Interface sound n: FUN_005153F0 (thiscall(itf, n, 0), ret 8; list +0x16C, 0x40 each): 7 a combo bar level
    reached (the filling sound), 8 a level lost; the level sparkle is particles node +0xC0 placed at the bar.
  * The fight gate's Romans left: squads with +0x154 >= -1, per pool to defeat (int16 +0x18, < 0 endless) - sent
    (+0x16) + alive (squad vf42) -- gm +0x64 is not it (the counter's value at its end).
  * The trio's fusion range +0x3D8 is 3.0; with individual heroes no Twister is copied (each player's own).
  What changed: hints.cpp -- two boxes (copies of the prompt's text object, the game's plate set and put back
  per draw) drawn after FUN_0042A470; the game's box left out of both 2D passes in co-op (not while a hero shops:
  the HUD is then the shopper's half); content per box: the player's hero's prompt, else its newest hint
  (HintShow(key, player, text, ms), HINT_BOTH), a change held 120 ms (300 ms from a prompt), slid out then in; text
  and sparkles drawn from y 1.5 (on no plate) with the words lowered as much. Boxes: side by side and one screen
  left (lines right-aligned) / right (left-aligned), stacked both right (player 1's 0.5 up). Sparkles: spark / glow
  glyphs (drawn, 4 strengths) at a button word of the last line. combohints.cpp (spend hook on 0x493810; the
  hero = LifeTickingHero, Idefix's tick wrapped in IndivEnter of the hero whose RB called him, 300 ms). icons.cpp:
  pad_*_unpressed, key(_wide)_unpressed.png (prototypes in coop\icons), a "next" chevron (the font has no '>'), the
  merchant's combo icons read from LVL0n\LVL0n.KWN (o_ico_massue_g01 ... o_ico_idefix_g01, their pinkish backing
  keyed out: R/G 1.40, B/G 1.11). Combo cheat sets 0x3F. Autotests: hudhints (XXL_SPLITMODE, XXL_ICONP2KB=1 for
  keycaps), iconsboat49 -Level 4 (both boxes' "Use"), combohint2 (player 2's A A X B lit; the Club itself not done
  -- finished mid-air), combohint2b (Mole done -> its hint goes, the Club's next), crowd36 -Level 3 (200 to beat:
  12 s + 8 s), fuse2 (fusion hint on both, then each player's roles while fused), rage2 (player 2's RB: Rage done).
  Test aids S_HINTTEST / S_HINTSPARK / S_COMBOHINT (100 + k: first in line; -2: the Twister learned quietly).
  Autotests in a remote desktop session: the window minimised and restored at 1.5 s and 5 s (else white shots).
- Round 186 (user's pad test of round 185: all worked; asked for): (1) every box's lines left-aligned (hints.cpp;
  the left box's right-aligned lines read oddly). (2) combohints.cpp: a combo whose first button is A (Club, Power
  Hammer, Twister, Mole) is pressed in the air -- with some of it lit, the hero off its feet (+0x384 bits 0-2) and
  then on them again resets it (log "landed with n of the X's buttons"; autotest combohint2l: A, then A A). (3)
  coop.cpp: the LB pass hint's count (3 a level) and "passed once" are per hero. (4) hud2.cpp LayoutFlask: the
  flask at the game's own place again (0.08, 0.10 at 16:9), anchored to the corner by screen heights (0.142 from
  the edge: the HUD's models are placed by shares of the width, so wider screens took it in from the corner) --
  mirrored for player 2 side by side, at the bottom of his half right of the combo column (3/4 size) stacked; it
  rises over a hint box under it (HintBoxRect: the plate as drawn, sliding included) to 0.01 above its top, at
  interface +0x278 a second (the game's flask slide speed), and comes down after. hudhints shows it above both a
  two-line (player 1) and a three-line (player 2, XXL_P2HERO=asterix) box.
- Round 187 (user's pad test of round 183: the LV3-6 pier Roman still ran on the spot; userlogs
  20261101-012914-xxl_coop.lv3-6_roman_stuck_running_again.log + screenshots/Snipaste_2026-11-01_01-29-09.png --
  squad #4's 4 revenge Romans, Obelix east along the pier to (-122.4 -1.8 -233.7); one held back at (-132.0 -1.9
  -233.6), 0.8 off its spot, 9.6 from Obelix, given up 1 s on, held back still (no change logged) to the level's end;
  and 20261031-234345-xxl_coop.lv2-1_roman_stuck_running.log -- squad #29's revenge Romans sent from Obelix's
  walk-to marker by its box's west edge: two held back from the start, given up and gone again over and over).
  Autotest nolock36y (nolock36s's env, XXL_NOLOCKGROUP=4, XXL_NOLOCKCORK=0 -- the user's ini has
  nolock_except_corkscrews on, which locked Obelix at his test spot): the user's walk timed from the group's
  coming, then player 2's camera turned round with the right stick (VPADR 1.6 s, the user's idea) -- that run
  reproduced the user's line exactly (held at (-132.01 -233.64), 0.8 off, 9.6 from Obelix, 0x28). Trace (now with
  the Roman's pathfinding node +0xA8 and its frame's forward +0x70, verified: (1, 0) walking east): Romans don't
  push each other (XXL_NOLOCKANGLE, a test aid: one spot for all -- 4 stood on one point); the crates on the pier
  stop a Roman 0.55 short of a spot at z -233.0 ([22]); about 0.5 s on it walks again ([28]), not to its spot (north)
  but east-south-east at Obelix (the squad's attacker goes straight at the hero -- one Roman a group, as in all the
  user's logs), slides along the crates onto the box's east edge (x max -132.0) and is held back there; given up,
  its slot its own place, no step and its own facing, it stayed held ([25] / [28], facing turning) -- pushed out of
  the box each frame by what it stands against and put back, not by its slot. Put back by hand onto its trail (0.56
  in, a test) it was on the edge again 0.15 s on. Fixes, nolock.cpp:
  - MoveBox(keep): a revenge squad whose box holds the waiting hero has its box grown over what the hero can walk to
    within 60 units (ReachBox: x -200.9..-132.0 -> -222.8..-116.8, then GrowBox as he goes, to -94.9) and its own
    box back as for one brought over (none of its Romans left / the wait over). nolock36y x2: no hold-back (0 of
    143 trace lines), the Romans walk onto the pier past the crates to 1.6-4 units from Obelix and knock him back;
    nolock36u (Obelix in the water): 0 held.
  - GivenUp: a Roman given up has, every frame, its slot its place (frame +0x80), no step (slot +0x18 zero) and its
    facing its frame's forward -- nothing to walk or turn to, however small (round 183's stand spot 0.5 in from the
    edge, and a fixed place / facing taken at the give-up, had it turn 0.04 rad or step 0.05 onto a cell its tick
    flags: [27] / [25], held for good). nolock36s + XXL_NOLOCKNOCLAMP: every give-up "free to go again" within 30 ms,
    173 trace samples given up -- all [22] / [23], none held (one of 3 held for 1.8 s before). It goes on again only
    with the straight line to its spot within the graph's node boxes too (pathfind.cpp PfLineInNodes: every 0.25
    units, y within 1; the tick puts back a step off them -- LV2-1's walk-to marker: walkable to, yet held again
    1 s on each time). Log: one given up held back still 1 s on (cap 20); the give-up line has the place.
  ini nolock_romans: a won fight's Romans follow the hero anywhere it walks; only other squads' wait at their edge.
- Round 188 (user: after a game over player 2's Obelix, not leading, kept his full combo bar; "a P2 combo bar
  issue"). The stats were right: the respawn (CKAsterixGameManager::vf13, 0x501940) sets the health, then calls the
  in-game interface's reset through gm +0xF8 (CKHkInterfaceInGame::vf16, 0x517870, thiscall, plain ret: +0x24 bits
  12-14 (combo bar state) and 23-24 ("Combo" word) cleared, the combo models +0xA4/+0xB4/+0xB8/+0xC0/+0xCC hidden,
  shields' alpha 0xFF), then empties the combo fields. hud2.cpp puts player 2's copy (g.s2: +0x24 bits, ranges,
  models) back over the interface at each of its ticks, so its bar stayed in state 4 (full, glowing) after the reset.
  Now a vf16 hook (slot 16 of vt 0x6285F0) drops the copy (it starts again from player 1's, reset) and P2's health
  marks. Autotest overcombo (level 2: Asterix down by area hits, Obelix 20 + 20 hits -- the bar has to see it fill:
  30 at once is already full while the heroes are apart, then no "Combo" -- then down too): before, after the
  respawn "bars p1 00002000 p2 18004000" and P2's bar glowing; after, both 00002000, empty.
- Round 189 (user's list: revives and the game over). individual.cpp / pathfind.cpp / config.cpp:
  - pathfind.cpp Steps: one step list (8 ways in a grid past walkable corners, walked over to a node at its box,
    the sidekick's jumps) for Flood and two new searches: PfWalk(from, to) (Dijkstra from `from`, stops at `to`'s
    cells; 0.3 ms, 436 cells for LV4-1's 28 units) and PfWayTo(to) + PfWayFrom(p) (the steps turned round -- CSR of
    predecessors over all 33337 cells of level 2 -- and Dijkstra from `to`: every cell's walk there, 16.6 ms once).
    Lengths are cell centre to centre (cells 2 x 2 units), x / z. Node boxes' y is no ground height (LV2-6 node 99:
    0..6.3, ground ~2.7) -- never place a hero on a cell.
  - Catch-up (CatchUpSpot): a fallen leader the other can't walk to (3 noes as before) -> the other is brought to
    the candidate nearest it with a way to the fallen hero: PathSpot list per hero (a spot every 3 units of the same
    solid ground NoteSafeSpot takes, 64 kept, with the loaded sector, level +0x24; only this sector's), both heroes',
    1+ unit off the fallen one; else SpotBesideFallen as before. Fixed limit: only spots whose walk <= (time left -
    grace) * speed. Send-back after the revive only for a revive by the grab (Revive byGrab; broughtTo = where it
    revived; > 10 units on from there -> stays); a sector change's / launcher's revive leaves it where it is.
  - Time limit (revive_time_limit off / N / auto, revive_time_limit_grace 30): N set at the fall; auto at the first
    reach answer that has a way (PfWalk, or the straight line with no graph) or at the catch-up (its walk), once.
    Walk speed 115.36 / 17 = 6.79 units/s (user's measure; an autotest's Obelix up a slight slope ~6.1). Counts
    while not paused (gm +0xCC & 3 != 2), no shopper, not suspended, last reach answer not "no". Hints every 250 ms:
    the fallen player's box "X comes to revive you / N s left", the other's the down hint + "N s left" (or the grab's
    with it). Run out, grab not held -> TimeOut: gameOver, spared = the standing hero, Use(down) (gm sees 0), Raise
    the fallen hero (its death from the first frame: the 0x0E handler sets gm +0x2FD at the animation's end -- left
    lying, the reload came at once), coop.cpp CoopDeathIs = NoteDeath + LeadTheDeath (it leads, the other the AI
    companion: the death sequence -- lightning, camera, fade -- on it, one screen). IndivEnter's spare scope
    (kSpareScope 2): the spared hero's business writes its own health into cb +0x50 and the place's back after
    (Hero_PreLifeTick would kill it on the 0). Respawn 7.1 s on, as a both-down game over.
  - Hold time revive_hold_seconds (5) / revive_hold_seconds_catapult (2): CKHkCatapult (2,130) fires at the leader
    from +0x48 to +0x4C units off (x / z) its node (+0x10 -> frame +4, world +0x80) when +0x14 & 0x14 == 0x10 (0x10
    armed, 4 destroyed); the fallen hero within +0x4C -> 2 s. LV3-1's: 12 to 50 units, flags 18.
  - Autotests: reach26c (-Level 2, XXL_DOWNAT=0:6800 XXL_REVIVELIMIT=auto XXL_REVIVEGRACE=8: Asterix placed spot by
    spot round LV2-6's far side, down at (84.7 2.5 -378.5); Obelix brought to the pit's edge (90.5 2.7 -389.3, a
    14-unit walk, 24 from him), 11 s, ran out, death on Asterix, Obelix walking as the AI companion, sit never 0E),
    reach26f (XXL_REVIVELIMIT=14 XXL_REVIVEGRACE=10: the edge past the 11-unit ceiling, brought to 86.0 -389.3, a
    10-unit walk), revive41 (-Level 4, XXL_DOWNAT=0:4000 auto 30: heroes connected on LV4-1's field, 28 units ->
    34 s, revived by B in time), catrevive31 (-Level 3, XXL_DOWNAT=0:3500: by LV3-1's catapult, revived in 2.03 s).
    reach23g / reach26w (no limit) still send Obelix back after the grab's revive. Env aids XXL_REVIVELIMIT,
    XXL_REVIVEGRACE, XXL_REVIVEHOLD, XXL_REVIVEHOLDCAT.
- Round 190 (user's pad test of 189: all worked; follow-ups). (1) The countdown only in the downed hero's player's
  box ("X comes to revive you / N s left"), refreshed through the other's grab (TimeLimit no longer stops while it is
  held; a released grab hides only the reviver's box). (2) Catapult hold: CKHkCatapult fields by XXL-Editor's member
  order = deserialize order: +0x48 passive range (12; nearer it holds its fire), +0x4C attack range (LV3-1 50, LV6-4
  30), +0x74 charge duration (LV3-1 3.5, LV6-4 5.0; the float after the CNode +0xD4); also +0x2C 1.5, +0x50 20,
  +0x54 75 (throw angle, degrees), +0x58 0.8, +0x5C 20, +0x60 2, +0x64 2.5, +0x68 / +0x6C 4. FUN_00509E20 = the
  throw: timer +0x70 += dt to +0x74, then state +0x1A = 2, +0x70 = 0, one of 4 stones (+0xB4 + 4 * +0x1F).
  catrate31 (XXL_P2HERO=asterix XXL_CATTRACE=1, Asterix leading 17-43 units off, knocked 5 units back a hit):
  throws 3.53 s apart, each stone landing 2.81 s on (2.97 at 38.6 units) -- so the stones land +0x74 apart; the
  flight doesn't widen the gap (the next stone is in the air as one lands). Hold = revive_hold_catapult_window
  (0.4) x the gap (several armed catapults with the fallen hero between passive and attack range: 1 / sum of 1 /
  charge), at most revive_hold_seconds (CatapultGap / HoldSeconds; logged on change). catrevive31 (-Level 3,
  XXL_DOWNAT=0:3500 XXL_DOWNAT2=0:14000 XXL_REVIVELIMIT=auto, ENDFIGHTWON at 2.5 s and 12.1 s): 5.0 s at the
  user's spot (2 units off: passive), 1.41 s at (91.1 -1.8 14.4) 17 units off. Lessons: LV3-1's courtyard fight
  (gate 60) has Romans by the catapult -- as guards of the downed hero they punched Obelix off the grab (1-point
  hits); the user asked to beat such squads in autotests (ENDFIGHTWON). Steps must be in time order (a late step
  early in the table holds back all after it).
- Round 191 (user, before round 189: xxl_coop.lv2-1_lifter_revival_tp.log -- Asterix down by the hearth at the top of
  LV2-1's lifter (-745.1 515.0 -979.9), Obelix put back off the lifter and left below (-747.2 507.6 -1008.6), put
  back again each time he stepped on it, "can Obelix walk to fallen Asterix? yes": a softlock). PFDUMP: the lifter
  shaft is node 53 (-751.5 491.5 -998.0)..(-742.3 531.5 -985.8), 5x7 cells of 1.84 x 1.74, joined to the corridor
  below (node 24) and the hearth's floor (node 54) only by the sidekick's jumps (kind 1, flags FFFFFFFD: on), the
  lifter's node place (matrix +0x40, (-746.8 495.9 -991.7)) inside it. pathfind.cpp PfCloseNodesAt(places): nodes at
  most 16 units across x and z holding a place are shut (Walkable false, no steps out, no jumps in) for every query
  until cleared. individual.cpp ShutLifters (all CKHkLifter 2,47 node places but the origin) around WayToFallen's
  queries: no way with them shut but one without -> "only over a lifter" (reachTold 2: the countdown waits), the
  catch-up whoever led (the level never brings them together past a lifter the mod keeps shut), CatchUpSpot /
  PfWalk with the lifters shut too, and no send-back after the revive (broughtPastLifter). Autotest liftdown21
  (-Level 2, XXL_DOWNAT=0:4000 [XXL_REVIVELIMIT=auto]): Asterix placed on top, Obelix below at (-766.3 508.5
  -1001.0) -- 20 units off the Roman agent at (-745.5 508.0 -1010.2), whose dialogue (Obelix placed by him) had
  suspended co-op and made the fall a game over -- then onto the lifter (-746.7 506.5 -992.1): put back below, "no
  -- only over a lifter", brought beside Asterix 3.1 s after the fall, revived by B, stays on top. reach23g (sent
  back) and reach26c unchanged.
- Round 192 (user, after 191 was confirmed: a hero down ON a lifter -- alone, it should wait there for the other;
  with the other on it, they are together already). individual.cpp LifterUnderFallen: the lifter (2,47) a field of
  which (+0x10..+0xFC) is the fallen hero's ground (+0x280, a CDynamicGround 0x6241BC -- it stays the lifter's
  while the hero lies in 0x0E). That lifter is left out of OnPartnerGround (no put-back), IndivHoldsBack lets its
  tick run (no put-back either), ShutLifters leaves its node open. Holding its tick (first try) froze it, and the
  first tick after the revive rode the whole way at once (y 501.3 -> 516.5 between two 0.45 s samples, liftdie21a);
  running, it carries both heroes up in its own ~0.7 s ride with its springy settle (516.8 -> 513.8 -> 514.3, as
  with nobody down). Autotests (-Level 2, SQUADWON 29 first -- user: the lifter goes up once it is beaten):
  liftdie21a (XXL_DOWNAT=0:4500: Asterix down alone on it at its foot (-746.7 501.3 -992.1), Obelix put on it at
  7 s: no put-back, up with both, revived by B on top, health 8), liftdie21b (XXL_DOWNAT=0:3500: both on it, down
  as it rides up: Obelix stays on it, revived). liftdown21 (fallen by the hearth, not on it) unchanged.
- Round 193 (user: a softlock by jumping down a drop after the partner's fall -- covered? Only for a fallen leader:
  the 0.5 s checks run all through the fall, but round 83's rule (the user's: the level brings the leader to its
  sidekick) left a non-leading fallen hero waiting for a sector change). Narrow fix (user's go): hadWay[down] is set
  by any "yes" (lifters shut) since the fall; three noes after one -> cut itself off -> the catch-up whoever led,
  broughtStays (no send-back after the grab's revive; it can go back the way it went). A fallen sidekick never
  reachable since the fall still waits. Autotests (-Level 2): reach26s (XXL_P2HERO=asterix XXL_DOWNAT=1:4000: both
  on LV2-6's far side, "yes" at 2 s, Asterix put across the pit at 7 s: "no" 1 s later, brought to the pit's edge
  (90.5 2.7 -389.3, an 8-unit walk), revived, stays at 85.7 -384.2), liftdown21r (same env: Obelix down by the
  hearth, Asterix below then onto the lifter: put back, "only over a lifter", brought up, revived, stays),
  reach26o (Obelix leading, Asterix never reachable: stays, as round 83).
- Round 194 (user: xxl_coop.lv2-6_stuck.log -- Obelix down at (21.8 2.1 -386.3); Asterix, trying an unreachable
  jump, went from the ledge (49.1 13.1 -391.1) onto the belly slide's ramp and through zone 0CA8 (its group messages
  the slide path: GroupNeedsBoth): "put back" to (41.3 8.2 -390.2), then stuck at (48.05 8.21 -392.22) to the level's
  end). IndivBlockPartnerSpotFrom (zones, round 65) puts a hero at the detection service's place of the frame before
  plus 1.5 units back the way it came, at that place's height + 0.3 -- coming down the steep ramp in the air (hero
  +0x384 = 09: bits 0-2 off its feet), 1.5 units back up the slope at the lower height is under the ramp. Autotest
  zonejump26j (-Level 2, XXL_P2HERO=asterix XXL_DOWNAT=1:3500: P2's Asterix jumps from the user's spot (48.836 13.209
  -389.419) with the stick ahead): before, put at (44.4 8.3 -387.4), stuck at (48.06 8.00 -389.28), +0x384 09, for
  good (the stick moved nothing); now off its feet or on Slide ground (+0x2A bit 3) -> IndivBlockPartnerSpot (the
  trail), back at (48.8 13.7 -389.4) on his feet, walking again. zonejump26 (glides) the same; slidecross3 (LV2-2,
  walking glides) still steps back before z -903.82. Note: the autotest state line's "gr %p/%vt %02X" byte is hero
  +0x384, not the ground's +0x2A. The belly slide's ground check (round 62) is the Slide-flagged slope itself, apart
  from the slide's start zone (0x480B to CKAsterixSlideFP): slide.log had the start zone held and the slope walked
  down past it to the level's end.
- Round 195 (user, after 194 worked on the pad). (1) The belly slide's Slide-ground put-back (round 62, ground +0x2A bit
  3) is gone from BlockPartnerGrounds and from the zone put-back's in-air rule: it dated from before round 64 made the
  slide's start zone fire with a hero down (slide.log: the plane walked past, the slope walked down to the level's
  end). The slide's start is LV2-2's cross detector (plane z -903.82 -- the user's -906.187 534.706 -903.680 --, event
  0x0028, group 5 -> 0x480B to CKAsterixSlideFP): slidecross3 (XXL_DOWNAT=1:4000) logs "zone event 0028 held back",
  the first put-back the frame before's step back (-906.0 524.9 -905.2), Asterix ends before the plane (-904.8).
  (2) User: xxl_coop.lv2-6_unreachable.log -- Asterix up on the slide ramp (54.9 10.0 -390.0), Obelix down below it
  (27.4 1.9 -391.6), apart since before the fall: "no" every 0.5 s, nobody moved (round 83: a fallen sidekick
  waits; round 193's hadWay only once a way was known after the fall). Now the catch-up runs whoever led;
  broughtStays only for a lifter or hadWay (its own drop) -- otherwise sent back after the grab's revive (the
  fallen hero's place may be one it can't leave). Log "(not since the fall)". Autotest unreach26 (-Level 2,
  XXL_P2HERO=asterix XXL_DOWNAT=1:4000; SQUADWON 13/14/15 first -- the LV2-6 fight's guards knocked Asterix off the
  grab; ENDFIGHTWON found no gated fight there): brought onto Obelix 3 s after the fall, revived in 5.0 s, back up
  at (54.9 2.5 -390.0). reach26o (Obelix leading, Asterix never reachable): now brought too. zonejump26j: still the
  last solid ground (48.8 13.7 -389.4). Lesson: a no-op `sed -i` turned individual.cpp's CRLF into LF (unix2dos
  put it back) -- never run sed -i on sources.
- Round 196 (user: userlogs 20261101-050700-xxl_coop.flask_offscreen.log + screenshots/Snipaste_2026-11-01_05-05-56.png:
  the flask off the left edge after the last combo hint went; prompts of the launcher and the gun on one screen
  only; options for the join, pass and combo hints). (1) xxl_widescreen2d.asi CloseRows pulls a HUD model whose 2D
  spot is on a recorded plate (this frame's or the last, PlateAt) towards that plate's anchor by its squeeze; the
  co-op box's plate was recorded, the flask rising over player 1's box was pulled with it, and as the box slid out
  (its anchor off the screen) dragged off the screen; the records only rotated on a 2D billboard / text draw, so a
  frame with none kept them. The asi now exports Widescreen2dRecordPlates(int) (hints.cpp switches it off around
  the boxes' plates: squeezed, not recorded, not pulled; an older asi: logged, as before) and CloseRows calls
  PlatesNextFrame. Autotest flaskbox (XXL_OBELIXPOTION=0, vertical): old asi -- flask at the edge then gone; new --
  in its column, down to its place. (2) Launcher = CKHkTrioCatapult (2,190, vtable 0x6264A4) vf15: ready (+0x14
  bits 3, 6, 7 clear), not timing (+0x24 != +0x38), its sector (+0x18 == level yp+0x90 -> +0x24) and the leader
  on its platform (trio vf29 == +0x48) -> trio 0x5130 (FUN_00482610 case 0x30: leader +0x544 = it, +0x3F0 |=
  0x4000; cleared by the game each frame -- an "Activate" goes once the hero steps off). proximity.cpp
  LauncherOfferBoth (after its tick, sliced or not): the other player's hero on +0x48, on its feet, gets the same.
  The machine gun offers itself (FUN_004C54D0, trio 0x4A01) within +0xAC (2.2 at LV3-3) of its node while free
  (+0x10C bit 0); offered, either player's X takes it (NoteMachineUse's request): coop.cpp MachinePromptHero
  shows "Use" to both heroes within 4 units; with Asterix alone on it, Obelix within 6 of its handles gets "Use"
  (ObelixAtGunHandles). CoopPromptKindFor no longer needs the game's prompt kind (gm +0xCC) set for a hero's own.
  Autotests launchboth (level 2: one on -> P2's, both -> both, off again -> P1's goes), gunboth33 (-Level 3). Not
  tested in autotests: the second hero's X on the launcher. (3) [hints] hint_join, hint_pass_roman, hint_combos
  (config.h hintJoin / hintPass / hintCombos, all on; logged "config: hints: ..."); combohint2b with all off: no
  join or combo hint.
- Round 197 (user: Obelix alone on the machine gun -- LV4 / LV5 boss fights; the leader flag must not matter). The
  gun (CKHkMachinegun, vtable 0x6265E0, tick FUN_004C51F0): +0x10C bit 0 in use (its event 0x4A00: param != 0
  on, 0 off -- also flags its in-use / free event group, +0x1A / +0x1C), +0x14 bit 7 the rail move on (0x4A08, sent
  by trio 0x480A for Obelix); in use FUN_004C5560 reads the gun map: rail input +0x4C = action 1 - action 0
  (with +0x14 bit 7), action 2 -> +0x10C bit 3 -> trio 0x4A02 (fire); FUN_004C5620 moves it along its rail
  (+0x68, its spline +0xF8, clamped 0.2 from the ends; LV3-3: 0.1 .. ~3.1). Trio 48xx (FUN_00482BB0): 0x4809
  case 9 -- Asterix (slot 0, whoever leads) f18 mode 2 player, gun 0x4A00(1), +0x3F4 |= 4, Obelix (AI state 0 /
  4 / 5 / 0xB) AI order 7 to 0x4A05's spot, Idefix to 0x4A06's; 0x480A(hero) case 10 -- that hero's f18 state 5
  (Obelix's CKHkObelixLife::State5_Ai: follows the gun, 0x4A07 its facing; entry 0x4A05 puts him at the handles),
  Obelix -> gun 0x4A08; 0x4806 case 6 -- only with Asterix in state 4: orders 0, gun 0x4A00(0), +0x3F4 &= ~4; all
  f18 to mode 0. The trio's offer (+0x48, +0x3F4 bits 10-11) stays after a take: Asterix's X anywhere took the
  gun through it (FUN_0049A080). coop.cpp: Obelix's X at the offered free gun (NoteMachineUse, in his tick) ->
  ObelixTakesGun: gun 0x4A00(1), +0x3F4 = (& ~0xC00) | 4, trio 0x480A(1); ObelixGunSolo (Obelix mode 2, Asterix
  not, gun in use, not held) -> no suspension (split on), shared.cpp SHARED_GUN_OBELIX (gun actions 0 / 1 his
  player's, 2 nobody's: Route -2 = 0); WatchObelixGun: his player's A (values 4 / 44) or his fall -> ObelixLeavesGun
  (gun 0x4A00(0), +0x3F4 &= ~4, f18 mode 0, order 0); ForgetP2 too. Asterix's X within 4 of the gun node -> trio
  0x4809 in his tick (SHARED_GUN, one screen); not leading, the mode he starts in his tick keeps the lead till the
  gun is left (gunAlone exception off for it: asterixGunJoin) -- as Obelix leading he had been the one screen's
  AI companion (f18 08 -> 0B), unable to fire or leave; GiveBackLead after (4801). Obelix's take keeps the real
  lead (gunAlone for obelixGunTook). The gun's camera (rail camera, vt 0x624624) goes to Obelix's half either way
  (his copy not leading: the take runs in his tick). Prompts: Asterix's kind 9 while Obelix is on it alone is
  no one's; Asterix within 4 gets "Use" (his X joins). Autotests obelixgun33 / obelixgun33l (-Level 3,
  XXL_GUNLOG=1: rail +0x68 3.06 -> 0.10 left, -> 2.40 right; no 0x4A02 on Obelix's X; far X: none).
- Round 198 (user: the machine gun's hints as an option). [hints] hint_machine_gun (config.h hintMachineGun, on;
  logged with the other hints): off, shared.cpp ShowModeHint shows nothing for SHARED_GUN / SHARED_GUN_SOLO /
  SHARED_GUN_OBELIX (the game's "Use" prompts stay). Checked with obelixgun33 and the option off: no gun hint,
  the prompts as before.
- Round 199 (user: single player gets co-op features, each an option off by default). New ini section
  [single_player] (config.h sp*, env aids XXL_SPCOMBOHINTS / SPICONS / SPPOTION / SPSWITCH / SPSECOND).
  (1) sp_combo_hints: combohints.cpp Solo() -- player 1 only, its hero the trio's leader; a combo is queued
  whoever leads and its hint waits for a hero that can do it (the Mole: Asterix); the fusion hint for the leader
  and the AI companion (it copies the leader's Twister), fused = the leader's Twister step 0x25 / 0x26. The box is
  hints.cpp's single-player one (the game's place, only while the game shows no prompt).
  (2) sp_coop_icons: hints.cpp LocStringHook rewrites every string for player 1 (IconsRewrite(s, 0)).
  (3) sp_obelix_potion: coop.cpp ReadCmdHook -- Obelix leading (only then in a player state), not fallen, gets
  +0x388 bit 4 while the gauge runs (speed 15 vs 7, no hits). The flask stays Asterix's.
  (4) sp_hero_switch: coop.cpp SoloHeroSwitch -- sp_hero_switch_button (PAD_* bit, any XInput pad, not with R3
  held; dpad_up default: preset 3 binds no D-pad in play, only menus 16-35) or sp_hero_switch_key (TAB; edge via
  pads.cpp PadsKeyPressed, game focus, not with Alt) -> CoopGiveLead(companion). SoloSwitchRefusal: pause,
  cutscene, potion dash (gm +0x5C bit 19), either hero not mode 0 / busy / off its feet (+0x384 & 7), the leader
  in a situation (+0x3E9 != 0: grab, lever, merchant...), the companion's AI order != 0 (held by the level),
  fallen, f1A set, >25 units away, a merchant open; refused -> the companion's "no" (bank +0x200).
  proximity.cpp SoloLeadFree (switch or second chance on, no player 2) runs the bundle ticks itself: SoloTick --
  a corkscrew (CKHkCorkscrew vf15, armed +8 & 3 == 2) whose landing event makes hero H wait (CorkscrewHero) is
  ticked with its radius +0x3C = -1 while H does not lead (vf15 compares |leader node - circle| per axis and
  squared against +0x3C: never true); a potion spitter ticks with Asterix as the leader. DispatchAs:
  SoloLeverHeldBack -- the safe sequence for the leader (a lever whose group switches to the hero leading drops
  all contacts) while the companion could take the lead (order 0, mode 0, standing). coop.cpp SoloPotionDash: the
  dash flag cleared when the drinker (IndivPotionDrinker) is not the leader (the AI never ends it).
  (5) sp_second_chance: individual.cpp g.solo -- active without player 2 once Obelix is with Asterix (+0x1A 0 at
  activation); a switch between co-op and single player deactivates (player 2's kept stats survive the
  single-player activation). PlayerOf -> 0. The fallen-hero rules (IndivBlockPartnerSpot(From), IndivBlockTalk,
  IndivHoldsBack, BlockPartnerGrounds, the detection's swapped pass, FlagEventHook's zone holding / let-through)
  no longer need player 2. coop.cpp SoloBeforeLifeTick: a fallen hero not leading is kept in the player variant
  (as the AI it gets up and walks after the leader, round 41) with an all-zero action array and the trio sliced
  to it (the co-op non-leader's arrangement; CoopAfterLifeTick restores both); the SP frame's "AI companion
  again" fix skips a fallen hero; Revive puts a solo non-leader back to the AI variant. Falls are noted in the
  hero's own tick, HeroSendEventHook and HitHook without player 2. CoopDeathIs (revive time out): the fallen
  hero takes the lead (single player's death). Locks: NoteTrioEventFlagged / the 4802-4803 tracking / the
  boarding switch run with individual heroes on; SoloWatchLock notes the AI companion's cable car (order 9) and
  rotary beam (8) rides; CoopUndoLockFor / LockedByLevel (nolock only with player 2) / UndoLock (no AI cycle
  without player 2) undo them when the leader falls. Not ported: the guards of a downed hero (GuardFrame is
  co-op only); the HUD shows the leader's stats only.
  Test aids: S_SPSWITCH (the button), player 1's scripted B sets binding 6's hold time (CoopHoldTime), S_PLACE
  writes +0x80 for an AI companion (placed by +0x40 alone it stayed; in practice FORCE around PLACEAT), env
  XXL_FLIPWINDOW (the remote-session flip without one; screenshots were white all session -- the screen locked).
  Autotests (level 2 unless noted): spswitch2 (switch, walk, back, refused while waiting / in the air), spcork21
  (Obelix leading on Asterix's platform circle: shut; Asterix on it: waits, the switch refused), splever25 (sector
  5's "switch to Asterix" lever: Asterix's punch ignored; Obelix's from 1.6 works it, lead to Asterix -- from 1.2
  he misses), spcombo2 (Club hint, A lit, landing resets, the Mole done -> next hint; POTION so the camp's Romans
  don't knock Asterix about), spicons33 -Level 3 (26 texts rewritten with the option, none without), sppotion2
  (Obelix leading: 388 = 0x1004, speed 15, health kept; off: hit 15 -> 13), spsecond2 (XXL_DOWNAT=0:3000
  XXL_REVIVEHOLD=2: Obelix plays on, Asterix stays at sit 0E unmoved, B revives, lead back), spsecond2c (Obelix
  down as the AI: stays, revived, follows), spsecond2g (both down: game over, respawn), spsecond2h (XXL_HPAT:
  the area hit takes the last point), spcork21d / spcork21e (Obelix down after 03C8 freed Asterix: the sequence
  set back; Obelix down with Asterix still on the circle: the lock undone, Asterix on solid ground, revives him).
  Co-op unchanged: combohint2, downcombo.
- Round 200 (user's pad test of round 199 -- switching and the revive softlock cases fine; notes and choices).
  Config: [single_player] sp_isolation (on): single player is the game's own -- config.cpp clears every sp_ flag
  (the ini's values logged), and the general options' single player parts ask CoopConfig::SoloMod() (obelix_potion,
  cheats and the mod buttons; difficulty_coop_only taken as on); only the join hint stays. sp_combo_hints /
  sp_coop_icons / sp_hero_switch on by default, sp_second_chance off; sp_obelix_potion gone ([coop] obelix_potion
  covers single player with SoloMod). Env aids XXL_SPISOLATION, XXL_SPSWITCHLOCK (XXL_SPPOTION gone).
  (1) The hop over a downed hero: the fallen one is kept in the player variant (SoloBeforeLifeTick), so the leader
  walking into it was "touched by a player's hero" -- pushed aside and hopping (FUN_0049CDF0, vertical speed 12; round
  44). HeroSendEventHook's contact rule (the other hero passes for a waiting sidekick: +0x18 bit 0, +0x561 = 3) now
  also runs without player 2 while a hero is down (user: the sidekick's own hop stays). Watch: S_WATCHRW x 7 is armed
  on the watchdog's next 2 s tick -- set it well before the move (one watch at a time: a second replaces it).
  (2) The revive countdown: hints.cpp's player 2 box is a second box at the bottom left without player 2
  (HINT_LEFT_BOX, own hints only -- not HINT_BOTH); TimeLimit shows there in single player (in player 1's box it
  took turns with the revive bar, same key HINT_REVIVE, every 250 ms). The potion's flask (LayoutFlask) keeps the
  game's corner and rises over it as in co-op.
  (3) sp_hero_switch_lock (with sp_hero_switch): event groups are flagged by inlined code in many places
  (CKHkActivator's lever among them: 0x460C20's detour never saw it), so the groups are known at their dispatch:
  CKSrvEvent::vf17 (vtable 0x624FF0 slot 17, 0x46B180, thiscall(arg), RET 4 -- wrapped with no argument it
  crashed at startup) wrapped -- the groups about to run noted (table +0xC: 3 bytes per group, count / mask /
  flags; first set +0x1C groups run with any flag, the next +0x1E and +0x20 with flags == mask); a trio switch
  (4800/4801/48B7/48B8) from the dispatch (callers 0x46B1F3 / 0x46B276 / 0x46B2FA) is dropped when the groups under
  way that send it only switch (GroupOnlySwitches: trio messages only switches, 0x4803, 0x2000; no cutscene).
  coop.cpp SoloSwitchLockWatch: `choice` = the leader at the level's start, then the switch button / R3 + X; the
  leader not the choice and the choice free (SoloSwitchRefusal null) for 0.3 s -> CoopGiveLead(choice); "too far"
  for 3 s -> the leader becomes the choice; nothing while a hero is down or the choice's lock sequence goes on
  (g_seqHero == choice && SequenceUnderWay: LV2-1, 03C8 frees Asterix but 0528 at the turnstile walks "the
  companion" onto the platform -- given the lead back, Obelix was sent there). LocksTracked() (p2, second chance,
  cheats in single player, the switch lock) gates the lock bookkeeping (NoteTrioEventFlagged, 4802/4803, the AI's
  rides, the pending lead). The automatic cable car (CKHkTelepher) sends no switch: the rider keeps the lead.
  (4) Second chance + the co-op rules: GuardFrame / WatchPendingLead also run in the single player frame, guard.cpp's
  gates take IndivActive (romans_guard_downed); the single player bundle loop puts a catapult's blow on its hitter's
  combo bar (CatapultHitter, as TickLife).
  (5) Two health rows (hud2.cpp SoloRows: second chance, no merchant open): the game's HUD is the leader's (HudOwner
  -> the trio leader; hints.cpp's tick enters Hud2HudHero()), the other hero's health a copy ticked as player 2's
  (s2, health part only, the HUD button and "changed" shown on both rows); when the lead changes hands
  SoloBeforeTick swaps s1 / s2 (and their changed / pending bookkeeping). Draw: DrawHudHook runs Hud2BeginP1 /
  the game's / DrawP2Hud in single player too; Obelix's row kRowGap (0.085 of the screen's height) under
  Asterix's, whichever leads (Xf3d on elements 0-30). Colours (tint.cpp): the shields' textures (X8R8G8B8 64x64 /
  32x32 here; DXT1/3/5 by end colours, 565 / 1555 / 4444 too) copied per hero into managed textures, each texel
  0.2 itself + 0.8 luma x colour x 1.75 (Asterix 1 / .32 / .26, Obelix .30 / .52 / 1); bound in the original's
  place by a wrapper on IDirect3DDevice9::SetTexture (device 0x668978, vtable slot 65). Learned in the other
  row's pass (only its shields and chips drawn), swapped in the game's pass by that list. RenderWare's stage cache
  (RwRaster* at 0x668600 + 24 * stage, FUN_0058BE30; raster ext at DAT_0066FD7C: +0 the IDirect3DTexture9) is
  cleared, with the device's stage 0, at each tinted pass's start and end.
  (6) The mod buttons (R3 + A / B included) only with [debug] cheats; single player (SoloModButtons, SoloMod): R3 +
  A brings the other hero (CoopJoinSpot; down: you to it), R3 + B undoes its lock (UnlockByPlayer) or frees it from
  a scripted order (4803), R3 + X forces the switch, Y / D-pad as in co-op (ModCheats). Autotest step S_SPMOD.
  Not done (asked): Obelix alone at the machine gun in single player. Autotests (level 2 unless noted; XXL_
  SPISOLATION=0): spbump2 (XXL_SPSECOND=1 XXL_DOWNAT=0:3000; downed Asterix put 2.5 ahead of running Obelix --
  without the fix the watch saw the hop's 12, with it none, Obelix stops against him), spbump2b, splever25 (the
  lever's 48B7 from event 0168's group dropped, Obelix keeps the lead, the light pillar lit), splock21 (0050 kept,
  03C8: "sequence under way", 0528 walks Asterix onto the platform), sptele36 -Level 3 (the automatic cable car:
  Asterix leads all the ride, the switch refused), spmod2 (R3 + B / A / X / Y), spguard41 -Level 4 (XXL_DOWNAT=0:13000
  XXL_OBELIXPOTION=0: 5 guards, fight Obelix within 5, back at 6.5+), sphud2 (rows, tint, the switch), spsecond2
  (countdown left, bar right), sppotion2 (XXL_SPSWITCHLOCK=0: its plain 4801 is a no-lock switch), spcork21d,
  spswitch2 (isolated: nothing; not: as before). Co-op unchanged: bump2, hudhints.
- Round 201 (user's pad test of round 200: all fine; difficulty_coop_only taken as on under isolation approved;
  two sp_hero_switch_lock reports, userlogs 20261102-xxl_coop.cable_car.log / .strange_switch_after_keg.log).
  (1) LV2-1's rope cable car: Asterix's landing at the rope's end crosses zone 0020 (box -780.8..-777.8 x,
  512.9..516.9 y, -1073.4..-1070.4 z), group 004: trio 4800 + 4803 (+ detector offs) -- GroupOnlySwitches took it
  for a plain switch and dropped the 4800; its 4803 went to the companion before the landing set Asterix's AI order
  3, so he stayed the AI companion waiting, never "free", never given the lead back. Now a switch to the hero you
  play (g_solo.choice) is never dropped: the level ends a lock of your hero that way. (2) LV2-1's keg (Asterix
  blows it up after the platform's top): group 069 (event 0348) = 4801 + 4803 + 099A to the platform + 4878
  (Idefix follows); the 4878 made it "more than switches", kept, and the watch handed the lead back 0.3 s later
  (the hop the user saw). Idefix's orders (4877, 4878, 4885..48B6) now count as part of a switch-only group;
  dropped, its 4803 frees Obelix (waiting after 0058's walk). Groups sending trio 4878 in LV2: 069, 0AA (S_FINDGROUPS).
  (3) Obelix's machine gun in single player (user: Asterix runs over, no fire until he is there, off it the lead
  stays Obelix's): coop.cpp SoloNoteMachineUse (NoteMachineUse without player 2, SoloMod) -- Obelix leading, X at
  the free gun, Asterix following within 40 units: ObelixTakesGun, then trio 0x4804 with {Vector3* pos, Vector3*
  dir} (CKGrpTrio_OnEvent48xx case 4: companion AI order 1, +0x568 / +0x574 copied at 0x483AE0) to the gun's frame
  +0x80 / +0x70 (it faces -z at LV3-3 and moves on its rail along x as it turns). SoloGunFrame: re-called when the
  order is lost (2 s) or the gun moved over 1 unit (0.5 s); within 3.5 units (its place is inside its body: he gets
  2.5-3.1 off, on top of it at worst) Obelix steps off (ObelixLeavesGun), CoopGiveLead(Asterix) with both normal, the
  next frame trio +0x48 = the gun, +0x3F4 kind 2 | 8 and 0x4809 (Obelix's AI state +0x561 forced 0 for it: case 9
  sends him to the handles only from 0/4/5/0x0B; it was 0x0F) -- the game's gun, Obelix f18 2B life 5 at the
  handles; both normal again -> CoopGiveLead(Obelix). Not there in 10 s: put 1.5 behind the gun. A (values 4 / 44)
  while alone leaves it. First try switched with Obelix still in the gun's mode: he went to mode 3 (life 6) and
  the switch back put him at a garbage position -- the game froze. shared.cpp GetActionHook: the gun map ("Astérix
  Baliste") action 2 (fire) answered 0 while CoopSoloGunAlone (the game's X, Obelix's own, fired it); PromptHook
  hides the machine "Use" then. (4) The co-op Twister push-through (round 52) for the hero you lead (SoloMod).
  Autotests spgun33 -Level 3 (Asterix held back 3 s by FORCE -- which clears his AI order: the re-call path; no
  fire alone; joined, fired, A, lead back), splockev2 (S_FLAGEV 0020 with Asterix held: kept, he leads; S_FLAGEV
  0348 with Obelix held: dropped, Asterix keeps the lead, Obelix order 0), spfindkeg2; splockcar21 (rope car: S_CARPOS
  never reaches the ride's end -- the landing itself not covered); splever25, splock21, co-op obelixgun33 unchanged.
- Round 202 (user: round 201's cases fixed; three reports, userlogs 20261102-xxl_coop.lv3-5_strange_landing_switches.log).
  (1) Second chance on the belly slide: a fall started a revive -- IndivHeroDown's game over at once only knew
  co-op's CoopSuspended (the boat's death went the game's own way). Now also g.solo with either hero in mode 3
  (slide) or 1 (boat). The slide no longer starts from slidecross's sector load (its cross detector, z -903.82,
  waits for LV2-2's opening); spslidemode2 checks the rule: Obelix's f18 0x0D (mode 3) 50 ms around Asterix's
  XXL_DOWNAT -> "down too ... during a scripted mode -- game over".
  (2) LV3-4's launcher to LV3-5, two hops of the lead: (a) zone 0B10's "Asterix leads" arrived mid-flight
  (CoopDeferTrioEvent: deferred to the landing, then re-sent by the mod -- out of the dispatch) and (b) event
  0850's group 10A (27 targets: CKHkLight 0999/099B, trio 4800 + 4803, detectors, camera sectors 0700, a
  CKManageEventCinematicBloc 0x624C2C <- 0501 -- round 30: it starts its node, a music stream) was kept as "plays a
  cutscene"; both handed back 0.3 s later. GroupOnlySwitches no longer asks GroupPlaysCutscene (targets up to 64);
  what keeps a switch is the group's own trio orders. Dropping a switch the level relies on later (LV2-10's boss: #34
  "Asterix leads" + 4803, then #4's 4829 "the sidekick walks to marker 22" -- meant for Obelix) is made safe:
  g_solo.levelLeader = the hero every group switch from the dispatch means to lead (dropped or kept);
  SoloOrderNeedsLead (CoopDeferTrioEvent, before the event runs): a group order for the sidekick (4802,
  4813..4876, 48BB..48EC) while the other hero leads -> CoopGiveLead(levelLeader) first, the order reaches the
  hero the level meant, a hold of yours the watch gives back. splockord2: S_FLAGEV 0020 with Obelix played (dropped),
  then 0528 (its group 0A5: 482D + detectors + a camera bloc): "Asterix leads for it first", Obelix order 1.
  (3) The Twister's camera (user: no pull-back or shake in LV3-4's arena, after player 2 had joined and left at the
  level's start): sptwist2 / sptwist2j (S_CAMFOV: renderer fov svc +0x18 -> +0x28, the view window, the shake at
  +0xC bit 0x1000 / +0x668..+0x680): LV2, with or without a join first, fov 70 -> 120, view window 0.934 -> 2.309,
  shake 24 s / 0.05 -- the round-200 Twister push change (contact handler, push vector only) leaves it alone.
  sptwist34 (-Level 3, the arena via S_FLAGEV 0578: an activator, the squad, 4821, a music bloc): no Roman came to
  Asterix in 20 s (S_ENEMY found none visible), no Twister -- not reproduced; isolated and not the same.
- Round 203 (user: round 202's launcher landing fixed; the LV3-4 arena's Twister has no camera pull-back in the
  game itself either -- sp_isolation on and without the asi, only the shake; userlogs
  20261102-xxl_coop.twister_fusion.log, 20261102-xxl_coop.lv3-5_softlock.log).
  (1) Single player with second chance: no Twister Fusion. The attack table (hero +0x130, built by FUN_0048CA00,
  shared by every hero): 9 = 0x493DD0 the plain Twister, 10 = 0x493F70 the Fusion-ready one. FUN_004963D0 gives 10
  only with the combo bar full (stats +0x10 bit 0x4000) and the Fusion learned (bit 8). The chooser (FUN_00494DB0)
  runs every tick; its AI branch copies the leader's Twister (+0x135 9 / 10) by calling FUN_004963D0 itself -- with
  individual heroes that reads the companion's own bar (Use() swaps it in for its tick): empty, 9. Attack 9 never
  runs FUN_00498560 (the companion's spin step in 0x493F70 case 0x23, which marks both heroes' "partner near" +0x189
  within trio +0x3D8), so the leader's X (Hero_ProcessPlayerInput 0x49971A: +0x135 == 10, +0x3EC bit 0x10,
  +0x189 -> request +0x188) found +0x189 0 (S_WATCHRW: read at 0x49972E, never written). ChooseAttackHook: single
  player, IndivActive, an AI hero whose chooser returns 9 while the leader is in 10 and it is not already in 9 ->
  10, every frame (a first-frame-only 10 was put back to 9 on the next tick: no access to Obelix's +0x189 at all).
  The companion's own entry spends nothing (f18 & 0x1E), the fusion's spend (FUN_00493810(3)) is the leader's.
  spfuse2 (XXL_SPSECOND=1): +0x189 written at 0x498692, "Asterix did the Twister Fusion (combo 3)"; XXL_SPSECOND=0
  fuses as before (the game's own path).
  (2) LV3-5's corkscrew softlock (sp_hero_switch_lock, you play Asterix): the cable car's end (zone 0C38: 4800,
  48C8 the sidekick put on a marker, 4802, camera 0701, 4891) puts Obelix on the corkscrew, waiting; Asterix's
  turnstile hits raise it; at the top 0C08's 484C jumps him off (AI order 2, AI_JUMP_TO_POS) into box 0340 (the
  user's screenshot: group 068 = 4801 "switch to Obelix" + 4803 "enable sidekick movement" + detectors + camera
  sector 0700). 4801 was dropped (switch-only group, to the other hero); the group's 4803 then reached Obelix
  mid-jump: the trio's 48xx handler (CKGrpTrio_OnEvent48xx 0x482BB0, cases 2 / 3 = 4802 / 4803) changes the
  companion's order only when FUN_00481810 allows it -- not while it is 2 (a jump) -- and the jump ended in order 3:
  held for good. In the game the switch made him the player's hero, so no wait was meant for him. Now SoloDropsSwitch
  notes a dropped switch's hero with an order other than follow (g_solo.release), and SoloReleaseWatch (from
  SoloSwitchLockWatch) sends trio 4803 once it waits (order 3), not after 10 s, once it leads, or after a later group
  order for the sidekick (SoloOrderNeedsLead clears it). splockcork35 (-Level 3, S_FLAGEV 0C38, Asterix at the user's
  turnstile spot -143.4 5.5 -257.7 hitting X): 0C08, 0340 dropped, "Obelix ... waits where the level held it --
  trio 4803", order 0, he follows (before: order 3 to the end). LV2-1's mirror (splockcork21, you play Obelix,
  S_FLAGEV 0050 with Asterix on the circle, Obelix through 03C8 to the turnstile): the top box 0058 (4800 + 4828
  "the sidekick walks to a marker") holds Obelix, so its switch is kept -- Asterix leads, Obelix waits at the
  marker as in the game; no softlock there. splockord2, splock21, splockev2 unchanged.
- Round 204 (user: round 203's LV3-5 corkscrew fixed, the Fusion works with second chance, the belly-slide death
  works; userlogs 20261102-xxl_coop.prolonged_twister_fusion_isolation_on.log,
  20261102-xxl_coop.lv2-10_boss_doesnt_activate_as_obelix.log, 20261102-xxl_coop.lv2-10_boss_activates_as_asterix.log,
  20261102-xxl_coop.lv2-4_squad_unintended_reinforcement.log).
  (1) The Twister Fusion's cost. Measured (spbar2, the game's own): the Twister's entry spends 23 of the 30 (gm +0xA4
  6.5 left, FUN_00493810 / the plain one's inline spend; only a hero with f18 & 0x1E == 0 pays), the full bar's rest
  drains ~1 a second, out at ~6.5 s; the leader's spin lasts trio +0x3AC (10 s, from +0x164 set at the end of the
  0x22 start step FUN_00497B90). The AI companion's chooser (FUN_00494DB0's AI branch) re-asks FUN_004963D0 every
  tick while the leader twists (not in 0x25-0x27): bar out -> 9, FUN_00491FE0 exits 10 (FUN_00498D70) and enters 9:
  the companion's start pose again (0x22, 2 s), then a fresh plain spin -- the user's "re-do". The leader's +0x189
  stays 1, and the request (Hero_ProcessPlayerInput 0x49971A: own +0x135 10, X +0x3EC bit 0x10, +0x189) is all the
  game asks: the leader's FUN_004984F0 sees the partner at 0x23 (the plain Twister's spin is 0x23 too) -> 0x25 and the
  Fusion's spend (nothing: bit 0x4000 clear); the plain Twister has no merge step, so the leader merged (0x25) until
  the companion's spin ended (9 s), then 0x26 (FUN_00498A30) ran a whole trio +0x3AC from the merge's +0x164 = now:
  spbarx2 (X at 12-13.2 s): the Twister from 2.5 s to ~33 s, free (user's log, isolated: Asterix's Twister body for 20
  s, no "did the Twister Fusion" -- the combo hints see the spend). Round 203's forced 10 made it worse with second
  chance (Fusion-ready to the leader's end, X free).
  Now: ChooseAttackHook -- single player, individual heroes, an AI hero while the leader twists: the chooser runs with
  the leader's stats in place (IndivEnter(leader) / IndivLeave), its FUN_004963D0 asks the leader's bar as the game's
  one bar (10 while some is left, then 9 as in the game). FuseHook (co-op, or single player with the mod's features;
  isolated single player stays the game's own): a request (the hero's own +0x188, or the leader's the AI follows)
  only when CoopFusionAllowed(leader) -- both heroes in attack 10 and the leader's bar full (IndivStats' gauge >= 0,
  else stats +0x10 bit 0x4000); else both requests cleared, "Twister Fusion refused". The combo hints' "X fuses
  them" asks the same. spbarx2 (XXL_SPISOLATION=0, second chance 0 / 1): refused x4, the Twister ends at 14.6 s;
  second chance: the companion 10 then 9 at the bar's end (9.06 s) as in the game. spfuse2 (second chance 1 / 0) and
  fuse2 (co-op) fuse as before. LogTick shows the bar (gm +0xA4 / +0xA0, full) and a Twister's age (tw), n +0x189, r
  +0x188.
  (2) LV2-10 / LV3-6 bosses with sp_hero_switch_lock, you play Obelix. LV2-10: checkpoint 05A0 (CKGrpAsterixCheckpoint
  0x536FED: 4800 + 4803 + pillar) and zone 0498 (#34 at x 1672.3: the intro, group 093 4C18 + 4800 + 4803) and 00A0
  (#4 at 1687.1: group 014, boss 4C18 / 4C15, 4829 the sidekick to marker 22, camera 0701) -- #34 and #4 bound to
  Asterix: Obelix crossed both, nothing; the AI Asterix behind him set them off (spboss210 before: 0498 at Asterix's
  x 1672.68, 00A0 when he reached 1687). The intro freezes Asterix (as in the game, where he is the player: user's
  Asterix log, 1672.49 through it); the Obelix you play walked on in it (p1 stick: 1678 -> 1684). Then 4829 lent the
  lead to Asterix (round 202's SoloOrderNeedsLead) -- he stood at the gate, the boss went for Obelix alone at marker
  22 (user). Now: (a) g_solo.levelLeader also starts as the level's first leader (SoloSwitchLockWatch's "you play");
  CoopSoloLevelLeaderAway() = the level's leader when it is the AI companion. DetectTickHook -> SoloZonesTick: the
  enabled tests bound to that hero, minus "while outside" and ZoneMeansItsHero (orders for the hero itself), are off
  in the engine's pass and run alone in a second one with the leader's node and last place in the companion's slot
  (as co-op's swapped pass; g_inSoloPass, "(the level's leader's zone, for the hero you play)"). (b) SoloBeforeLifeTick:
  the leader while CutsceneBars() and roles swapped gets a zeroed action values array for its tick ("holds still").
  (c) SoloOrderNeedsLead: an order from a boss run's group (ProximityDispatchInBossRun: GroupStartsBossRun -- 48EE /
  48F1 + a marker walk -- or a group of g_bossRun, now collected in single player too while the roles are swapped)
  is not lent: it reaches the companion (user's wish: Obelix leads, Asterix as the sidekick goes to the marker; the
  walk ends in a wait, order 3, at marker 22 in the arena -- where the game's Obelix waits). spboss210 (XXL_SPISOLATION=0
  XXL_LOGFLAGS=1): 0498 and 00A0 by Obelix, Obelix still through the intro, "a boss run's order for the sidekick
  (4829) -- Asterix goes, Obelix keeps the lead", Asterix walks to (1712.8 -747.8 -1143.1), the spiked roller after
  Obelix in the canyon. spboss36 (-Level 3): 0388 and 05D8 by Obelix, boss run 0BB 0D3 0D4 (walks 4825 4823 4824),
  4824 and 4825 to Asterix running ahead, Obelix keeps the lead (the test's Obelix stops and is crushed). Not tested:
  the LV2-10 fight's later steps (the automatic cable car to the boss's button is Asterix's in the game; the boss's own
  4801 / 4802 are not group switches -- kept). splockord2 (482D still lent), splockev2, splockcork21, splock21,
  splockcork35 unchanged.
  (3) LV2-4's "reinforcements the counter did not count" (user's log: the arena's gate, 2 squads, 60 Romans, from
  648.98 s; no rise logged; Obelix played from 672.7 s). spsq24 (S_SQUADS round the arena): the gate's squads are
  03DC78DC (box x -49..42.3, z -616.3..-542.2: 47 + 8) and 03DC7A40 (1 + 4); not the gate's (+0x154 -2) and reaching
  the arena: 03DC7E6C (x -65.1..-30.9, z -583.2..-545.9: 4 placed) and 03DC9A3C (x -34..51.1, z -549.1..-489.7: 15 + 2,
  4 + 1 placed -- the next section's). The user's Obelix took the shield in the arena's north passage at 711.1 s,
  at (-24.1 2.1 -548.5): inside 03DC9A3C's box. spsq24n (XXL_SPISOLATION=1, the game's own): Asterix there wakes it
  (active 1, sent 4 + 1), the gate counter stays 60 -- the game's own level layout, nothing of the mod's.
- Round 205 (user: round 204's fixes work; userlogs 20261102-xxl_coop.lv3-6_boss_jump_{far,near}_as_{asterix,obelix}.log,
  ..._specific_jump_over_detector_as_obelix.log; XXL-Editor screenshots Snipaste_2026-11-02_19-06-30 / 19-11-21).
  (1) LV3-6's boss run with the roles swapped (sp_hero_switch_lock, you play Obelix): after the far jump off the
  platform (-229.6 13.6 -181.4) the boss never rolled. Detectors: #46 (On Cross Rectangle, centre -232.0 9.8 -173.9,
  15 x 10, direction 2: flat -- the hero falling through y 9.8; node index 0 = Asterix's slot; group 0BB / event 05D8:
  disable 46, PF transition, enable 52, cutscene 0501, enable 143, 4824 the sidekick to marker 17, 48EE / 48F1) and
  #143 (centre -235.8 11.5 -174.7, 9.5 x 10.28, direction 0: the plane x -235.8; node index 1 = Obelix's; group 0x70
  / event 0380: the boss 4C15 "move to the other end", disable 143) -- #27 is the same rectangle on Asterix's slot,
  no event. The far jump crosses x -235.8 above the arena before falling through #46, so #143 is switched on behind
  it; in the game the move comes from the walked Obelix passing #143 (user's far-jump-as-Asterix log: 0380 at
  24.86 s, Asterix landed at 23.5 s, Obelix coming down; the user: Asterix's far jump waits for Obelix too). Round
  204 sent the run's walk to Asterix, not bound to #143: no 0380 at all (spboss36j before). Now BossRun keeps the
  detector tests its groups enable (AddBossRunGroup: ops 0x14..0x18, list << 8 | index; #143 and #52 from 0BB), and
  SoloZonesTick runs those bound to the hero you play in its second pass too, with the companion's node in that slot
  (moved = 2: answered by both). spboss36j (-Level 3, XXL_SPISOLATION=0: Obelix glides the far jump, -238 11.5 -178
  then -240 5.6 -177): "zone event 0380 (... set off by Asterix)", the roller after Obelix. FlagEventHook's solo-pass
  line names the hero (ZoneEventHero). spboss210, spboss36 unchanged.
  (2) Second chance, Obelix down, Asterix hit: Obelix's row "went down like the HUD just appeared". The rows show
  together (SoloBeforeTick: either hero's change); the health part (FUN_0051A140) slides a row in (state 1) and in
  state 2 only goes on with health or a last shown +0x270 -- 0 and 0: hidden at once (the game's own HUD at a death).
  XXL_HUDPOS now logs the other row (bits, +0x270, +0x234, shown models): 0x10 -> 0x20 -> 0x00 within 15 ms on each
  hit. Now Hud2AfterInterfaceTick clears the show flags for a fallen other row (and its pending): it stays hidden;
  after the revive it shows again (spdownhud2, XXL_SPSECOND=1 XXL_DOWNAT=1:2000, and with XXL_UPAT=6000).
- Round 206 (user, nolock_romans: LV3-6 squad #29's Romans, sent at the waiting hero, still walked slowly and
  oddly and never attacked; maybe also LV3-1 #7 / LV3-2 #0). Cause: the squad's tick copies its current
  choreography key's flags (CKChoreoKey +0x1C: bits 0-2 squad rotation, 3-5 enemy attack, 6-7 enemy movement;
  key +4 slots 0x1C each, +0x10/+0x14 duration min/max, +0x18 share at position) into each Roman's slot
  (FUN_0052B670: slot +0x24 bits 0-2 = attack, bits 3-4 = movement; slot = hook +0x1AC, vf12(0x46)). #29's key
  (XXL-Editor: "only attack when in position", "Walk") gives attack 0 movement 0 -- a patrol: the Romans walked
  after Obelix and never went for him. Fix: NoLockSteer (NoLockAttackMode) sets our Romans' slot to attack 3
  (approach and attack) unless 3/5, movement 1 (run) if walk/static (sprint kept). Logged once a Roman on change.
  nolock36f (-Level 3, XXL_NOLOCK=1 XXL_NOLOCKROMANS=1 XXL_NOLOCKROMANSANY=1 XXL_OBELIXPOTION=0
  XXL_NOLOCKPAUSE=4000 XXL_NOLOCKFIRST=2000 XXL_NOLOCKCORK=0): "key has attack 0 movement 0 -- made attack 3
  movement 1" for all 3; Obelix's health 15 -> 8 as he walks round (user watched: "behaves normal now").
  Not checked why squad #4 (also "Walk" keys) looked right -- likely its fight switches it to another key.
- Round 207 (user, LV3-2: after Obelix finished pulling the big block / pushing a log, his prompt box mirrored
  Asterix's "Light" at a hearth and kept it after Asterix walked off; xxl_coop.lv3-2_prompt_light.log). Cause:
  FUN_0049A1A0 (the hero's HUD prompt) writes nothing for a hero in the push/pull situation (+0x3E9 == 1 or
  +0x3F0 bit 0x20) whose block step +0x488 is none of 1 (Push B) / 2 (Pull C) / 3 (A) -- a block moved to its
  end gives 0. Game manager +0xCC bits 4-7 is one field, and PromptHook puts the frame's prompt back into it
  after each hero's call, so Obelix's call read Asterix's leftover kind 8 as his own ("both offered"). Fix:
  PromptHook sets the field to 0xE (nothing) before a player hero's call in co-op. prompt32 (-Level 3; zone
  00E0 at 187.1 -189.0 gives Obelix the lead, X + right pulls, Obelix glides off the block and back while
  Asterix with a torch stands at the hearth 177.7 -147.9): before, "player 2's box (right): prompt Light"
  and it stayed after Asterix left; after, P2's box empties at the pull's end and never shows "Light".
- Round 208 (user, sp_hero_switch_lock: four unintended switches). (1) LV4-3 light pillar as Obelix
  (xxl_coop.sp_switch_lock_lv4-3.log): zone 00D9's group 01B (4800 + 4803) was dropped, but the second group
  0E7 (event 0738: 4800 + 4803 + 487D + 4811, pillar 0900, camera, detection) was kept -- Asterix led 0.3 s.
  Trio 0x4811 = Asterix +0x3EC |= 0x400000 (his torch used up: CKHkAsterixLife::InitOnce sends 0x513C to the
  item at +0x464), 0x487D / 0x487E = the sidekick's / Idefix's +0x884 |= 0x400 (FUN_004A0xxx: a hop to a
  pathfinding spot by the leader) -- nobody held; GroupOnlySwitches now skips them. splock43 (-Level 4): both
  "ignored -- Obelix keeps the lead". (2) LV4-4 as Asterix: the rope cable car's boarding (game code, kept)
  gives Obelix the lead; levelLeader stayed Asterix, so CoopSoloLevelLeaderAway said "you play the sidekick"
  and Asterix-bound zones answered Obelix -- sphere #4 (-733.9 18.5 -237.9, event 0650: fight lock 090F on
  the spitter's lever, 0D17 pools, disable itself) at the ride's end never fired (lock off: 0650 + the fight).
  Now CoopSoloLevelLeaderAway needs the leader to be `choice`. splockcar44 (-Level 4: S_PULLLEVER the foot's
  lever -681.8 9.4 -214.4 -- fight-locked: ENDFIGHTWON -- again, the car comes back, Asterix glides on, S_CARPOS
  along the rope): after boarding no "zones for Asterix answer Obelix"; S_CARPOS doesn't end a rope ride, so
  the sphere itself was not reached. (3)+(4) LV4-7 both logs: plane #17 (-972.8 247.5 -1069.1, event 0448,
  bound to Obelix: 4818 "the sidekick walks to marker 5" + 1616 plane #22 on). Planes there (S_DETECT, list
  +0x3C, obj 0 Asterix / 1 Obelix): #20 0480 Obelix (4801 + 4803 + 099A CKHkTelepher + 161B), #21 0488 Asterix
  (4802 + 1D1B), #22 0490 Asterix, armed by 0448 (099A + 1701 + 48CD + 1D16). Playing Obelix: SoloOrderNeedsLead
  gave Asterix the lead so Obelix walked (levelLeader Asterix from the level start), or, levelLeader Obelix
  from a dropped 4801 20 min earlier (event 0318), walked Asterix -- who then waited at the car's foot and was
  locked into it. Now a sidekick walk (4813..4876) whose zone is bound to the hero you play, leading
  (DispatchWalkMeans: the tests carrying the dispatched group's event, by NodeIndex) is dropped and
  levelLeader set to that hero -- unless the other hero's lock sequence goes on (g_seqHero, SequenceUnderWay):
  LV2-1's 0528 (box #19, also bound to Obelix) walks the freed Asterix onto the spinning platform in the game.
  The zones the group turns on did not tell (LV2-1's 0530 / 0058 are Asterix's own). splock47 (-Level 4):
  "walk dropped", Obelix glides the walk's top path, 0480 then 0488 (Asterix waits) -- what starts this car
  was not found (Obelix on its seat -924.9 227.6 -1019.5 did not ride). splockcork21 unchanged (0528 walks
  Asterix, 0530, 0058 at the same times as Nov 2); splockord2 (no circle, no sequence) now drops 0528's walk
  instead of round 202's "Asterix leads first"; splock21, spboss210 unchanged.
- Round 209 (user, LV4-7 with sp_hero_switch_lock: no switch to Asterix now, but Asterix not at his marker
  by the automatic car, no camera, and the lead given back to Obelix at the ride's end; lock off
  (xxl_coop.lv4-7_switch_lock_off.log): Obelix keeps the lead at 0448, Asterix walked, Asterix keeps it after
  the ride). Round 208 misread 0448: zone 0448 (plane #17, Obelix's) sends 4818 "the sidekick walks to marker
  5" -- with Obelix leading that is Asterix, walked to the car's foot (-930.5 227.6 -1021.0); round 208 dropped
  it. The earlier "switch to Asterix" came from SoloOrderNeedsLead: the level's leader was Asterix (level start;
  the user's manual switch to Obelix does not change it -- it must not: LV2-10's boss gate, bound to Asterix,
  answers Obelix through it, spboss210). A zone bound to one hero orders the other (LV2-1's 0528, Obelix's,
  walks Asterix too): SoloOrderNeedsLead now lets an order from a zone bound to the leader through as sent
  (ProximityDispatchZoneHero: the hero bound to the tests carrying the dispatched group's event). The lever
  (-941.7 254.9 -1067.2, CKHkActivator, fight-locked; event 0230: CKHkSlideDoor 0900 + plane test 2C on): the
  car drops for Asterix ("Obelix locked by the level", kept). Ride's end: Asterix drops off the top through
  #147 (-934.6 256.6 -1019.6, horizontal, off at start, on with 0468; event 0AF0 = sequence #350: 4803 + itself
  off) -- Obelix freed, the lock gave him the lead back. Now SoloFreedByLeadersZone: 4803 alone (no switch in
  the group) from a zone bound to the leader while the hero you play is held -> handedTo; SoloSwitchLockWatch,
  past a lock sequence still going on (LV2-1's 03C8, Obelix's, frees the circle's Asterix for the turnstile:
  SequenceUnderWay only after its group), makes the leader the hero you play. show_leader tag: "LOCK: ON (hero)"
  / "LOCK: OFF" in single player (CoopSoloLockedHero; glyphs C K N). Autotests: XXL_SPSWITCH=1 turns the lock
  off (sp_hero_switch_lock defaults off with it) -- use XXL_SPSWITCHLOCK=1; the user's ini may have it off.
  splock47 (-Level 4): 4818 "as sent", Asterix at the marker, Obelix punches the lever himself (PLACEAT the
  user's -941.576 254.837 -1068.224, S_NEAROBJ class 52 to face it, P1 X -- S_PULLLEVER fires 0230 but the car
  did not drop), Asterix on the car (-932.601 229.270 -1021.889), rides, down through #147: "now the hero you
  play" Asterix, Obelix follows. splock43, splockcar44, spboss210, splockcork21, splock21 as before; splockord2:
  0528's walk as sent (Asterix).
- Round 210 (user, sp_hero_switch_lock: LV5-3's launcher -341.2 14.1 151.5 as Obelix went Obelix -> Asterix ->
  Obelix; LV5-5's keg order switched Asterix -> Obelix -> Asterix in a full play (xxl_coop.lv5-5_switch.log) but
  not from a fresh level load (_didnt_repro.log)). One cause: the launcher's landing switch is the game's own code,
  not an event group -- CKHkTrioCatapult tick (vf15 0x4C2AB0, send at 0x4C2B26): with its +0x15 bit 1 set it sends
  4800 if its +0x14 bit 2 (Asterix's launcher) and Obelix leads, 4801 the other way, then FUN_00460c20(+0x1C) its
  event. SoloDropsSwitch only took switches from the dispatch (0x46B180..0x46B331): the 4800 came mid-flight,
  CoopDeferTrioEvent deferred it to the landing (the deferral log now names the caller), it was sent, and the
  watch gave Obelix back 0.3 s later. And g_solo.levelLeader was never set by it: in the user's full play it
  stayed Obelix from event 07E0's dropped 4801 (LV5-3, 218 s) to LV5-5 (989 s), where the keg's 4815 ("the
  sidekick ...", not from a zone) made SoloOrderNeedsLead give Obelix the lead for it; a fresh LV5 load starts
  with 05E8's 4800 (levelLeader Asterix): no switch. Now SoloDropsSwitch also takes the launcher's caller
  range: levelLeader = its hero, the switch dropped unless it is to the hero you play / the leader. splaunch53
  (-Level 5, XXL_SPSWITCHLOCK=1, S_SPSWITCH to Obelix, X on the launcher): "the launcher's landing switch 4800
  ignored -- Obelix keeps the lead", landed at (360 12 42), Asterix order 0. Was (same test, before):
  "trio event 4800 from 004C2B2E ... after the landing", then "Obelix takes the lead (... free again)". LV5-5's
  keg not autotested (its levelLeader is now Asterix after the launcher, as on a fresh load).
- Round 211 (user, sp_hero_switch_lock: LV2-6, xxl_coop.lv2-6_question.log). You play Asterix; his lever (98 s)
  locks him (Obelix leads, Asterix waits); zone 01D8 (Obelix's, 4803 alone) frees him -- round 209's
  SoloFreedByLeadersZone made Obelix "the hero you play" for good, so the last block's 01E8 (4800 + 4803, 130 s)
  was a switch away from your hero: ignored, Obelix kept the lead (lock off: Asterix leads there). Now the handedTo
  state keeps choice: the leader keeps the lead (no give-back, levelLeader = it) until your hero leads again --
  the level's later switch to it is kept (SoloDropsSwitch: to == choice) -- or your own switch. LV4-7 unchanged
  (no later switch to Obelix there). splock26 (-Level 2, sector 6: S_TRIOEV 4801 + 4802, S_FLAGEV 01D8, then
  01E8): "it keeps the lead until the level hands it back", then Asterix leads after 01E8. splock47: Asterix keeps
  the lead after 0AF0. LV2-5 (user question, xxl_coop.lv2-5_after_rotarybeam_trasport_sequence_switch.log): the
  lever #419's 48B7 + 4803 after the rotary beam gives the lead to Asterix -- a switch to the hero you play ends
  its lock, kept as designed (no change).
- Round 212 (user, single player). (1) LV2-6 Obelix walked back to his marker and waited for good
  (xxl_coop.lv2-6_walk_to_obelix_stuck.log): the torch cheat. CKHkTorch::sendEvent 0x0C07 (taken) flags the
  torch's +0x38 event on every take -- LV2-6's torch (14.9 2.5 -356.0): 0231, 481A "the sidekick walks to marker"
  (Obelix to 6.3 2.0 -355.2, for Asterix's lever #406: 48B8 + 481B, push once). The cheat's take of that torch,
  carried before, at 221.7 s set it off again (trio event count +1, no zone). FUN_00460C20 skips events
  >= 0xFFF8: the cheat now mutes +0x38 (0xFFFF) for a torch already carried this level (g_torch.carried, per
  level), back once taken or given up; a first take keeps its event. Also the cheat's take press (+0x3EC 0x10 at
  FUN_00499CF0) is cleared again when the handler did not turn it into 0x2000 (no torch the take accepts): it
  was an X, Asterix's three-hit attack for 60 frames (user: with every torch used up). Torch +0x3E bits 0-1: 0 on
  the ground, 1 carried, 2 after 0x513C / 0x5127 (still re-takeable by the cheat). sptorch26 (-Level 2, XXL_LOGTRIO=1):
  first cheat take -> 481A; thrown, given again -> "(carried before: its take's event muted)", no 481A.
  (2) LV2-5 Asterix held for good (xxl_coop.lv2-5_asterix_softlock.log): the lever behind the iron crates
  (lever 31 at 134.2 12.1 -573.7, +0x2A 0168: 48B7 + 4803 + pillar) punched by Obelix (your switch) while Asterix
  followed -- the switch dropped by the lock, the push-once lever spent; the hearth's 0D60 (4801 + 4813) then held
  Asterix at its marker with no lever left. The lock-off game softlocks the same way. Safe sequence
  (SoloLeverHeldBack) only knew "switches to the hero leading"; SoloLeverEarly: a lever whose group switches to
  the companion while it follows (AI_FOLLOW_LEADER, not held, not down) takes no hits either. splever25
  (XXL_SPSWITCHLOCK=1): Asterix's punch ignored as before, Obelix's too now (no 48B7, Obelix leads, Asterix order
  0; was: the lead to Asterix). splock47: Obelix's lever with Asterix held at the car's foot works ("Obelix locked
  by the level", the car drops). Open: LV4-7's handedTo (xxl_coop.lv4-7_inf_lead_pend.log) -- choice stays Obelix
  after 0AF0 with nothing in the level handing it back (user calls it a pending that never ends).
- Round 213 (user, sp_hero_switch_lock: no "pending" lead after a level's hand-over -- LV4-7's lever #138, Obelix's,
  4800 + 4802 "disable sidekick movement": Asterix should simply lead, as with the lock off; LV2-6's blocks lever
  the same for Asterix; the last block walks Asterix into 01E8 (4800 + 4803, XXL-Editor Snipaste_2026-11-04_21-56-17 /
  21-57-20) -- the SP game's own switch back). SoloDropsSwitch: a kept switch whose dispatched group also holds the
  hero losing the lead (ProximityDispatchHoldsLoser: trio 4802, 4813..4876 -- the circle's 4849 too --, 48BB..48EC)
  is the level's hand-over: SoloHandOver makes `to` the hero you play (choice), the old one noted in
  g_solo.handedFrom. A later switch-only group to handedFrom while the level holds it (AI order not follow, not
  down) is kept and hands it back (LV2-6 01E8); to any other hero still dropped (LV3-5's corkscrew box 0340 for
  Obelix -- never handed over). handedFrom cleared by your own switch / R3 + X / a new level. The handedTo branch
  (SoloFreedByLeadersZone, a lock whose switch was not a hand-over) now hands over too, instead of round 211's
  endless "keeps the lead until the level hands it back". The 0.3 s give-back ("pending") stays only after
  switches that are not the level's hand-over: the game's own (a cable car's boarding, the belly slide), a group
  with other trio orders, the mod's lending (SoloOrderNeedsLead). Also round 212's SoloLeverEarly now needs the
  lever's group to free someone (GroupOnlySwitches + trio 4803): a hand-over lever (LV2-6's #406: 48B8 + 481B)
  with the other hero following is the game's (it would have been ignored). splock26 rewritten (XXL_LOGTRIO=1): 01B8
  (Obelix waits), Asterix punches the blocks lever (87.0 2.5 -396.2): "hands the lead to Obelix ... the hero you
  play now"; 01D8 frees Asterix, Obelix keeps it; 4802 then 01E8: "hands the lead back to the hero it parked";
  Asterix punches #406 (25.8 3.0 -348.4) with Obelix following: 48B8 + 481B, Obelix yours, Asterix walked (order 1
  -> 3). splock47: 38.6 s "hands the lead to Asterix", at the end Asterix leads, Obelix follows. splock21 / splockcork21:
  the circle's 0050 (4801 + 4802) and the top box 0058 (4800 + 4828) hand over. splever25, splockord2, splockev2,
  spboss210, splock43, splockcork35 unchanged.
- Round 214 (user: round 213 right, but LV3-5's corkscrew box 0340 -- 4801 + 4803, Obelix jumping off -- should
  switch to Obelix under the new rule (the user's round-203 wish withdrawn); the rope cable car's (CKHkTowedTelepher)
  switch is the game's own, Asterix on the bars, back at the dismount -- the pending looks like a hack there).
  (1) A group that holds the other hero (ProximityDispatchHoldsLoser) is a hand-over even with its switch's hero
  already leading: SoloHandOver now notes handedFrom = the other hero (the one parked), not the old choice, and is
  checked before the "leads already / your hero" early return. LV3-5: the rope car's end 0C38 (4800 + 48C8 + 4802)
  notes Obelix; 0340's 4801 while Obelix jumps (order 2) is the hand-back: kept, Obelix the hero you play.
  (2) The game's own switches (outside the dispatch): the rope car's boarding (0x49ACB9 4801, the rider's tick;
  then 0x49ACC7 4812 the ride order) is a hand-over -- Obelix yours and the level's leader (LV4-4's sphere #4 bound
  to Asterix stays unswapped); a game switch back to handedFrom hands it back too. The car's end in each level is a
  zone (LV2-1 0020, LV3-3 0830: 4800 + 4803) -- the hand-back with Asterix still on the car (order 9). The pending
  give-back never fired for rope cars (Asterix held to the end zone); it remains for the belly slide / boat (not
  looked at), groups with other trio orders, and SoloOrderNeedsLead's lending. splockcork35 (XXL_SPSECOND=1):
  "hands the lead back to the hero it parked" at 0340, Obelix leads, Asterix follows (order 0). splockcar21 (+ S_FLAGEV
  0020 at 13 s): boarding -> Obelix, 0020 -> Asterix (on the car, order 9). splockcar44: boarding -> Obelix, no
  "zones for Asterix answer Obelix". splock26, splock47, splock21, splockcork21, splockord2, spboss210 as round 213.
- Round 215 (user: rope cars and LV3-5 fine; LV2-2's belly slide (xxl_coop.lv2-2_belly_slide_lock_switch_fix.log):
  Asterix into the start zone, the game switches to Obelix -- right -- but at the slide's end the lock gave the lead
  back to Asterix (104.3 s, after the end's Roman dialogue); in the game Obelix keeps it after every belly slide. The
  boat: only Asterix starts it and keeps the lead). The slide's start zone 0028 sends 480B to the trio;
  CKGrpTrio_OnEvent48xx (0x482BB0, 3991 bytes) cases 0x0B..0x0F (480B..480F, every hero's f18 mode 3) send their
  own switch through the trio's vf5: 480B / 480D / 480E -> 4801 (480B's from 0x48349A), 480C / 480F -> 4800. Case 7
  (4807, the boat: mode 1) sends no switch. The end zone 0030: 4806 (normal mode) + 48BA (from 0x49BA1D), no switch.
  SoloDropsSwitch: a switch from inside that handler is a hand-over (both heroes in the ride, the new leader yours and
  the level's leader). Every give-back in the user's saved logs was checked: launcher landings, LV4-7 0AF0, LV2-1's
  light pillar 0008 / 0010, LV5-5's keg lending (stale levelLeader), the LV2-2 slide x3 -- all fixed by rounds
  210-215; none left. The pending stays only for the mod's lending (SoloOrderNeedsLead, logged "leads for it
  first"), a kept dispatch switch whose group sends other trio orders with nobody held, and a game switch from code
  not known yet -- both now logged ("... gets the lead back once free (pending)", the latter with its caller).
  splockslide22 (new; XXL_SPSWITCHLOCK=1 XXL_LOGTRIO=1; the slide is steered by hand -- the user did): before,
  "free again" at 87.7 s; after, "the trio's ride mode ... Obelix is the hero you play now" at the start, 0030 at
  61.2 s, Obelix leads to the end (96 s).
- Round 216 (user: the lock is good now; LV2-4 (level 2 sector 3) launcher at -38.80 6.52 -570.38 switched Obelix ->
  Asterix, xxl_coop.lv2-4_launcher_switch.log). Obelix (yours since the belly slide's hand-over: handedFrom Asterix) on
  the launcher (situation 0x0D, waiting) with Asterix swimming far off; the launcher's event 0AB0 (group 156: 48BD,
  the sidekick put on a marker -- Asterix onto it). The level's leader was Asterix (dropped switches 0288 / 0568), so
  SoloOrderNeedsLead lent Asterix the lead "for it"; that CoopGiveLead's 4800 (from the mod, 0x66E26056) went down
  round 214's game-switch branch as "the game hands the lead back to the hero it parked" -> Asterix the hero you play;
  deferred (flying) to the landing, where the launcher's own 4800 (0x4C2B2E) was then kept ("to == choice"). Two
  fixes: SoloDropsSwitch ignores callers outside axxl.exe's image (GetModuleHandle(0) + SizeOfImage: the mod's own
  switches -- yours, the give-back, a lending -- were never meant for it; round 215's "game's own switch ... (pending)"
  log fired on them too). SoloOrderNeedsLead: with the leader in situation 0x0D (on a launcher) the order is for the
  companion, as sent. splaunch24 (new, LOADSECTOR(200, 3): you play Obelix, 0568 dropped, Obelix on the launcher, P1
  X, Asterix put in the water -62.0 -1.5 -551.5): before, "meaning Obelix -- Asterix leads for it first", the 4800
  deferred to the landing; after, "with Obelix on a launcher -- Asterix, as sent", the landing's 4800 ignored, Obelix
  leads to the end. splockcar21 unchanged (boarding / 0020 hand-over and back).
- Round 217 (user, LV2-6 big blocks, the other block pulled last: Asterix's walk into the end detector did not
  switch to him, a strange switch later; xxl_coop.lv2-6_switch_miss.log). The blocks have two end detectors: 01E8
  (group 03D: 4800 then 4803) and 0D29 (group 1A5: 4803 then 48B7, + 1B07). The group's own 4803 ran first and set the
  walked Asterix (order 1) following, so round 213's hand-back ("to handedFrom while the level holds it") found him
  free: 48B7 dropped, levelLeader Asterix; 0CA8 (48CF + 4803) then lent him the lead and the watch took it back
  0.6 s later. EventDispatchHook (single player, the lock on) now notes each hero's held state (AI order not follow)
  as the dispatch begins (g_heldAtDispatch, ProximityHeldAtDispatch); the hand-back takes either. splock26d (new: as
  splock26, ending with 4802 then S_FLAGEV 0D29): before, "48B7 (event 0D28's group: no lock) ignored"; after,
  "hands the lead back to the hero it parked", Asterix leads, Obelix follows.
- Round 218 (user: round 217's LV2-6 blocks fine; the jump down into the fight -- #144, On Cross Rectangle 36.48 7.68
  -390.54 (11.67 x 17.5, flat), node index 1, event 0CA8 = group 195: 1D90 disable itself, 48CF "teleport the sidekick
  to marker 20", 4803; XXL-Editor Snipaste_2026-11-05_00-16-48 -- puts Obelix at the marker when Asterix jumps, nothing
  when Obelix does). #144 is Asterix's; playing Obelix with the level's leader Asterix, SoloZonesTick moves the
  leader's zones to the hero you play, but skipped ZoneMeansItsHero groups (co-op's "a sidekick order set off by the
  other hero means that hero itself") -- 0CA8 stayed Asterix's, never crossed. Solo pass now asks
  SoloZoneMeansItsHero(ev, bound): only with the group's switch away from the bound hero (a circle: "Obelix leads" +
  "the sidekick waits") or a light pillar lit (0x900); else moved. And SoloOrderNeedsLead lets an order through as sent
  from a zone of CoopSoloLevelLeaderAway() (the level's leader's, answered by you) as well as the leader's own (it
  would have lent Asterix the lead for 48CF). splock26f (new: you play Obelix, Asterix up on the blocks 60.1 10.5
  -389.3, Obelix put at 36.48 11.0 -390.54 to drop): before, no 0CA8; after, "zone event 0CA8 (... set off by Obelix)",
  "48CF from Asterix's zone -- Asterix, as sent", Asterix at 35.4 1.6 -391.4. spboss210: 4829 now "from Asterix's zone
  -- as sent" (was the boss run's line; same: Asterix walks), splockcork21 / splockord2 unchanged; splock47's Asterix
  (no input after his glide) slid off to y 220 this run through his own #21 (0488: 4802, the engine's pass) -- Obelix
  waited at the end; the physics, not the change.
- Round 219 (user: round 218's jump works; an unexpected Obelix -> Asterix switch further on, thought to be #284
  (event 08E0 = group 11C: 4803, 1645, lights, 4800, 1D47 / 1D24 off, swing door, camera; rectangles #36 / #71 at 13.5
  6.4 -364.5; XXL-Editor Snipaste_2026-11-05_00-40-11), not reproduced twice; userlogs 20261105-0040-xxl_coop{,.1,.2}.log).
  08E0 fired only in xxl_coop.log, with Asterix leading. The switch (xxl_coop.2.log 136.4 s) was zone 0C50 (group
  18A: 48CE "teleport the sidekick", 4800, 4803, 1D8D itself off), bound to Obelix, set off by the Obelix you play at
  (15.4 5.0 -363.6): "48CE from Obelix's zone -- Asterix, as sent", then round 213 took the group for a hand-over
  (48CE in it) -- Asterix led and was teleported. In the game Obelix crosses it as the AI sidekick: its own catch-up.
  ProximityGroupOrdersBeforeSwitchAway(ev, bound): a sidekick order (4802, 4813..4876, 48BB..48EC) before the group's
  switch, the switch away from `bound` -- a circle switches first then parks (LV2-1 0050: 4801, 4802), not this.
  SoloOwnCatchUp (coop.cpp): from the dispatch, the zone bound to the leader = your hero, such a group -> its sidekick
  orders (SoloOrderNeedsLead, now bool: true drops) and its switch (SoloDropsSwitch) dropped, logged "is that hero's
  own catch-up as the sidekick". ProximityDispatchGroupOf(msg): the dispatched group sending msg. splock26c (new: you
  play Obelix by 15.4 2.5 -363.6, S_FLAGEV 0C50): before, Asterix leads, put at 12.3 2.1 -368.0; after, dropped,
  Obelix leads, nobody moved. splock21, splockcork21, splock26, splock26f, splock47, splockord2, splockcar21 unchanged.
- Round 220 (user: LV2-10 boss, xxl_coop.lv2-10_unwanted_switch.log: "a strange mod's switch" after Asterix jumps off
  the boss's button). The chain, sp_hero_switch_lock on: Asterix on the corkscrew platform by the light pillar (event
  0360 = group 06C: pillar 0901, 4801, 4802, cameras) -- hand-over to Obelix; Obelix's punches lift it, the level hands
  Asterix the lead (Obelix waits); the automatic cable car drops Asterix on the boss's button: the boss's own code
  (CKHkBoss 2,177, vt 627A60) 0x4F3165 4801 + 0x4F317E 4802. The 4801 matched handedFrom (Obelix): "the game hands the
  lead back", handedFrom cleared -- nobody noted as parked. ~15 s later the boss throws Asterix off (state end
  FUN_004F18D0, called from 0x4F2EA3 / 0x4F36C4 / 0x4F3E47 when +0x5B4 reaches the spline +0x600's length: clears +8
  bit 0x20, flags its event +0x5C0, trio 487C then 4800 from 0x4F194F; FUN_004F1850 starts it: 487A, bit 0x20; Asterix
  AI order 12, "under external control"): a game switch to a hero nobody parked -> "(pending)", and the lock gave Obelix
  the lead back once Asterix was free (user's 68.3 s; 3.3 s in the test). Now SoloGameParks (CoopDeferTrioEvent, after
  SoloDropsSwitch): a 4802 from game code within 0x40 bytes after that code's own switch (g_solo.gameSwitchFrom/At,
  noted in SoloDropsSwitch's game-code branch) and within 100 ms is a hand-over (SoloHandOver to the leader: the parked
  hero is handedFrom), logged "the game parks X after its switch". The boss's 4800 is then the hand-back: Asterix
  yours. New autotest step S_CALLHK (class (2,a) object 0, thiscall s.b) -- 0x4F18D0 on the boss runs fine; S_NEAROBJ's
  y (if not 0) is the put height. splockboss210 (new, user's spots: Asterix on the platform 1716.946 -746.66 -1144.917,
  Obelix on the ground 1715.26 -747.82 -1141.75 punching the turnstile 11 times, then the boss left to throw him off):
  before, "the game's own switch 4800 (from 004F194F) kept ... (pending)", Obelix given the lead 3.3 s later; after,
  "the game parks Asterix after its switch (4802 from 004F317E)", then "the level hands the lead to Asterix (the game
  hands the lead back to the hero it parked)", Asterix leads at the end. splockcar21, splock21, splock47 (one extra
  late 0488 lock: its known slide-off variance), splockcork35, spboss210, splock26c, splock26 unchanged.
- Round 221 (user: a whole level 4 run, xxl_coop.v4-3_corkscrew_skip.log / xxl_coop.lv4-4_broke.log; and "can the
  lock be built around the nuisance switches instead of around the corkscrews, pillars...?"). (1) LV4-3: Obelix,
  launched off his corkscrew (01B0 4845, 01A0 4846: AI order 2), lands in 01A8 (group 035: 4801 + 4803 + 488B +
  1B03) -- dropped as "no lock" (nobody parked, Obelix not handedFrom). (2) sp_hero_switch_lock simplified --
  the SP game's switches are trusted unless they decide out of nowhere. SoloDropsSwitch now: the mod's own (outside
  axxl.exe) kept; levelLeader = to; a trio launcher's landing (0x4C2AB0) dropped (unless to the leader / your hero);
  the game's own code (not the dispatch) kept, the new leader yours (SoloHandOver) -- rope car boarding, ride modes,
  the boss, all one branch; from the dispatch: a group that parks a hero (DispatchHoldsLoser -> GroupParks: 4802,
  or a walk 4813..4876 / a put 48BB..48EC with no 4803 in the group -- by content, not message order) kept, yours;
  a switch to the leader / your hero kept; a switch to a hero in the level's hands as the dispatch began
  (g_heldAtDispatch, now LevelHoldsOrder: AI order 1 go to, 2 jump, 3 wait, 8 beam, 9 cable car, 12 external
  control, 15 go to no PF -- not 0 follow, 4/5/6 fights, 10 hit, 11 clear the path (LV2-4's 0288 met Asterix in
  it a moment), 13 hint man, 14 shop; only the hero not leading) kept, yours; the rest dropped. SoloOwnCatchUp is
  now ProximityGroupCatchUp(ev, bound): the group switches away from `bound` and GroupParks is false (order-free;
  was "a sidekick order before the switch"). Gone: handedFrom (hand-backs needed the level's own earlier
  hand-over), the game-code hand-back / rope car / ride mode branches, round 220's SoloGameParks, the "(pending)"
  kept group with other trio orders and nobody held, the dropped switch's release (SoloReleaseWatch: a held target is kept now).
  The lending (SoloOrderNeedsLead) and its give-back stay. Consequence: a switch to a waiting hero the level
  parked is a hand-back now -- LV2-1's keg (0348: 4801 + 4803, Obelix waiting after 0058's walk) gives Obelix
  the lead for good (round 201 dropped it), LV4-4's 0070 (Asterix walked to a marker at 463 s, waiting) and LV5-3's
  0858 / 0228 (Asterix waiting) too. (3) LV4-4's sphere #4 (event 0650, sphere-enter 4, bound to Asterix, flags
  0F/01; off, 0E, once fired) did not fire in the user's run before the car's end (568.2 s) -- with the lock off it
  fires 0.2 s before the end. Not reproduced: splockcar44 (fresh) and splockcar44b (the user's way: you play Obelix
  through sector 4, Asterix's zones answering him, then Asterix at the car) both fire it at rope +0x14 ~164, the
  end ~168 then hands Asterix the lead. Not the lock picture (no restore in the log). Autotest notes: S_ZONEREC
  (the tests of event a: list, index, bound hero, flags); the rope car needs S_CARPOS (the X hold only starts the
  pull, and P1's injected X did not -- the user pressed it); POTION against the fights; S_NEAROBJ's y = height.
  splockcork43 (new: S_FLAGEV 01A0 then 01A8 400 ms later): before, "ignored -- Asterix keeps the lead"; after,
  Obelix leads, the hero you play.
  Regression (cmp of HAND / DROP / TAKE / end leader vs the last runs): splock21, splockcork21, splockord2, splock26,
  26d, 26f, 26c, 26w, splockcar21, spboss210, splockboss210, splaunch24, splockcork35, splock43, splock47, splaunch53,
  splockcar44b the same; splockev2's keg 0348 now hands Obelix the lead (above); spboss36's test Obelix crushed at its
  end (as noted in round 204) -- no lock decision in it.
- Round 222 (user: a whole-game run with round 221, xxl_coop.full_game_run.log; xxl_coop.lv4-8_switch.log,
  xxl_coop.lv6-5_last_boss_activator_safe_{on,off}.log). (1) LV4-8's launcher (-1008.5 273.4 -989.8), you play
  Obelix: mid-flight Asterix's landing zone 0858 (4800 + 4803) was kept as "the level hands it to the hero it had
  in hand" -- Asterix's AI order 3 is the launcher's (its call 4804, the companion's wait on it), not the level's
  -- then deferred to the landing (CoopDeferTrioEvent: a switch mid-flight hung both heroes in the air): Obelix
  -> Asterix. SoloDropsSwitch: a dispatch switch while a hero flies from a catapult (HeroFlying, situation 0x0D)
  and not to the leader / your hero is dropped, as the launcher's own landing switch. splaunch48 (new, -Level 4):
  before, the hand-over + "after the landing"; after, "mid-flight from a launcher ignored -- Obelix keeps the
  lead", 4803 still sent. (2) LV6-5's last lever (749.3 64.9 -257.4, (2,52) #5, +0x2A 0238: 4800 + 4803, the
  boss's door) took no hits in single player with safe_sequence: Asterix leading -> the leader rule ("switches to
  Asterix, who leads already (Obelix's to pull)"), Obelix leading -> round 212's SoloLeverEarly ("gives the lead
  to Asterix, who is not held by the level"). A freeing lever (FreeingLeverTarget: GroupOnlySwitches + 4803) is now
  judged by SoloLeverEarly alone: blocked only for a hero not the one it gives the lead to, that one free
  (following), and -- with the lock -- that one the level's leader (CoopSoloLevelLeaderAway == to: you switched
  away from it); the leader rule no longer applies to it. LV2-5's #419 (48B7 + 4803, for the hearth's 0D60 wait;
  user: only Obelix gets onto that cliff) stays blocked for an early Obelix (the level's leader Asterix); after
  0D60 hands the lead to Obelix it is the game's. Tried and dropped: "an armed zone / any group of the level parks
  that hero later" -- 0D60 is no detection zone (flagged by the hearth), and every level's table has groups that
  park a hero (LV2's circle 0050, LV6's 0008). spact65a / spact65o (new, -Level 6: the spy's talk, X x3, then
  sector 5 -- loaded before the talk's end the arrival camera never ended, user --, teleport): Asterix's punch
  opens the door (was ignored); Obelix (switched) still ignored -- it gives the lead back to Asterix, the level's
  leader (switch back). splever25: Asterix's punch dropped from it (cannot reach the cliff), Obelix ignored as
  before. (3) User question, the "pending" switch: in the full run (8.7 h, all levels) the catapult deferral fired
  once (LV4-8, 5770 s); the shop deferral (co-op only) never; the lock's lending (SoloOrderNeedsLead "leads for
  it first") and its give-back ("the hero you play, free again") never. The deferral stays (it is the mid-flight
  hang fix, co-op and the lock off too); under the lock the LV4-8 switch no longer reaches it.
  Regression (cmp of HAND / DROP / TAKE / SAFE / end leader vs round 221's runs): splock21, splockcork21,
  splockord2, splock26, 26d, 26f, 26c, 26w, splockcar21, spboss210, splockboss210, splaunch24, splockev2,
  splockcork35, splock43, splock47, splockcork43, splaunch53 the same. splockcar44b: one run with both hero
  switches refused ("a hero off its feet", a timing flake) -- sphere #4 fired, as before; the rerun (switches
  taken, the user's path) -- sphere 0650 stayed armed (0F/01) past the rope's end, Obelix kept the lead: the
  user's LV4-4 bug of round 221, intermittent on the same path (round 221's run fired). Open, next round.
- Round 223 (user: the "pending" meant is the lock's own -- the hero in show_leader's "LOCK: ON (X)", the lock's
  `choice` given back to you -- never needed in the whole-game run: remove it; show the level's lead instead).
  coop.cpp: gone -- SoloOrderNeedsLead's lending (CoopGiveLead to levelLeader for a level's later sidekick order),
  SoloFreedByLeadersZone / handedTo, and SoloSwitchLockWatch's give-back (0.3 s free -> the lead back to `choice`),
  its "sequence under way" wait and "free again but far" adoption (freeSince / farSince). Kept: `choice` as the
  lock's "the hero you play" (SoloDropsSwitch's "to your hero: kept", SoloOwnCatchUp, CoopSoloLevelLeaderAway) --
  the mod's own short leads (the machine gun's gunner, second chance's) give the lead back themselves; a leader
  other than `choice` that stays settled (no fallen hero, SoloSwitchRefusal null) for 1 s becomes `choice`
  (otherSince), no switch. SoloCatchUpOrder: only the catch-up group's orders dropped (SoloOwnCatchUp). The
  level's lead (levelLeader): set by SoloDropsSwitch for every switch from axxl.exe (dispatch or game code, kept
  or dropped; not the mod's own -- your switch, R3 + X, the landing deferral's resend) now in single player with
  sp_hero_switch whether locked or not, reset per level, the leader at the level's start. show_leader: "LEADER:
  X  LEVEL'S LEAD: Y" (CoopSoloLevelLead, was CoopSoloLockedHero; glyphs V and ' added). In the full run the
  removed parts never ran (no "leads for it first", no "the hero you play, free again"). LV4-4 (user): the
  sphere and the switch to Asterix at the car's end fired in the user's play -- splockcar44b's one miss (round
  222) taken as the test's (S_CARPOS drives the car), not a bug.
  Regression (cmp HAND / DROP / TAKE / SAFE / end leader vs round 222): all 23 the same (splock21, splockcork21,
  splockord2, splock26, 26d, 26f, 26c, 26w, splockcar21, spboss210, splockboss210, splaunch24, splockev2,
  splockcork35, splock43, splock47, splockcork43, splaunch53, splaunch48, spact65a, spact65o, splever25);
  splockcar44b fired its sphere this time (Asterix leads at the end); no "settled" adoption in any run.
  spact65o's shot: "LEADER: OBELIX  LEVEL'S LEAD: ASTERIX".
- Round 224 (user: a Portal 2 style partner view -- the other hero in a small box at the bottom left, 5 s after a
  refused hero switch (restarted by each refusal, gone at once on a switch that goes through), and with second
  chance while the other hero is down until revived or the game over; the mod's camera on him; the HUD over it).
  [single_player] sp_sidekick_view (on, behind sp_isolation; env XXL_SPVIEW). coop.cpp CoopSidekickView(): the
  other hero while g_viewUntil (SoloHeroSwitch: a refusal +5 s; kSwitchNoView refusals -- no game, paused,
  cutscene, no leader, a hero missing, Obelix not in the level -- start nothing) or IndivFallenHero() is it
  (sp_second_chance; -1 after a game over); never with player 2 in, co-op suspended, a cutscene or the pause.
  gamecam.cpp: Wanted() in single player = CoopSidekickView() >= 0, the copy's hero = that hero (TickHook), its
  input zeros (UseInputOf(copy): nobody's right stick), the game's own fight camera untouched (ApplyFight on the
  live camera in co-op only); the copy starts from the game's camera with a cut onto the hero, as in co-op, and is
  dropped with the view. splitscreen.cpp GraphicTickHook: no split + a view + GameCamView ready -> RenderSplitFrame
  (view, dt, pip): the split frame's three walks (stamp for the box camera, player 1's, the box's) with vp1 the
  whole screen and vp2 30 % x 30 % of the window, 2.2 % of its height off the bottom left; player 1's camera the
  game's own (no P1CamMatrix / split_fov), the box the copy's matrix, window and planes; before the box's walk the
  box and a 2 px frame cleared (D3D Clear target + z, ClearRect flags); then DrawHud (DrawHudHook: single
  player's HUD and second chance's rows) over everything, present; g_splitActive stays false (HUD layout, hints,
  klusters, sound as single player). A switch that goes through mid-view: the leader-switch hand-over swaps the
  game's camera with the copy (the new leader's camera is the copy's), then the copy goes.
  Not done: crates / bonus beacons only for player 1's frustum (klusters.cpp is split-only) -- ones near the
  sidekick and out of player 1's view may be missing from the box.
  Autotests: spview2 (new, XXL_SPISOLATION=0 XXL_SPSWITCH=1): Obelix told to wait, refused at 10.6 s -> view;
  refused again 14.1 -> off at 19.1; refused 20.1, Obelix follows, switch at 23.6 -> off the same frame. spsecond2c
  (XXL_SPSECOND=1 XXL_DOWNAT=1:3000 XXL_REVIVEHOLD=2): view of the fallen Obelix 8.9 s, off at the revive 16.4 (the
  revive countdown box covers the box's lower part, as the user expected). spsecond2g: off at the game over. split1:
  co-op split screen as before.
- Round 225 (user, on round 224: crates missing in the view is fine; the left box -- the revive countdown -- above
  the view while it shows, animated as the flask moves out of a box's way; the potion's flask out of the view's
  way too, not over the combo bar's column at the left; a debug option with the view always on, to watch the AI).
  splitscreen.cpp: SidekickViewRect (shown this frame; frame included, shares of the screen, y up from the bottom
  -- the HUD's 2D places, as HintBoxRect's). hints.cpp: in single player PlaceOf(HINT_LEFT_BOX).up = g_leftLift,
  UpdateSlot moves it towards (view top + 0.01 - the box's unraised bottom) at the flask's speed (interface
  +0x278), snapped while the box comes in or is away. hud2.cpp LayoutFlask: in single player g_flaskAside moves it
  right of the view (view right + 0.01 + its half width) at the same speed, before the hint-box rise check;
  DrawHudHook runs Hud2BeginP1 / EndP1 (the flask's layout) in plain single player while the view shows or the flask
  is still on its way back (Hud2FlaskAside). [debug] sidekick_view_always (env XXL_SPVIEWALWAYS): CoopSidekickView
  gives the other hero whenever playing (also with sp_isolation on). Autotests: spviewhud2 (new; XXL_SPSECOND=1
  XXL_DOWNAT=1:3000 XXL_REVIVEHOLD=2 XXL_REVIVELIMIT=60): flask up, Obelix down -> the countdown box comes in above
  the view (lift 0.329), the flask slides to x 0.35; revived -> both back. sppotion2 with XXL_SPISOLATION=1
  XXL_SPVIEWALWAYS=1: the view from the level's start, on Asterix after Obelix takes the lead.
- Round 226 (user, on round 225: the left box right of the view instead -- where the flask had gone -- and the
  flask rising as it does over the left box; the box in from below the screen when the view is already up, else
  from the left as before). hints.cpp: BoxPlace(p, slot, plate) -- the one place DrawSlot and HintBoxRect take a
  box's x and rise from: sideways slide (now also over g_leftAside), or Slot.fromBelow (set as new content starts:
  the left box in single player with SidekickViewRect up): x in place, risen by -(1 - e) * (its top + 0.01) --
  in from below and out the same way. g_leftAside (was g_leftLift) moves the left box right of the view (its
  left edge at view right + 0.01) at the flask's speed, back when the view goes; snapped while the box is away or
  coming in. hud2.cpp LayoutFlask: the sidekick view is a third rect (q 2) the flask rises over, as a hint box;
  g_flaskAside gone; Hud2FlaskMoved (lift > 0) keeps the layout running in plain single player while it comes
  back down. Autotests: spviewbox2 (new, XXL_SPSWITCH=1): hint in the left box (from the left, flask risen over
  it), refused switch -> box right of the view (0.294), flask over the view; view gone -> box back left; view
  first, then a hint -> in from below. spviewhud2: the revive countdown right of the view, clear of player 1's
  "Hold B to revive" box; the flask over the view.
- Round 227 (user: less clutter -- no sidekick view with the other hero within 10 units, also as you reach the
  fallen one; the flask and the left box back to their places without it). coop.cpp CoopSidekickView: the other
  hero within 10 units of the leader (out again past 11: no flicker) -> -1 while a view is wanted (the refused
  switch's 5 s run on; second chance's view back as you walk off); not with [debug] sidekick_view_always. The HUD
  pieces follow SidekickViewRect as before (box back left at the flask's speed, flask down). Autotests: spviewhud2
  reworked (Obelix down by Asterix: no view, countdown from the left; Asterix walks off: view at 11.1 units, box
  right of it, flask over it; back by Obelix: hidden at 9.9, box slides back; revived). spview2: the view on at
  the refusal (Obelix waiting, far), hidden at 10.0 as Obelix follows again.
- Round 228 (Nov 7, commit 9af90b1; user: options' "Overwrite this data?" box text ran past the box under
  xxl_widescreen2d.asi). CText2d layout 0x42EF80 (thiscall on the layout part = CText2d + 0x20, no args, plain
  ret; prologue 83 EC 3C 53 56 8B F1): break test glyphW * aspect * [0x623C64] < width(+8) * aspect; words +0x2D4
  count, +0x2D8 strings, +0x3C8 x/y (height units), +0x5A8 line index, +0x620 hidden flag (room for 60), +0x6AC
  alignment (1 centre, 2 right, 5 spread), +0x6AD vertical, +0x10 lines shown. Draw 0x42F430 places the left edge
  at pos - width(+8) * {0, 0.5, 1}, times the aspect. CMessageBox2d layout FUN_0042ADB0: raw text width
  (FUN_0042B020, no aspect factor) over 0.8 -> text width 0.8, box plate (+0x2C) width 0.8 * 1.05 -- drawn
  (4/3)/aspect as wide by the asi. Fix in the asi: LayoutHook runs the layout with width * (4/3)/aspect (0 < width
  < 0.95), then shifts shown words by the difference * alignment share. Autotest bootoptwrap (XXL_LEVEL=-1): title
  validate, down x2, confirm, up, confirm -- S_BIND one frame (3 frames confirmed the menu's first item too);
  menu bindings up 16/32/52, down 17/33/53, confirm 20/44/56.
- Round 229 (Nov 7; user: after a refused switch, getting near the companion hid the view, but walking off again
  within the 5 s showed it again). CoopSidekickView: the other hero near (hysteresis 10/11) ends a refused
  switch's 5 s (g_viewUntil = 0, logged "refused switch's 5 s ended"); nothing shows until the next refusal. A
  fallen hero's view still comes back once away. Autotest spviewnear2 (XXL_SPSWITCH=1): Obelix told to wait,
  Asterix off 3 s, refusal -- view; glide by Obelix -- hidden at 9.8, 5 s ended; glide 13 units out inside the old
  window -- no view; refusal -- view again.
- Round 230 (Nov 7, commit e88e976; user: after a manual hero switch the new sidekick could be stuck for good --
  LV2-3 Obelix leading indoors (-46.5 0.1 -538.1), Asterix outside (-57.1 0.7 -534.3) past the doorway's cyan
  cells; LV5-5 Asterix leading on the progression ledge (8.2 10.9 61.9, in no node), Obelix behind (20.5 1.5
  77.0) -- Asterix went to the nearest cell, behind a door). The mod's graph (round 83) lets a hero on foot
  through cyan cells by an open door and over the enemies' walk-overs: right for a player, wrong for the AI (the
  game's cell table: 0 = Romans only; after the LV2-3 switch Obelix never moved). pathfind.cpp PfSidekickReaches
  (Graph.sidekick): value-0 cells always shut, walk-overs only with bit 0 (sidekick) and bit 31 (on), a place in
  no node seeded with the nearest walkable cell (x/z + distance above/below the node box). coop.cpp
  SoloSwitchRefusal(t, press): last check, on the press only (not SoloSwitchLockWatch's per-frame "both free"):
  PfSidekickReaches(leader, other) == PF_NO -> "the leader couldn't walk to the other hero"; no graph -> goes
  through. One query, both heroes already required on their feet (no 3 x 0.5 s like WayToFallen / ApartFrame):
  0.15-0.5 ms. Autotests: spunited23 (refused at the user's spot; both indoors goes through), spunited55
  (-Level 5: REACH ledge -> Obelix's area no, together yes; Obelix placed by an autotest on level 5 -- PLACEAT or
  R3 + A -- hangs at y 7.0, body 0B, "off its feet", so the press stops there), spview2 regression.
- Round 231 (Nov 7, commit 37a7600; user's pad test, userlogs/xxl_coop.lv5_5_fail.log: LV5-5 switch
  refused with Asterix at 6.1 10.7 63.1, let through a step on (~13.9 10.5 63.4), Asterix walked to 6.8 3.0 67.8
  behind the door). Round 230's off-graph seed (nearest walkable cell centre over the whole graph) is not the
  game's. The AI's start (FUN_004a00f0, AI states 1/9): its place in a node (FUN_0045caa0: strict box x/y/z, the
  last node first, then its transitions' nodes, then all); else FUN_0045c890: the node whose box is nearest in 3D
  (FUN_0045aa50 = point clamped to the box), FUN_0045ca30 clamps the place into it -> its cell (FUN_00459710) /
  centre at box min y + 0.01 (FUN_004597a0); FUN_004a0010 tests the cell (table 0x665240, 0x7F = not for this
  mover); not valid -> FUN_0045a080: rings round the cell within that node (up to 100), the first ring with a valid
  cell, its nearest (3D) cell. pathfind.cpp NearestCell does the same; the switch log gives "its AI start cell".
  Autotest spstart55 (-Level 5, S_SIDEREACH / SIDEREACH(t, x, y, z): hero a's AI way to a point, start cells):
  ledge points 6.1 / 9.9 / 13.9 / 8.2 -> start cell 6.93 67.77 (node box top 16.4), no way to Obelix 17.4 1.7
  77.7; Asterix as the sidekick put on the ledge walks to 6.96 67.62 and drops to 7.93 2.74 66.77. spunited23
  unchanged.
  End to end: spunited55 reworked as the user did it -- Asterix put at 17.4 1.7 77.7, player 2 joins, R3 + A
  brings Obelix, player 2 leaves (Obelix on the ground; put by PLACEAT or single player's R3 + A on level 5 he
  hangs at y 7.0, body 0B): switch there and back goes through; Asterix at 6.1 10.7 63.1 and 13.9 10.5 63.4 on
  the ledge -> "from its AI start cell 6.9 16.4 67.8 ... no", refused both times (the second was round 230's let-through).
- Round 232 (Nov 7, commit 3c87227; user: LV2-1, Obelix by the lever (-758.3 508.8 -1037.9) after a hero
  switch ran backwards fast, then ran in place at -777.1 -1037.96 for good; narrowed by the user to a slide
  stuck against the lever's wall when the switch is pressed; logs xxl_coop.strange_obelix_infinite_run_lock{,2}.log,
  xxl_coop.ai_slide_bug.log, userlogs/xxl_coop.lv2-1_slide_switch_aiwatch{1,2,3}.log). The walk (FUN_0048FE70,
  from FUN_0048FDA0 in the player / AI Life states) adds a slope's push: +0x30 direction x +0x44 speed, eased to
  the target +0x3C at the rate +0x40. Steep ground (normal y +0x2E8 < +0x8DC, ground +0x280's +0x2A bit 0 clear):
  first frame +0x388 bit 0x40 + time +0x8F8; after +0x8FC on its feet: speed 0, target +0x8E0 (30), rate +0x8E4
  (25), bit 0x80 (sliding). Walkable ground: bit 0x80 set -> rate +0x8E8, target 0 (the brake); bits 0x40/0x80
  cleared. Leaving the Life state (State1_Ai param 2: +0x388 &= ~0x80, likewise the player's) clears 0x80 and
  keeps the push -- a lead change mid-slide left target 30 forever (the user's log: push (-0.73 -0.56 -0.38)
  target 30 rate 25 speed 30, +0x388 0). An AI hero's new place is clamped into its node box (FUN_004A1A20 ->
  FUN_0045C970: AI states other than 2/3/8/9/0xC/0xF), so it ran at the box's edge (-767.49 = node 22's x min,
  -777.58 = node 15's); a player kept the speed until X (an attack) or a hit. Fix coop.cpp KeepSlideFlag
  (CoopBeforeLifeTick, both heroes, normal mode): push target != 0 without bit 0x80 -> bit put back (the game
  then brakes it on walkable ground, or the slide carries on on the slope). Side notes: the AI's start
  (FUN_004A00F0) puts a hero off the graph on its start cell only when the sweep there (FUN_004A1970, ret 4) hits
  something, else +0x390 |= 0x80 and walks to +0x580 (AI state 0F); entering State1_Ai frees the path slot
  (FUN_004A00F0(1)). Autotest sprun21: Asterix leads by the lever, the AI Obelix beside him given the user's
  state (new step S_SLIDEPUSH) -- before: to -777.28 -1037.96, running in place; after: "slide flag put back",
  3 units, then follows. New test steps S_AIWATCH (a hero's AI fields on change), S_CLEARLINE (FUN_004A1970
  reported clear). spswitch2 unchanged.
- Round 233 (Nov 7, commit 023a0f5; user: LV3-6 boss run, second chance, you play Obelix, log
  xxl_coop.lv3-6_partner_death_underground.log: Obelix down at -307.5 5.5 -167.3 ahead of the rolling machine,
  shoved by it along z to -114.4, then at 66.8 s to -311.9 -3.82 -105.5 under the floor (PF "not known"), the
  revive time ran out). The machine is CKHkBoss (2,177, vt 627A60), object 0 at -218.4 5.5 -174.8; its +0x600
  (CKSpline4L, length 34.7, -419.8 12.2 -110.9 .. -437.6 5.8 -110.8) is only the jump at the run's end (state
  bit +8 0x20, distance +0x5B4 += +0x598->+0xC x dt, point at distance = way vf13(out, d, tangent), length way
  +8); the chase itself rolls 5.9 units/s: x -218.4 -> -317.9 at z -174.9, north to z -110.6, west to the arena
  (the user's editor corners are the line's, a few units off). Speed/flags unchanged in the chase (+8 3C6; 3C2
  after a respawn put it back at its start). individual.cpp, levels 2 and 3 only (LV4-11 / LV5-11 / LV6-5 go to
  and fro): TrackBosses keeps each boss's trail (a spot per unit, 512; a jump > 20 = respawn clears it; a turn
  back starts it afresh); rolling = moved within 1 s. A fallen hero (co-op or second chance) is the game over
  (TimeOut with a reason) when (a) dragged: 2+ units moved with a rolling boss within 12 (user: can't hold the
  grab before its saws), or (b) behind: within 8.5 of the trail 9+ units back along it and 6+ behind the boss's
  back (heading over its last 3 units -- without it the body shoved round the first corner counted, the trail
  cuts it), the standing hero not behind too. WayToFallen brings nobody while a boss rolls (bossdown36 before:
  Asterix put by the body in the boss's way, killed too). KeepBodyOnGround: a fallen body's last place in a PF
  node (every 0.1 s; none set while it is in none, e.g. a death in mid-air); 1.5+ lower and in no node (no
  lifter) -> put back there. Not exercised by the autotests: the drag now ends the user's case before the
  corner. Autotests bossdown36 (XXL_SPISOLATION=0 XXL_SPSECOND=1 XXL_DOWNAT=1:24000: dragged, game over 2 s
  after the boss reaches it), bossbehind36 (XXL_DOWNAT=1:23000, Obelix put at -255 -172 behind it: "23 units
  back", game over), spboss36 (Obelix killed by the boss at -248, dragged -> game over). Diagnostic bossway36
  + step S_BOSSDUMP (each (2,177): flags, +0x5B4, +0x600 way, place; a=1 samples the way). LV2-10 not run.
- Round 234 (Nov 7, commit 15ed42c; user: LV6-5, xxl_coop.lv6-5_death_stuck.log, second chance, sp_hero_switch_lock).
  Obelix pulled the block Asterix rode (Asterix order 9, Obelix sit 03) to 844.0 64.1 -265.7; "its group parks a
  hero": Asterix yours on the ledge, Obelix order 3. Asterix down at 830.3 69.6 -254.1: "can Obelix walk? yes,
  11.8 units" -- no bring; Obelix's jump reached y 67.4, never up. Graph (ledge65, PFDUMP): node 8 (753.0 62.1
  -270.6)..(847.0 84.0 -250.6), one flat grid for the floor (64.1) and the ledge (69.6); the ledge's rows z -251.6
  .. -257.6 are all wall cells, Seeds takes the ring-3 walkable cells on the floor below. Fix: pathfind.cpp
  PfSeedRing (how many cells out a place's seed cells are); WayToFallen: PF_YES -> no way (reachTold
  kReachOffGround, revive time not counted) when the fallen hero's ring >= 2 and it is > 2 units above/below the
  other hero (spsecond2: level 2's start has wall rings round the floor -- Obelix 1.2 lower was brought for
  nothing before the height rule) and not already beside it; CatchUpSpot then SpotBesideFallen (the fallen
  hero's own safe spots, else on it). The grab's revive sends the brought hero back (as in co-op).
  (2) user: single player should restore what the down interrupted -- the companion back on its marker, the
  lead back. The lead back and the send-back already worked; the companion's order did not: a level wait
  (4802) noted as a lock is undone at the down (4803, order 0), and LV2-3's hearth walk (4826, order 1) was
  lost at the takeover (order 0 after the revive). coop.cpp: NoteSoloOrder (CoopNoteTrioEvent) keeps the last
  marker walk 0x4813..0x4876 or 4802 sent while a leader leads (4803 clears); NoteSoloPark at CoopUndoLockFor's
  start (every second-chance down): companion order 1 or 3 and that order its -> kept (place, event, a lock's
  "after" picture and its "before" one; not a cable car's or a light pillar group's); individual.cpp after the
  lead back (Revive sets parkRevived; 2 s without the lead -> dropped; a game over drops it): CoopRestoreSoloPark
  -- the after picture, the companion put back (if within 40 units), the event sent again, the lock's before
  picture kept for a later undo. Autotests ledgerev65 (-Level 6, XXL_SPISOLATION=0 XXL_SPSECOND=1
  XXL_DOWNAT=0:24000 XXL_REVIVEHOLD=2; Obelix parked by S_TRIOEV 4802: before "yes", stuck; after brought, B,
  back at 844.0 64.1 -265.7 with 4802, order 3, Asterix leads), sphearth23 (-Level 2, XXL_DOWNAT=0:12000:
  Obelix outside at the user's -56.731 0.75 -534.386 walking to his marker with 4826; brought in, B, sent back,
  4826 again -> walks to -54.6 0.5 -523.3, order 3), spsecond2 unchanged. Autotest note: an AI Obelix put by
  PLACEAT can hang in the air (as LV5-5's y 7.0; the user's spot is fine) -- cause still unknown.
- Round 235 (Nov 7, commit 56ae603; user: round 234 confirmed on the pad; "restore the park in co-op too ...
  not nolock-dependent": a revive loses a marker park the level can't set off again; light pillars can be stood
  on again). coop.cpp NoteSoloOrder / NoteSoloPark / CoopRestorePark (was CoopRestoreSoloPark) / CoopDropPark:
  no p2Joined gate; in co-op the order is kept only if player 2's hero was held for it (g.held -- Hold() hands a
  player's hero to the game on a scripted order, or nolock leaves it the player's: then never held, nothing
  kept). The replayed event goes through the same trio path, so Hold() re-holds it ("handed to the game (event
  4826 / 4802)"). individual.cpp: parkRevived set in co-op too. Autotests cohearth23 (-Level 2, XXL_NOLOCK=0
  XXL_DOWNAT=0:12000 XXL_REVIVEHOLD=2: Obelix at his marker -54.6 0.6 -523.1, order 3, kept; brought in, P2 B,
  back on the marker, 4826 -> held, order 3; with XXL_NOLOCK=1 nothing kept, sent back, stays player 2's),
  coledgerev65 (-Level 6, XXL_NOLOCK=0 XXL_DOWNAT=0:24000: 4802 kept, brought up the ledge, back, held waiting).
  sphearth23 unchanged.
- Round 236 (Nov 7, commit ea2b0b1; user: LV2-3 (they wrote LV3-2) with nolock off, Obelix walked into the hearth room
  before Asterix lit the hearth: the door shut behind him and the hearth's 4826 walked him to his marker outside --
  stuck inside for good). New [coop] walk_to_marker_teleport (default off, user: nolock on is the default; in the
  user's ini and dist; env XXL_MARKERTP). coop.cpp WatchMarkerWalk (every frame, gameplay live, no cutscene):
  co-op a held hero (g.held), single player the AI companion with sp_isolation off; AI order 1 and its target
  (+0x568) 2+ units off (x/z), the walk 1 s old: PfReachable every 0.5 s, three PF_NO in a row -> SetHeroPosition
  on the target (+0.5 y). Autotest comarker23 (-Level 2, XXL_NOLOCK=0; Obelix at -46.5 0.8 -539.5 in the room):
  XXL_MARKERTP=0 -- order 1 at -46.5 -539.5 for good; =1 -- "no way to its marker (-54.6 0.3 -523.1) -- put on
  it" 2 s after the 4826, order 3 there.
  Follow-up (commit 8cf602b, user): walk_to_marker_teleport defaults on; in co-op it runs only with nolock off
  (WatchMarkerWalk: g.p2Joined ? noLock : !SoloMod() -> nothing). comarker23 with the defaults: XXL_NOLOCK=0 put
  on the marker 2 s after 4826; XXL_NOLOCK=1 "Obelix stays its player's (event 4826 ... not followed)", no teleport.
- Round 237 (Nov 7, commit 8e18e0e; user: second chance <-> co-op resets health; second chance at the merchant
  has no coloured shields and "purchases are shared"; a full combo bar switched away stands still and comes back
  broken -- hits do not fill it until it has visibly run down). (1) individual.cpp IndivFrame: a solo <-> co-op
  change with individual stats wanted in both modes snapshots Current() of both heroes, runs Deactivate/Activate
  as before, then writes the snapshot back (g.kept cleared; a fallen hero gets the revive's half health, as it is
  raised by Deactivate). Log "each hero's own stats kept". (2) hud2.cpp Hud2ShopTint (IndivActive, solo, a
  CKHkAsterixShop (2,172) state != 0): Hud2BeginP1 tints the game's pass with the leader's (shopper's) colours;
  splitscreen.cpp DrawHudHook routes single player's HUD through Hud2BeginP1 for it (it skipped the hook unless
  the solo rows / sidekick view were up). The shop's item pictures are not the shield textures: untouched. The
  purchase itself (FUN_004BFC50 state 3 writes the +0x40 block) already went to the shopper in single player
  (game's place = leader): spshopo2 Obelix 5/3 -> 30/6, Asterix 5/3 kept. (3) The combo bar (FUN_00518190;
  +0x24 bits 12-14: 0 hidden, 1 sliding in, 2 partial, 4 full -- entered only when hits cross gm +0xC8 (30) from
  below, itf +0x2C0 the hits last seen) lives in the interface; single player's other row (s2) ticked only the
  health part, so a full bar's own countdown froze in the copy at the switch and came back with the lead: still
  state 4, it played "Combo" again and set the gauge back to +0xA0 (spdrain2b before: back at 44.5 s -> gauge
  28.1). Now the other row ticks kComboPart / kComboGlowPart too (as player 2's copy), not drawn, with the
  interface's sounds (FUN_005153F0, thiscall(itf, sound, arg), ret 8, detoured) muted for it. spdrain2b after:
  state 4 ends at 45.8 s with the gauge, 10 hits then show state 2. Autotests (XXL_SPISOLATION=0 XXL_SPSECOND=1
  XXL_SPSWITCH=1): spjoin2 (A 5 / O 9 -> co-op 5/9, O 7 in co-op -> single player 5/7, twice), spshopo2 /
  spshopa2 (shop menu by S_BIND 20 confirm / 19 right / 21 cancel -- the P1 pad injection does not reach menus),
  spdrain2 / spdrain2b (bar filled by HITS 3 x 11: S_GIVE's 40 hits in one frame skip the bar's full state).
  Regressions spsecond2, combofull2, shopbuyo unchanged.

- Round 238 (Nov 7, commit 7810796; user: in single player with the hero switch, switching to Obelix holds
  Asterix's torch at will -- it should be held only while the sidekick Asterix is parked; co-op stays per lead).
  CKHkTorch::vf15 (0x4AF790), carried (+0x3E & 3 == 1) and lit (bits 2-4): +0x1C counts down only while trio
  vf40 (0x47C5F0: +0x2C == 0, Asterix leads) -- held, not slowed; the HUD flames (vf15/vf16 loop over the
  interface's 4 icons) and the flame anim follow the state. No other leader read in state 1 (FUN_004B10B0 /
  FUN_004B1060 only write the torch's own nodes). SoloTick (proximity.cpp, single player with SoloLeadFree):
  Obelix leading, Asterix's AI order a follow / fight one (IsScriptedOrder false) and not down -> the torch ticks
  with Asterix posing as the leader (as the spitter's potion); otherwise the game's own tick (held). Logged on
  change. Autotest sptorch2 (XXL_SPISOLATION=0 XXL_SPSWITCH=1 XXL_LIFTLOG=1, LV2-1 start): Asterix leads 4.90 ->
  2.44; switched to Obelix, following, 2.33 -> 0.36 then flame 4 -> 3; 4802 (order wait) held at 3.43 for 4 s;
  4803 burns 3.37 -> 0.86; Asterix again burns. HUD lit with Obelix leading (obelix_leads.png).
- Round 239 (Nov 7, commit 36d5d36; user: LV5-10's keg at 587.727 -441.649 -176.906 -- Asterix throws his torch
  away when it blows (xxl_coop.torch_throw_asterix_leader.log), but switched to Obelix before it blew he kept it
  until switched back (xxl_coop.torch_no_throw_obleix_leader.log)). Trio 0x4811 (FUN_00482610's 48xx case 0x11) only
  sets Asterix +0x3EC |= 0x400000. Read by his player situation: Hero_ChooseSituationState via FUN_0049A460 (from
  CKHkAsterixLife::State0_Player 0x4AAAE0) -> 0x49B420: at sub-state (+0x3EC & 0x7E000000) == 0x1A000000 and anim
  frame +0xE8 == 0x28, 0x5127 to the torch, +0x464 = 0, then +0x3EC & ~0x400000 & 0x9DFFFFFF | 0x1C000000 -- and by
  CKHkAsterixLife::InitOnce (vf13, 0x4AA850: lead back to him) with 0x513C. The AI states never get there.
  CKHkTorch::sendEvent: 0x5127 and 0x513C both clear the 4 flame icons (CKGrpMecaCpntAsterix, yp+0x8C -> +0x18 ->
  +0x24, +0x1C..+0x28: +0x10 bit 0x80 via FUN_0041AB10(7,0), +0x48+4 & ~8, unlinked from their branch +0x0C),
  move the torch's node to the world and set state 2 (flying); 0x513C also hides the node (+0x10 bit 1).
  FUN_0043B730 / FUN_0043C150 (ECX yp+0x18) are the sound player, torch +0x2C its sounds. Fix (SoloTick): Obelix
  leading, Asterix's +0x464 == this carried torch with the bit -> 0x5127, +0x464 = 0, bit cleared, the torch ticked
  that frame with Asterix posing as the leader (else vf15 state 2 found its node unseen -- +0x14 != the frame stamp
  DAT_006464C4 -- and went to 3 at once: no flight). Autotests (XXL_SPISOLATION=0 XXL_SPSWITCH=1 XXL_LIFTLOG=1,
  level 2): sptorchthrow2 (Asterix leads: thrown 0.2 s after 4811 at 0x49B462, unchanged), sptorchthrowo2 (Obelix
  leads: thrown at the 4811, flies 0x12 -> 0x13), sptorch2 unchanged. OPEN: with Obelix leading the burning-torch
  picture at the bottom right stays (live -- 876/1140 sampled pixels change in 0.5 s) until Asterix leads; a
  burn-out with Obelix leading does take it down (sptorchout2). Ruled out (same in both cases, S_TORCH 10/14): the
  4 flame icons, the torch's branch +0x34 (CSGBranch), CKGrpMeca's action box (+0x90 null; +0x9C/+0xA0 empty),
  CKGrpMecaCpntAsterix +0x64, Asterix's nodes +0x36C/+0xC28..+0xC48 (FUN_004A3EF0), the torch's events +0x38/+0x3A/
  +0x3C (FFF8), the mod's SP icons / hints / sidekick view (XXL_SPICONS/SPCOMBOHINTS/SPVIEW=0). The crash.cpp
  write-watch slots alias across re-arming (S_TORCH 4 then 8): later hits name earlier addresses.
- Round 240 (user, userlogs xxl_coop.lv5_3_marker64_in_view.log, Nov 4: LV5-3's fight, Obelix played at -243
  9.6 318 in single player with sp_isolation off -- Romans came in on top of him at "marker 64"; seen in co-op too;
  crates "load in view" the same way). Diagnostic first: FUN_0052EC30 (thiscall(squad, Roman, position, facing,
  flag), ret 0x10; prologue `mov eax, fs:[0]`) places every Roman a squad brings in -- from vf33 (the squad's
  starting Romans, 0x52EF70), vf37 (reinforcements, 0x52F930), vf38 (spawn markers, 0x52FB80), vf55 (no-lock,
  0x52DA90); proximity.cpp PlaceRomanHook logs one placed in a player's view (KlusterViewsSeeing 60 units +
  SeesMarker) with the path, bounded 60. The fight's squad (box x -272.5..-187.3) has markers 0 (-245.8 10.1 358.7),
  1 (-219.3 9.7 321.0), 2 (-242.6 9.8 315.0) -- by the user's spot, the likely "marker 64" -- and 3 (-252.1 10.0
  319.1). (1) Single player: MarkerSpawnHook / SpawnPointHook only ran with player 2 in, and the game's vf38 takes a
  marker in its camera's cone facing it first -- marker 2 right by Obelix. Now SpawnRuleOn(): co-op, or single player
  with sp_isolation off (isolation on stays the game's own). (2) Co-op: markers 2 and 3 "in a view 1, seen 0" -- the
  wall sweep's walls without a top reach up for ever, and a camera over the lava (outside the arena's walls) had one
  between itself and every spot of the arena floor. WallsHideFrom(eye): the hero nearest the eye (the one it follows)
  behind a wall from it = the camera is outside the walls, they hide nothing from it (kept per eye, 4; log "outside
  the walls", bounded). Marker 3 now seen; marker 2 still hidden from one eye by a wall WITH a top at (-244.0 11.4
  322.6), the line 1.9 above its foot -- a real occluder (the arena's crate stacks). XXL_LOGMARKERS now also logs the
  wall each eye hits (WallBetween's new `hit`: point, res[12] height above the foot, res+0x34 bit 0 = has a top).
  (3) Crates: the crate log (CrateUpdateHook) now works in single player (the plain query's answer kept). In this
  arena the game's own single player (sp_isolation on) has the same shortage as off: handler 3 has 26 crate objects
  and up to 6-7 crates in view had none (a crate keeps its object anywhere in the frustum, however far). Round 158's
  nearest-first cut (CutCrates, out of QueryHook) now also runs in single player with sp_isolation off; it only
  orders who waits -- more crates in view than objects still leaves some waiting (placeholder geometry). Autotests
  (-Level 5, XXL_MARKERTP=0 -- with it on, round 236's teleport put the AI Asterix on a stale marker 225 units off
  (-16.8 11.4 305.4) right after the PLACEAT, and the level's camera broke): fight53 (single player, XXL_SPISOLATION=0:
  Asterix twists the fight every 6.5 s; "1 of 4 spawn markers in a player's view, left out", no Roman placed in view),
  fight53c (co-op, XXL_LOGMARKERS=1: none placed in view), marker56 regression (LV5-6 marker 28 still behind its
  rock, 7 Romans throughout). Open: vf33's starting Romans are placed with no view check (none seen in view in the
  tests); the user's co-op case and crate spot not known -- the new log lines name them next time.

- Round 241 (Nov 7, commit 741b112; user: the torch HUD should go when Asterix throws his torch away (round 239's
  open item), and with Obelix leading by hand a lit torch's HUD fire still looks slowed although the timer runs).
  The bottom-right box is the in-game interface's (CKHkInterfaceInGame, yp +0x8C -> ... the interface whose vf18
  calls FUN_00520230: thiscall(itf, time), ret 4, prologue 83 EC 2C 53 55). State in itf +0x24 bits 19-20 (0 off,
  1 sliding in, 2 shown, 3 sliding out), Asterix's held object from trio vf46 (0x47C6A0, Asterix +0x464; vf37 is
  the leader's), box node +0x118, the 4 fire-level flames +0x11C..+0x128 (particle pieces), +0x14C its slide,
  +0x414 the level shown, +0x418/+0x41C/+0x420 slide target/speed/place. In state 2 it calls trio vf40 ("Asterix
  leads", +0xA0) twice: (1) Asterix holds nothing -> slide out only if Asterix leads; (2) a level shown -> leader
  Asterix: the flame piece's +0x10 bit 0 set (its own clock); else bit 0 cleared and +0x20 = dt * 0.05 (stepped:
  the sidekick's slowed flame). The torch's own +0x1C..+0x28 "icons" (CKGrpMecaCpntAsterix) are the fire on the
  torch tip in the world (node at Asterix's hand) -- not the box. Fix: coop.cpp TorchHudHook (detour) -- single
  player with the lead free (SoloLeadFree), Obelix leading, box shown: Asterix poses as the leader for the call
  when he holds no torch or is not parked (ProximitySoloAsterixParked, proximity.cpp, shared with SoloTick's
  timer). Parked keeps the game's slowed flame (as the held timer). Co-op unchanged. Autotests (XXL_SPISOLATION=0
  XXL_SPSWITCH=1): sptorchhud2 (burst shots of the box, mean change per pixel between shots: Asterix leading
  30.6 / 66.3, Obelix leading with Asterix following 34 / 61.5 -- was 1.8 / 8.7 --, Asterix told to wait 0.9 /
  4.9), sptorchthrowo2 (box gone after the throw with Obelix leading), sptorchthrow2 / sptorch2 / sptorchout2
  unchanged (box gone after a burn-out with Obelix leading as before). Shots are 960x540 or 770x422 by run (crop
  by share).
- Round 242 (Nov 8, commit d4bb01a; user: with Obelix leading the used-up torch (LV5-10's keg) is thrown away, but
  without Asterix's throw on his model). Asterix's throw is the player situation 0x49B420: enter (param 1) sets the
  action field +0x3EC bits 25-30 = 13; tick (param 0): field 13 and +0xE8 == 0x28 -> 0x5127, +0x464 = 0, field 14
  (later 0). +0xE8 is the hero's current ANIMATION id, not a frame (the AI sat at 101): 39 the wind-up, 40 (0x28)
  the throw. Setting the field on the AI did nothing (the animation is chosen from the player situation +0x3E9).
  Fix: coop.cpp SoloTorchThrow (CoopFrame, single player with the lead free, Obelix leading): Asterix holding a lit,
  used-up torch in a playable mode, not busy / down / in another action -> +0x1A = 1 (the pad's commands ignored),
  bit 0x400000 cleared for the frame (his life's vf13 0x4AA850 on becoming a player's drops a used-up torch at once
  with 0x513C), +0x18 player variant; next frame the bit back -> his situation 06 picks the throw. Over when the
  torch is gone and the field is 0 (or 1.5 s after it left; 2.5 s without a throw): +0x18 AI variant, +0x1A back.
  CoopFrame's "a player hero without player 2" reset skips Asterix meanwhile. SoloTick: the immediate throw waits
  0.3 s (SoloTorchThrowSoon) and not while throwing (CoopSoloTorchThrowing); a flying torch (state 2) is ticked as
  Asterix's then. Autotest sptorchthrowo2: field 13 / animation 39 -> 40 (torch gone) -> field 14 -> 0, the AI
  again 0.7 s later (f18 03, order follow, +0x1A 0); shots throw_0..4 match sptorchthrow2's (Asterix leading).
  sptorch2 / sptorchhud2 unchanged.- Round 243 (user: the who-does-what hints of the machine gun, boat and belly slide on every use -> only the first
  one's). shared.cpp ShowModeHint / ModeHintHere: per kind (gun -- all three gun modes --, boat, slide) the first
  co-op use since the game started is remembered by its spot (the riding hero's +kHero_pos at the mode change:
  Asterix on gun / boat, Obelix alone on the gun or on the slide) and the level load it was in. The hint shows when
  the mode starts within 25 units (10 up/down) of that spot in that same load -- each time --, never elsewhere, never
  after any load. NoteLevelLoad counts loads: level number change, gm +0x08 (next level) >= 0 seen (-1 in play; the
  same level again sets it), a new trio or gm +0x20 level object. Twister Fusion not touched. Logs "the X's hint
  stays with this one (...)" once, then capped 20 lines "X at ..., load N: the hint's own / not the first one".
  Autotests (S_RELOAD a > 0 now loads level a): hintgun3 (-Level 3: LV3-3 gun twice shown, LV3-6 gun -172.1 5.7
  -196.7 none, level 3 again + LV3-3 none; this gun is taken by Obelix alone, P2 A leaves; a same-level load keeps
  player 2 in), hintboat (-Level 4: LV4-9 shown, level 5 + LV5-4 boat 352.8 12.7 22.6 none), hintslide2 (LV2-2
  shown, LV2-9 none), hintslide29 (LV2-9 first: shown). LV2-9's slide start (cross line z -219.48 at x 214.6) is
  armed by walking in from 218.3 50.0 -179.4 (user); its start spot 214.6 50.5 -217.1/-217.2 in two runs. A slide
  runs 60+ s with nobody steering; trio 0x4806 by hand ends the mode but leaves Asterix at 0 0 0 -- no second ride
  of the same slide in one run (left to the pad test). Placing the leader with one PLACEAT right after level 2's
  start / into an unloaded spot does not stick (a short GLIDEAT does). gunlist3 lists level 3's guns (2,31).
- Round 244 (Nov 8, commit b7401ad; user: (1) co-op's start takes the leader the player switched to in single
  player with sp_hero_switch, not the level's lead (LVL LEAD); (2) LVL LEAD skips LV2-1's switch at -793.3 526.2
  -989.6). (1) coop.cpp TryJoin: once CoopAllowedNow passes and before p2Joined, CoopSoloLevelLead() (the level's
  lead, -2 with sp_hero_switch off or in co-op) differing from the leader -> CoopGiveLead to it ("co-op starts: the
  level's lead ..."), unless that hero is not following (AI order != 0), +0x1A set or down (then logged, kept).
  p2_hero = other is decided after it. (2) The zone is 01E0 (group 03C: 4800 "Asterix leads", 4803, ...), Obelix's:
  with Obelix played and leading, SoloDropsSwitch's lock check SoloOwnCatchUp (that hero's own catch-up as the
  sidekick) returned before levelLeader was set (user's xxl_coop.1.log 75.8 s). The level's lead is now set first
  for every switch from the exe, the lock's catch-up drop after it (lock off never reached it: the same tracking).
  Logged (cap 60) "the level's lead: X (its switch, from, who leads)". LV2-1's keg is event 0348 (4801, with the
  lock dropped as "nobody in the level's hands"). Autotests lvllead21 (S_FLAGEV 0348 -> lead Obelix; switch button
  to Obelix; Obelix glided into 01E0 -> lead Asterix, HUD "LEADER: OBELIX LVL LEAD: ASTERIX"; P2 Start -> Asterix
  takes the lead, P2 joins as Obelix) and lvllead21j (0348, P2 Start with Asterix leading -> Obelix leads).

- Round 245 (Nov 8, commit d1a0e55; user: cutscene skip by holding A 3 s with a "Hold [A] to skip" prompt and a
  loading bar in the box itself, Figma "progress bar" 4966:830; the revive's bar the same). (1) CKSrvCinematic::vf17
  (0x46AC10..0x46AD3F) reads InputView_GetAction(0) on its view (yp +0x80 +0x18): value 44, a combined action whose
  value is a press's edge (255 one frame) and whose binding has no hold time (+0x18 stays 0 with Enter held;
  CKInput_UpdateBinding 0x435A50: +6 the device's raw byte, +0x14 press time, +0x18 held time, 0 when raw is 0).
  shared.cpp's GetAction hook hands that read to cutskip.cpp (CutSkipRead): 0 until held 3 s, then 1 once (the game's
  own skip path, coop.cpp's run-on still fires). Held = Enter (GetAsyncKeyState, game focused) or any XInput pad's A
  (player 2's pad: player 2's box); a button held into the scene counts only after a release. Skippable = a playing
  scene with 0x1000, (f & 0x180) != 0x80, +0x30 >= 0 (vf17's test). Level 0 untouched (no in-game HUD). Prompt
  HINT_SKIP: pad_a_unpressed / pad_a, IconKeyGlyph(VK_RETURN) unpressed / pressed; 5 s after the last press;
  the full bar 0.4 s after the skip. The in-game interface ticks during level 1's intro (the box shows over it;
  Draw2dHook's fallback for a non-ticking interface not needed there). (2) hints.cpp HintProgress(key, player, p):
  Choose/UpdateSlot carry it, DrawBar draws the plate (interface +0x140, CBillboard2d, own billboard at +0x20: +8 Rt2d
  brush, +0xC fill RGBA, +0x10 border RGBA drawn when +0x1D = 1 -- the 5-vertex outline) a second time from the box's
  left edge, p of its width, brush set by Rt2dBrushSetRGBA 0x5F9280 (cdecl(brush, 4 corner RGBA*); 0x20 floats saved
  and put back), border off. Colour: white a=.33(1-e) under #00FF0E a=.33e, e = cubic-bezier(.5,0,.5,1)(p); fade-out
  0.25 s once full. The game plate's fill is 00 00 00 99. Revive: "Reviving X" + HintProgress instead of the =/- text
  bar; after Revive the full bar shown 0.4 s. Autotests holdskip (-Level 1: tap / Enter 1.6 s / pad A 4 s on virtual
  XInput slot 0 -> skipped at 3.0 s; the autotest's P1() writes the game's values only, not seen as held) and
  revivebar (-Level 2, XXL_SPSECOND=1 XXL_DOWNAT=0:3000 XXL_REVIVEHOLD=2).
- Round 246 (Nov 8, commit 69e0276; user: with force_level -1 the game's intro has no hold prompt; the film's own
  skip ~15 s later must stay a press). Level 0's scenes (S_WATCHCINE): 0 the intro D801 -> D901 (0x1000 from
  skip_cutscenes), 1 the film 5900 (the game's own 0x1000), 2 kind 0x80, 3 the title 5000 -> 5100 (skipped with
  "Press Validate"), 4 5200 -> 5300. MakeCutscenesSkippable keeps the scenes it gave 0x1000 (CoopSkipAdded); cutskip's
  hold scenes: any skippable one in levels > 0, in level 0 only scenes[0] when skip_cutscenes gave it the flag; a read
  with no hold scene playing gets the game's value (film and title on a press, the intro's chain to the film --
  g_level0SkipAt -- unchanged). Level 0 has no CKHkInterfaceInGame (2/132); present: CKHkInterfaceOption 2/133,
  CKHkInterfaceMain 2/136 (vt 0x6286B4: +0x30/+0x34/+0x38 plates -- +0x38 the bottom bar 1.0 x 0.09 at y 0.075 --,
  +0x4C/+0x50/+0x54/+0x58 texts, +0x50 bottom right at 0.965 0.080), LoadSave 2/138; the HUD's 2D layer (0x4140F4)
  is drawn there. hints.cpp: UpdateSlot(p, itf, dt, tpl) / DrawSlot(p, text, plate, arg) take the templates; Choose
  with no interface shows hints only; Draw2dHook's level 0 path (HINT_SKIP shown, Level0Templates) lays the boxes
  out afresh from +0x50 (its font a little larger) and draws on +0x38. Autotests (XXL_LEVEL=-1): boothold (pad 1
  A tap: box; held from 2.5 s on XInput slot 0: intro skipped at 3.0 s, film chained, title; P1 A there validates
  it, no box), bootfilm (P1 A tap in the intro: no skip -- P1() reaches only the game's values, so no box either;
  tap 17 s in: the film skipped at once, title). holdskip (level 1) unchanged.
- Round 247 (Nov 8, commit 984daf1; user: rounds 245-246 confirmed on the pad; the skip's hold 2 s instead of 3).
  cutskip.cpp kHoldMs 3000 -> 2000 (the bar fills over 2 s). Autotests holdskip (-Level 1: skipped at 2.0 s; Enter
  held 1.6 s still not) and boothold (XXL_LEVEL=-1: the intro at 2.0 s, the film chained, title on a press).
- Round 248 (Nov 10, commit 1cbbe26; user + their designer, Figma 4966-780). (1) Level 0's intro on a press again:
  cutskip.cpp kHoldLevel0Intro (false) -- a source switch, no ini. (2) Revive countdown bar: hints.cpp Bar {share,
  style BAR_HOLD / BAR_TIMER, secs}; HintTimerBar(key, player, share, secondsLeft): red FF0000 at 33%, from 10 s left
  33% + 33% (0.5 - 0.5 cos(2 pi t / 1 s)); no fade when full. individual.cpp TimeLimit: limitAll (the limit as set:
  SetLimit / the fall) -> share = 1 - left / all every frame (user: fills left to right like the hold bars; first
  version shrank). (3) Text centred on the plate: in DrawSlot the text's y = plate middle + 1 - wy0 - kInkMid * size
  + step * (lines - 1) / 2, kInkMid = 1.23 (a line's ink middle above its place, text y - 1 + word y; measured --
  0.5 put the text above the plate). Plate geometry unchanged (single line 0.09 high; x - kMargin .. x + w + kMargin).
  (4) Heroes: HintHeroName = the icon alone; LayOutSlot swaps each hero glyph for a blank (icons.cpp
  hero_slot_asterix / _obelix: the icon's ink width -- asterix 49, obelix 51 of 64 px, columns 7-55 / 7-57 -- times
  1.5) and records its x / width / line; DrawSlot draws the hero glyph over it at 1.5x the font size (g_glyphObj 8 /
  9, strings 10 / 11; kNumStrings 12), raised 0.06 of the size (kHeroDy). (5) Autotest FitWindowOnScreen: the
  1920x1080 window's client reached 31 px past a 1080 screen; PrintWindow left those rows white (the shots' last
  15 rows). Autotests: hintalign (-Level 2: S_HINTTEST cases 4-8 added; XXL_HINTLOG=1 logs each box's plate rect,
  tools/hint_pad.py prints the ink paddings per shot -- single line 20-22 px left/right, 13-14 top/bottom (7/7 with
  a hero icon); pad icons' dark rims read a few px short), revivetimer (level 2, XXL_SPSECOND=1 XXL_DOWNAT=0:3000
  XXL_REVIVEHOLD=2: 31 s limit, the bar from the left, pulsing from 10 s, revived at 30 s); holdskip 2.0 s, bootfilm
  (intro + film skipped at the first tap) unchanged otherwise.
- Round 249 (Nov 10, commit f80929a; user: round 248 confirmed on the pad; the game's own prompt box -- single player's
  "Push / Pull X" -- sits high with a wide left padding, as the mod's did). hints.cpp: Choose takes the prompt
  (CoopPromptKindFor: single player = player 1's box, the game's kind) for any mode with an in-game interface;
  OwnBoxes() = no co-op shopper (single player always), so HideGamePrompt / Draw2dListsHook leave the game's prompt
  text and plate out and player 1's box (at the game's place, right) draws it -- centred (round 248). The text is
  IconsRewrite'd only in co-op or with sp_coop_icons (as LocStringHook does). HintBoxRect counts in single player too
  (the flask, hud2.cpp, is at the left: unaffected). Autotest promptsp (-Level 2: Asterix at sector 1's merchant,
  "Open X": plate 126x48 at 960x540, ink 20/20 left/right, 14/13 top/bottom; no second box); hintalign unchanged.
- Round 250 (Nov 10, commit e119dbf; user saw spobtele53 work): (1) single player (sp_isolation off), Obelix leading
  by a machine gun had no "Use": FUN_0049A1A0 writes kind 9 only from Asterix's call (hero vtable +0x3C == 0, trio
  +0x48 set), and the AI companion's never runs. PromptHook writes 9 for leading Obelix where his call left none,
  while the gun is offered (TrioGun, trio +0x3F4 bits 10-11 == 2, +0x4C 0, gun +0x10C bit 0 clear) and his solo
  gun is not on. Co-op unchanged (round 162). (2) Obelix rode LV5-3's automatic cable car (CKHkTelepher 2,158,
  -336.198 16.820 373.618) -- in SP after a hero switch, in co-op whenever proximity ticked it with him posing as
  leader. CKHkTelepher::vf15 (0x5485F0, thiscall dt, ret 4; vtable 0x6293D4 slot 15; rope car CKHkTowedTelepher has
  its own 0x4BC970): +0x1C rider = trio vf25 (leader hero), +0x20 = its ground (trio vf29 = leader +0x280 unless
  +0x384 & 7; later hero event 0xB00); +0x20 == +0x84 (own ground) -> vf27 go (+0x7C 0x80: 0x0C0D boarding), else
  rider cleared + vf28 stop; +0x7C 0x20 = going regardless; 0x40 = skip the mismatch once. AutoCarTickHook: a tick
  with Obelix as leader runs with Asterix posing (trio +0x2C/+0x2D, restored if unchanged), Obelix never kept as
  +0x1C (nor, in SP with Obelix leading, the AI Asterix outside situation 2); SP with the AI Asterix on it too and
  no +0x7C 0x20: tick skipped. Autotests spobtele53 (-Level 5, XXL_SPISOLATION=0 XXL_SPSWITCH=1: Obelix on it
  5.4-11 s, no ride; Asterix after the switch back rides at ~4 s), cotele53 (-Level 5: P2 Obelix on it, Asterix
  leading then Obelix leading -- stays; Asterix boarding with Obelix leading rides, 0C09 jump off at the end),
  spgunuse33 (-Level 3, XXL_SPISOLATION=0 XXL_SPSECOND=0: "Use" by the gun, gone 10 units off, back, X takes it).
  Ground by the LV5-3 car (user): -339.570 18.010 378.275 (-337.172 16.939 374.286 is by the lava cliff: PLACEAT
  there fell in). PLACEAT/GLIDEAT can't take a coordinate macro (one argument to the preprocessor).
- Round 251 (Nov 10, commit b41e029; user: LV2-2 -184.287 5.491 -540.223, CKHkDonutTurtle): combo hints came up by
  turtles though no combo works on them. NearestRoman2 (combohints.cpp, squads 4,26) skips pools whose component
  (pool record +4) vtable is CKMobileTowerCpnt 0x627D20 or a turtle's (Triangular 0x627D50, Square 0x627DB8, Donut
  0x627DEC, Pyramidal 0x627E20) -- the same list as experimental.cpp KeptAsIs, minus the jetpack Romans. LV2-2 has
  three donut-turtle squads (pool 0 each). Autotest comboturtle22 (-Level 2: P2 Obelix inside the donut, every
  combo learned, bar full): before, "Try your new combo: Club"; after, no hint (log: "no combos on its enemies");
  combohint2 still gives the Club / Power Hammer by basic Romans. Putting Asterix there too gets him knocked down.
- Round 252 (Nov 10, commit ff6f390; user: round 250 confirmed on the pad): single player (sp_isolation off), Obelix
  leading on the boat's deck had no "Use" and his X did nothing -- the boat offers itself to him (trio 0x4901: +0x48
  = CKHkBoat 0x627EB8, +0x3F4 bits 10-11 = 1), but FUN_0049A080 takes a machine and FUN_0049A1A0 writes kind 9 only for
  Asterix (vtable +0x3C == 0). PromptHook writes 9 for leading Obelix while the boat is offered (SoloBoatOffered:
  kind 1, +0x4C 0, +0x3F4 bit 2 clear), as for the gun (round 250). His X (SoloNoteMachineUse -> SoloBoatTake): the
  AI Asterix (not down, f1A 0, not held, mode playable -- swimming is mode 4 --, within 40 units) put at Obelix's
  spot, CoopGiveLead(Asterix) + SoloHandOver (the hero you play: the boat's switch kept, as the belly slide's 4801
  to Obelix is -- user); next frame (Asterix leading, player variant) SoloBoatFrame sets +0x48 / kind 1 | 8 and
  sends 0x4807 (machineSending): both heroes mode 1, Obelix in the water pushing, Asterix steers. Autotest spboat49
  (-Level 4, XXL_SPISOLATION=0 XXL_SPSWITCH=1): switch at the level start (both standing -- after LOADSECTOR 9 they
  swim, mode != 0, the switch is refused; on the deck the zone walks the AI Obelix, order 1, refused too), load
  sector 9, Asterix in the water -162 6 -650, Obelix on the deck from -165.2 7.5 -651.7: "Use" shown, X -> Asterix
  leads, 0x4807, the boat sails. spgunuse33 unchanged.

- Round 253 (Nov 11, commits 7dfe01e + 956cdb3; user: QoL windowed_mode = off): (1) xxl_widescreen2d.asi had no
  effect on level 0 in fullscreen (fine after a level load). The 2D manager's aspect (yp+0x34 -> +4) is summed by
  FUN_0042A2A0 (frame buffer raster yp+8 -> +4 -> +8 -> +4, +0xC / +0x10) only while its flag +0x40 is set: its
  deserialize (level load), FUN_00429FD0, FUN_0042A260 (from FUN_00414A90, the WndProc's resize check). Fullscreen
  boots at 800x600 and goes to the saved 1920x1080 without either -> 4:3 aspect kept (asi log: "frame buffer
  1920x1080, 2D manager's 1333"). The asi's watch thread (KeepAspectFresh) sets +0x40 whenever aspect != w / h;
  logs each change (24 max). Also covers a resolution picked in the options. (2) force_level started its level
  before CKAsterixGameManager::vf10(6) (vtable 0x627F6C slot 10 = 0x5037A0, thiscall(state), ret 4), which applies
  the saved options: resolution gm+0x170/174/178 (FUN_005038D0), anti-aliasing +0x17C (FUN_005039A0), slot 0x84
  (+0x180), pad preset (FUN_00503AA0 -> 3 at 0x503AED) -- the root of the old wrong-preset sessions and the
  xxl_coop_padlayout.txt workaround. Levels ran at 800x600 without AA. main.cpp HookGmState wraps the slot;
  ForceLevel waits for state 6 (10 s at most; seen ~2 s after level 0 is up). (3) shot.cpp: in exclusive
  fullscreen (swap chain !Windowed) shots come from the back buffer (StretchRect to a plain RT, GetRenderTargetData)
  -- PrintWindow gave black. Autotests boot1 (fullscreen, aspect 1777 on the title) and idle -Level 2 (1920x1080, AA,
  preset 3 from the game itself).
- Round 254 (Nov 11, commit b7e0428; user: squad_romans_at_once 3 should work, level limit): a squad's Romans at once
  are capped by its pool's hooks -- CKGrpPoolSquad's member list (+0x20 via vf12, chained +4) is the free list,
  FUN_00530380 moves one to the squad (FUN_00408b50(hook, to, from, life), FUN_00408e60 life active) and
  FUN_00530460 back. Per level (class (2,93) / pools): LV1 18; LV2 8+12; LV3 6/4/9/10/6; LV4 18 (lock fight 6 at
  once + 6 hiding Romans) + 4; LV5 10/7/7; LV6 5/8/14; leaders 1-6 per pool. Lending between pools can't reach x3
  (LV4 has 22 basic Romans in all), so Romans are cloned in the level files: romanforge/ = XXL-Editor core
  (KEnvironment, ClassRegister, Duplicator, GamePatcher; MIT) built as romanforge.lib with shims for json/fmt/
  squish/zlib/GUI, linked into the asi (types Vector3/CNode/CKService renamed by /D to stay apart from ours;
  asserts throw). Original PC levels: class header + "DRM" member values live in axxl.exe (Ipatix build: DRM sig
  at file 0x2E933C, 8 level headers from 0x2E97AD); GamePatcher converts a level to the "Asterix " format and the
  module needs 5 patches (file offsets = VA - 0x400000): 0xC1EB loader reads/validates the header (MessageBox +
  ExitProcess(0xF5) otherwise -- exit code 245 = a level not converted), 0xCE7F, 0x1018C9 NOP 0x1F (DRM pointer
  check), 0x229C60 message, 0x510D NOP 5 (DRM level number). So every level 0-8 is served converted; levels with
  Roman pools also cloned (pools of CKHkBasicEnemy / CKHkBasicEnemyLeader to ceil(n x factor)) and saved with
  their sectors (sector object ids follow the level's counts). Each Roman clone: +1 collision life, +33 tests,
  CAnimatedClone + 4 CClone + 7 particle nodes + fog box + bounding shapes; ~1-2.5 s per level, ~10 s for all.
  levelforge.cpp: in DllMain (before level 0's open) reads axxl.exe, checks the patch sites hold the file's bytes,
  points the 4 call/jmp [0x622118] (CreateFileA import: 0x555F64, 0x5CC683, 0x615248, jmp 0x6176CA) at its own
  pointer -- the import slot itself is rewritten after our DllMain (ASI loader), a slot hook saw no open -- then
  patches the module; a worker builds levels into Game Files\coop_levels\LVLnnn (forge.key: forge version, factor,
  exe size, the level's LVL/STR sizes + times; rebuilt on change); an open waits for / builds its level. Game opens
  "./LVL004\LVL04.KWN". Editor round trip differs only in RW chunk-size fields of CCloneManager / 2D elements.
  Editor bug: KObjectList::getFirst<CLightSet>() indexed past XXL1's category-13 class table (garbage -> random
  crash in Duplicator::cloneNode); guarded. experimental.cpp CheckSharedPools skipped on rebuilt levels.
  XXL_ATONCE=<f> overrides squad_romans_at_once (autotests); XXL_FORGEOPENS=1 logs CreateFileA opens. Autotest
  atonce41 (-Level 4, rules4t + WATCHENEMIES): XXL_ATONCE=3 -> lock squad 18 alive (pool 54, 6 hiding still
  placed), x2 -> 12. romanforge\build\romanforge.exe <game dir> <out dir> <level> [factor] builds one level as the
  asi does. Not cloned: archers, jumping Romans (their pools keep the level's size).
- Round 255 (Nov 11, commit 920a04d; user: round 254 works on the pad; clone archers + jumping Romans, not jetpacks;
  more crates/helmets for two views): romanforge 2 fills CKHkRomanArcher (2,95) and CKHkJumpingRoman (2,75) pools too
  (LV2/LV3 archers 2 -> 6, LV4 jumping 3 -> 9 at x3). Bonuses: CKGrpAsterixBonusPool (4,63) members = CKHkBasicBonus
  (2,114); the free list is the pool's nextBonusHook chain through nextBonus (all members, in the file); each bonus
  refs the pool's bonusCpnt. Crates: CKGrpCrate members CKHkCrate (2,112): a CDynamicGround (cloned, added to the
  mesh klusters by the duplicator) and the group's CKCrateCpnt. Both x2 (kViewObjects) in every rebuilt level,
  whatever the factor: clones keep the component (Duplicator::keepShared = cloneMap[obj] = obj; the duplicator
  clones components otherwise), bonuses pushed on the nextBonus chain. LV1 +90 bonuses / +60 crates; helmets 50 ->
  100 (LV4 pool "free 100"); LV4 crate objects 59 -> 118. The forge runs at any factor now (Romans only above 1).
  New limit found: CKSrvAvoidance keeps 50 entries at +0x10, count +0xD8 right after; add 0x4600A0 (thiscall(entry),
  ret 4; entry +0x14..+0x1C push vector, +0x20) has no bound, remove 0x4600E0 swaps the last in and skips an entry
  it can't find; vf17 (0x45FDF0, its tick) iterates the count. With >50 Romans awake (LV4 x3, crates44 teleport to
  sector 4 at 9.2 s) the 51st wrote over the count -> crash 0x41CB56 writing 0x14. experimental.cpp AvoidAddHook:
  past 50 the Roman is left out (push cleared as the add does; "avoidance service full" log, cap 10). Tests:
  crates44 (118 objects, 0 in view without one; crashed before the cap), crate (iron 4 -> 0 pieces, wood 3), helmets12,
  pass22 (archer in the cloned LV2), atonce41 (18 at once). Jumping Romans not seen in a test yet.
- Round 256 (Nov 11, commit 972b674; user: round 255 works; LV2-1 with difficulty_coop_only off: Romans of a
  formation 3 in one spot, even moving; wants an ini switch for coop_levels): a squad's current choreography key
  (FUN_0052C780(squad), thiscall, no args; key +4 spots 0x1C each: +0 position, +0xC facing, +0x18 pool or -1, +0xC
  count; transformed by its node +0x30 with FUN_005918B0 / FUN_00591910, cdecl(out, in, count, node)) has as many
  spots as the level's Romans. The squad tick FUN_0052B860 (thiscall, vf31) hands them out with FUN_0052B670
  (thiscall(squad; Roman, key, spot), ret 0xC: Roman vf12(0x46) component +0 spot, +0xC facing, +0x18 step = (spot -
  Roman) x/z / max(dist, squad +0xC0), zero within 0.01; attack/movement bits +0x24); Romans left over get spot -1 =
  the squad node's +0x30 / +0x20 (centre). vf33's wake places the same ones on the centre too (0x52F416 branch, no
  FUN_0052B480 walkable snap) via FUN_0052EC30. Fix: FormationHook (0x52B860) numbers a squad's extras per tick,
  SlotHook (0x52B670) sets extra n's target to ExperimentalExtraSpot (spot n % spots + 1.5 x ceil(row/2) sideways,
  rows alternating sides; no key: round the centre); proximity.cpp PlaceRomanHook moves a wake (vf33) placement on
  the centre to the same spot (snapped by FUN_0052B480, thiscall(float* pos), ret 4). Romans sent in later come at
  spawn markers (the user saw no stacking after the first group). [coop_difficulty] rebuilt_levels (default on;
  env XXL_REBUILT): off = no levelforge at all. Autotest stack21 (-Level 2, XXL_COOPONLY=0 XXL_ATONCE=3; the watch
  now logs "stacked pairs N of M": shown Romans within 0.3 x/z, 1 y): before 121 of 27, after 0; atonce41 0, 18 at
  once; XXL_REBUILT=0 atonce41: 12 alive + 6 queued (the old pool limit).
- Round 257 (Nov 11, commit 17a0267; user: crash in xxl_coop.log, userlogs 20261111-crash-shadow-xxl_coop.log, single
  player, Obelix leading up LV4 sector 5's lift to -917.2 20.9 -337.6 at x3): CKSrvShadow (vt 0x62520C) has
  +0xC slots (30 in LV4: 0x14-byte nodes {next, shadow (vt 0x625268, 0x50), node?, caster, def}, free list +0x2C,
  in use +0x30 (def +0x28 == 0) / +0x34 (== 1)) and waiting nodes (0x10: {next, def, node, caster}, pool +0x3C, 33;
  waiting +0x40 / +0x44). Register FUN_00471050, remove FUN_004712E0 (promotes the first waiting one into the freed
  slot), out of range FUN_00471160 (thiscall(srv; node, def), ret 8; from the tick 0x470A70 when the shadow's
  update returns 1). Game bug: FUN_00471160 with the waiting pool empty skips the unlink but still pushes the slot
  on +0x2C -- the slot sits in both lists, its shadow +8 nulled; the next tick's update read +8 -> +0x2A: crash
  0x46F9B7 (all 30 +0x34 slots chained into the free list in the diagnostic). Needs 63+ casters: only with the
  rebuilt levels at x3 (x1 / x2 no crash). splitscreen.cpp ShadowWaitHook: pool empty -> the shadow keeps its slot
  (its +0x4C cleared by the tick, so it retries once drawn again). A first stopgap (skip a shadow with +8 null,
  result 3) froze the game in the corrupted lists -- removed. Autotest shadow45 (-Level 4, XXL_ATONCE=3): crash at
  8.9 s before, "keeps its slot" at 8.0 s and through to the end after.
- Round 258 (Nov 13, commit 46ef7e1; user: Snipaste_2026-11-13_05-12-04, LV6-4 after a Twister: helmets past some
  number not shiny; suggested LV5-1 170.3 11.6 283.0): a helmet is a bare CClone (no child nodes); the shine is
  one CParticlesNodeFx per bonus pool (pool +0x40 = the editor's particleNode1; +0x44 particleNode2, +0x48
  secondBonusCpnt; bonus +0x18 = its pool). CKHkBasicBonus::vf15's end (0x54D788..0x54D812) adds the bonus's
  position (frame +0x80 with bonus +8 bit 0x400, else frame +0x40) as an emission point: buffer (sys +0x44) +0x10,
  12 bytes each, count byte sys +0x40 (cleared by the walk, CParticlesNodeFx::vf14 0x41A510), sys +0x10 |= 0x400;
  "count < 0x40" -- 64 points, and the system's particles are fixed too (the buffer is a RW plugin block, 0x1000006,
  set in CParticlesNodeFx::vf10). The same 64 cap is in ~20 other emitters (left alone). Fix: romanforge 3 clones
  the pool's shine system for each 64 bonuses past the first (Duplicator::doCloneNode, under the original's
  parent) and reports "pool class id, pool index, (11,19) indexes;" (each level's 100-helmet pool: one copy; LV4
  #544, LV5 #604) into the key file's 3rd line; levelforge.cpp replaces the emission (jmp 0x54D788 ->
  EmitShineStub -> EmitShine(bonus), back to 0x54D812): the first of the pool's systems with a point left, all
  full -> one of the frame's points in turn. A first try with only the turn-taking (no copies) still left helmets
  bare (fixed particles shared). Autotests: shine41 (-Level 4, S_SHINE: bonuses near a point with a particle node
  on them -- 2-6 of ~65, originals and clones alike: not per-helmet nodes), shine51 (-Level 5, XXL_ATONCE=3,
  Twister at the user's spot): "a bonus on its copy 1", no "all full", the field's helmets all shining.
- Round 259 (Nov 13, commit 65e5b41; user: Snipaste_2026-11-12_01-18-49 / 01-20-42 / 01-22-59 / 01-30-09, Pasted
  image 20261113050057 -- the controls menu's direction icons all the left stick's, Display/Hide Interface and
  Centre Camera without one; keyboard: new keycaps coop_icons\kb, a combo's key waiting = the glowing cap, pressed
  = *_press). Strings with the game's direction icons (XXL_LOCDUMP dumps every string with an action icon and ids
  0x380-0x3AF): 22/4B/4C/38A/372 "Selection", 374 "Setting", 390 "Page", 3F-42 single icons, C0-C5 save menu --
  all the interface's; 391-394 Up..Right (moves, actions 0-3), 39D-3A0 Interface up..right (16-19), 3A3-3A6 Camera
  up/down/left/right = actions 47/46/48/49 (preset 3: right stick down/up/right/left -- the menu's own text),
  39C Display/Hide Interface (15, Start / I), 3A9 Centre Camera (50, R3 / V), 3A7/3A8 Zoom in/out (not shown on the
  pad pages; left alone). LocStringHook passes the kind (IconDirs) to IconsRewrite: all four actions on one of
  left stick (pad bytes 6-9) / right stick (10-13) / D-pad (26-29) -> that prefix with each action's bound
  direction (pairs/all as one glyph), all on keys -> keycaps, else is_*; rows 39C/3A9 get IconsAppendAction. Pad
  byte names gained rs_*. Keyboard bug: player 1's keys never got a cap -- the device's key handler (vf5
  0x433900, switch on vk) writes block 1 (Space 0x3D, F 0x25, Up 0x7D) and the bindings read block 2 (+0x90: Space
  0xCD), so MapKeyboard's byte -> vk table missed every binding; IconsP1Vk reads off - 0x90. Keycaps: kb\<name>.png
  / <name>_press.png (key_a, key_0, key_F1, arrow_up, key_space, key_enter, key_esc..., Figma's odd names: "/" =
  key_.png + key_\_press-1.png, "\" = key_\_.png + key_\_press.png, "*" = key__.png + key___press.png; modifiers
  key_lshift/rshift/lctrl/rctrl/lalt/ralt not drawn yet -> drawn caps "L Shift"...). IconLook (PLAIN / WAITING /
  PRESSED) replaces the unpressed flag: pad waiting = *_unpressed, key pressed = *_press. HintsFrame also refreshes
  all texts on a controller preset change (CKInput +0x64). Autotests: ctrlicons (pause -> Options -> Controls by
  the menus' bindings, pages 1-3; with the pad: ls_ / dpad_ / rs_down.. / pad_start / pad_r3; pad unplugged, preset
  1: arrow / F / SPACE / P / G / D / ESC caps, drawn Shift/Ctrl, V), ctrliconskb (S_SETPRESET 1 -- the menu shows
  the options' configuration, not the preset: use the real unplug), hintcatalog with XXL_ICONP2KB and no pad
  (crowd Twister 2 lit: SPACE F dark, G D / G H glowing).
- Round 260 (Nov 13, commit d7c4098; user: round 259 works; Invert Camera X / Y rows wanted below Vibration on the
  controls' page 3 -- their reverted camera bindings had stood in for it --; Vibration could not be chosen on the
  XInput pad). Options interface CKHkInterfaceOption (2/133, vtable 0x628764): vf18 0x526630 switches on +0xF0
  (page kind) -- 1 the controls, FUN_00527030 (thiscall(arg), ret 4; returns the focused element) then
  FUN_00527960(time, focused) (ret 8; FUN_00527D40 first sets every known row's label/value text layout colour to
  +0xB0, then the focused row's label (vf26 = button +0x6DC) and value get the pulse, FUN_00461A00(+0xB4)). Fields:
  +0x18 input view (InputView_GetAction 0x4358B0: 0/0x10 up, 1/0x11 down, 2/0x12 left, 3/0x13 right, 4 confirm,
  5 cancel), +0x4C Vibration label, +0x58 its value (YES 0x1E / NO 0x1F = +0x88/+0x8C; FUN_004FE9F0 sets gm +0x5C
  bit 5 and CKInput +0x15), +0x10C.. the 23 binding labels, +0x168.. their values, +0x1C4.. page element ids 10/11/12,
  +0x1D0.. the "Page n/3" buttons, +0x1F0 Current Configuration, +0x20C page, +0x214 waiting for a key, +0x21C
  frames to ignore input. Menu elements: yellow pages +0x34 -> +0x3C -> +4 the list (FUN_0042A720(id) = index; +4
  first, +0xC count); an element +4 kind byte (2 = button), +5 bits (2 shown/selectable, 4 focus), +8 its list,
  +0xC / +0x10 prev / next, +0x14 / +0x18 place; containers +0x24 the buttons' list, +0x28 the texts' (list +4
  first, +8 last, +0xC count); text layout embedded at +0x20 (+0x34 string id, +0x38 string; FUN_0042E590 on it
  sets the string and remakes its word pointers +0x2F8..), vf10 (+0x28) of the layout = colour by value;
  CColorTextButton2d 0x6E0 bytes, CText2d 0x6DC; +0x68C an RwBrush-like 0x80-byte colour object (each row its
  own; focus vf24 -> +0x6DC colour, unfocus vf25 -> +0x690; sharing it lit copies with Vibration), FUN_005F9280 sets
  its RGBA. Page 3: labels Camera up/down/left/right (3A3-3A6), Centre Camera (3A9), Vibration (0x2F), the Page
  button, the backing plate; values (3AF.., 1F). Zoom in/out (3A7/3A8) have no rows. ctrlopts.cpp: while the
  page ticks, the Vibration label/value copied (own brush), linked after them in their lists, labels "Invert Camera
  X/Y" (HintsModString slots 12-13), rows closed up 0.08 from the top; unlinked within 100 ms of the page's last
  tick (no teardown ever sees them); left/right/confirm on a copy toggles, the menu sound vf21 (+0x54)(0),
  [controls] invert_camera_x / _y written back; FUN_00527960 hooked to colour the copies' labels/values. Invert:
  CKInput values 46<->47 / 48<->49 right after the input update (camera view CKSrvCamera +0x580 reads them); player
  2's CoopP2CameraValues and p2cam.cpp follow cameras multiply rx/ry. Vibration: CKInput FUN_00437E60(pad, motor,
  strength) on / FUN_00437EA0(pad, motor, strength, float secs) timed / FUN_00437EE0(pad, motor) off (thiscall, ret
  0xC/0x10/8; skip when CKInput +0x68) -> pad device slots 10/11/12 (FUN_00432EF0 timed: an idle motor starts, a
  running timed one is extended); the device plays its DirectInput effect (+0xC4) only, so an XInput pad (no FF
  through DirectInput) never rumbled and FUN_00438B30(pad) (+0xC4 != 0) made the controls page skip the row. Now
  FUN_00438B30 also says yes with an XInput pad in, the three calls keep two motors (strength, end) and each frame
  player 1's XInput pad (Player1Pad, else the only one) gets them (x257) while CKInput +0x15 = 1 and not paused
  (pads.cpp PadsSetRumble, XInputSetState, told on change). Co-op: only pad 0's rumble is mirrored (player 1's pad;
  player 2's hero's hits not routed). Autotests: ctrlicons (page 3 up into the rows, Y on by right, X on by confirm,
  Vibration toggled, both back off, cancel -> "taken out"), rumble (S_RUMBLE: motor 0 200 for 0.6 s, motor 1 120
  for 0.3 s -> "pad 0 motors 51400 / 30840", off on time; user felt it).
- Round 261 (Nov 13, commit 5b316d6; user: round 260 confirmed -- invert and vibration work; route player 2's hero's
  rumble to player 2's pad). The game rumbles pad 0 only (FUN_00437E60/EA0/EE0 with pad 0). Callers: Asterix life
  FUN_004A4090 (states 0/1/4/7/8/9) and FUN_004A3EF0 (vf19 / InitOnce, off), Obelix life FUN_004A7430 and
  FUN_004A72A0 (the same); the trio CKGrpTrioLife FUN_0047DE80 (potion start: gm +0x40 +0x9C > 0, motor 1 255 for
  0.3 s) / FUN_00480290 (on and off) / FUN_0047FA90; machine gun FUN_004C6130 (via FUN_004C51F0), boat FUN_004F8CC0
  / FUN_004FCC40 path, mobile tower FUN_004E8DD0, FUN_004F1C50. ctrlopts.cpp: the caller's return address in a
  hero life's range -> that hero's player, else the leader's player (single player: player 1); motors kept per
  player; off clears both players' motor; player 2's pad = CoopP2Pad() (XInput only); player 1's pad before
  identification = the first connected pad that is not player 2's (it was none in co-op). Autotest rumble2 (player
  2 on virtual pad 2; S_RUMBLE with Asterix leading -> "player 1's pad 0 motors 51400 / 0", after trio 4801
  Obelix leading -> "player 2's pad 1 motors 0 / 38550", both off on time; the virtual pad is not told). The hero
  life ranges are not exercised by the autotest (its calls come from the asi): pad test with a second pad pending.
- Round 262 (Nov 13, commit b72f18c; user: new Controls rows -- page 3 Switch hero (binding), Vibration, Invert X/Y; Deadzone
  on a page 4; later phases: player 2 in the menus with its own controls, a CO-OP Options pause entry, a co-op hero
  swap). Page 4 is the mod's: the game cycles its pages % 3 (opt +0x20C; FUN_00527030 on the page button
  +0x1D0 + page*4 with left / right), so page 4 lives in page 3's container: its rows linked in but hidden
  (+5 bit 1 cleared), page 3's hidden instead while it is up; right on page 3's button -> page 4 (the game's tick
  skipped by +0x21C = 1), right on page 4 -> the game's own 3 -> 1, left on page 1 -> the game's 1 -> 3 then page
  4, left on page 4 -> 3. The game shows every child of a page it goes to, so the rows' shown bits are put right
  after each tick. Counter: loc variable 0x39 = the page (yp +0x94 vf12(var, value) also refreshes the texts); the
  count %CtrlPage_Nb% is not set in code (variable 0x3A is something else -- set once by mistake, removed), so
  CtrlOptsLocString serves string 0x390 with "4" while the rows are in. Current Configuration: opt +0x208 (1 the
  controller, 0 the keyboard). Binding capture: XInput edges on player 1's pad (B cancels) or GetAsyncKeyState
  edges (Escape cancels), the menu held off with +0x21C = 2 meanwhile. p1_deadzone: radial on the game's byte
  values (0-3 move, 46-49 camera) before the invert swap. Autotest ctrlicons (page 4/4, Deadzone 0.00 -> 0.04 -> 0.00,
  4 -> 1 -> 4 -> 3, Invert/Vibration as before); the binding itself needs a real press (pad test).
- Round 263 (Nov 13, commits 12a0ac3 + e35419a; phase 2 of the menu plan: player 2 in the menus). Pause from outside:
  the game's Pause is binding 12 (a frame of 0xFF after the input update); the pause screen CKHkInterfacePause
  (2/131) reads its view (+0x18; remap +8) actions 0-7 = bindings 16-23, action 6 = resume (event 0x4F01) = binding
  22 (the keyboard's Escape, in every preset). Menu owner: the pause beginning within 0.5 s of player 2's Back.
  Player 2 drives it by pressing the menu bindings (16-19/32-35/52-55 directions, 20/44/56 confirm, 21 cancel, 22
  resume) from its logical pad edges. Player 2's pad layout: pads.cpp turns the physical buttons into the default
  ones (P2Logical, before edges), PadsRawButtons keeps the physical; the keyboard pad gives D-pad down (hud) and up
  (switch_hero). Controls page offsets: +0x10C + i*4 the 23 binding labels, +0x168 + i*4 their value texts
  (FUN_00526540 refills them with player 1's), +0x1F0 Current Configuration button, +0x1F4 its value text,
  +0x1F8 / +0x1FC its strings (0x408 keyboard, 0x40A controller), +0x208 player 1's configuration (1 controller),
  +0x5C / +0x60 bottom bar left / right texts (right: x 0.965 alignment 2, its layout right-aligned -- the
  element's alignment byte does not centre it; a copy of the left one linked into its list was never drawn).
  Text width: layout +0x2D4 words, +0x3C8 + 8i word x (screen = aspect wide), FUN_005FDAB0 word width
  (HintsTextWidth). Player 2's Start in the pause would have left co-op: ignored while paused.
- Round 264 (Nov 13, commit 5b4d25c; phase 3 of the menu plan: CO-OP Options). Pause screen CKHkInterfacePause (2/131) vf18
  0x529660 (thiscall(arg), ret 4): +0x20 its box's element id, items +0x30 Combos (0.6), +0x34 Options (0.5), +0x38
  Quit Game (0.25), +0x3C Forfeit (0.4), +0x40 Resume (0.7) -- all CColorTextButton2d; backing billboard at 0.475
  height 0.65 (alignment 1 1: centred). Its confirm (view action 4) acts on the focused item; a copy of Options linked
  after it does nothing there, so its confirm focuses Options first (element vf24 at +0x60, thiscall()) and the game
  opens Options. Options' list (page kind 3) FUN_00526730 (ret 4): +0x100 the Controls item (its confirm: page kind 1,
  page 0, container +0x1C4 shown, the page button focused); +0xFC Audio (kind 0), +0x104 Graphics (kind 2). The
  co-op mode on the controls page: the game taken to its page 3 by pressing its page button rightwards (binding 19,
  one press per tick), the co-op rows copies of the mod's rows linked after them; cancel (view action 5) leaves to
  the list, whose tick then gets a cancel (binding 21) -- the pause again (the list's cancel applies the options as
  the game does). coopopts.cpp: the table (section, key, kind, field, range); CoopSwapHeroes swaps p1Hero/p2Hero
  (views, HUD, input follow), P2CamReset. experimental.cpp: g_pathCheck worked out each frame, the combo bar once
  touched always followed (factors back to 1 restore the game's values), squad / merchant load hooks always in.
  Autotests coopopts (pages 1/2/5, nolock and revive_time_limit round trips, back to the pause with the item
  focused), coopswap (player 2 joined: p2_hero asterix -> "heroes swapped", split_mode horizontal live; both back).
- Round 265 (Nov 13, commit 01535ae; user: CO-OP Options work; the item stayed lit after the Options screen, centre the
  pause box; phase 4: the hero swap prompt in the mod's boxes with the P1/P2 icon). The pause screen's items are
  copied while focused when the pause comes back from Options (Options focused): a CColorTextButton2d's focused
  colour is in its brush, so the copy is unfocused with its vf25 (+0x64; sets the brush to +0x690, clears the bit).
  Centring: every item and the backing +0.5 step (0.05), the backing a step taller. Hero swap (coop.cpp
  HeroSwapFrame): player 1's button sp_hero_switch_button on its XInput pad (Player1Pad, else the first pad not
  player 2's) or sp_hero_switch_key with the game window in front, its icon by the controller preset (CKInput +0x64
  = 1 the keyboard); player 2's logical D-pad up ([p2_pad] / [keyboard] switch_hero). Hold 0.6 s -> HINT_SWAP on
  both boxes (8 s), the other's press -> CoopSwapHeroes. Autotests heroswap (player 2 asks, player 1 on virtual pad
  1 confirms -> swapped; player 1 asks, dropped after 8 s), pauseback (Options and back: only Options lit).
- Round 266 (Nov 13, commit 1d6168b; user: after Quit Game -> YES -> New Game -> intro skipped, CO-OP Options' next page
  showed the controls' page 1; the ini written without spaces). The options interface's +0x214 (waiting for a key)
  is a byte the game tests for == 1 (FUN_00527030); after a quit to the main menu it held 164 (0xA4, never reset),
  and the mod's "!= 0 = waiting" refused every press, so the game's own page button logic ran (3 -> 1). Ready is now
  +0x214 != 1. IniWrite replaces WritePrivateProfileString for the menus (key = value, line ends kept); the user's
  ini's compact lines tidied once. Autotest quitnew (S_POKE8 pause, Quit Game by up from Resume, the message box's
  YES by left, New Game, CINE_ENTER x2, level 1: CO-OP Options page 2 shown, safe_sequence off/on written
  "safe_sequence = on").
- Round 267 (Nov 14, commit 8bcc5f7; user: the other distributed build, GameModule.elb run by Asterix.exe or renamed to
  .exe -- everything worked but squad_romans_at_once / crates / helmets were back at the levels' limits): GameModule.elb
  (2,674,688 bytes, md5 c1b32b95..., also in Game Files and F:\XXL PLAY) has the same .text/.rdata as axxl.exe
  (the 5 module patch sites identical) but a .files section (va 0x295000, raw 0x285000) where the DRM values are
  listed: {va, size} pairs at file 0x285000 ({0x695100, 0x2B}, {0x69512B, 0xE0}, ...; file offset = va - 0x410000),
  the 8 level headers from 0x285605 -- XXL-Editor's "Polish patch" layout. levelforge had refused it (no Ipatix
  signature). GamePatcher::DrmOffsetsX1 (vendored addition) finds either layout; ForgeModuleKnown uses it. Every
  level 0-8 built from it is byte-identical to axxl.exe's (romanforge.exe <game> <out> <lvl> 3 GameModule.elb). The
  cache key holds the exe size: switching module rebuilds once. run_test.ps1 -Exe <module> (started without the
  shell, its stdout/stderr drained -- the forge's printf would otherwise flood the console). atonce41 -Exe
  GameModule.elb: 18 at once, helmet pool free 100, through to the end.
- Round 268 (Nov 14, commit e04d01c; user: LV6-5, the last lever's door, then zone 0308 (758.7 64.1 -263.1) starts
  Caesar's cutscene; skipped by holding A, Caesar sat on nothing at the door (840 64 -286) instead of up at marker 69):
  the scene (level 6 scene 4, flags 0x1D01, one of the scenes skip_cutscenes made skippable) has a StartDoor -> 3
  PathFinding -> 3 PlayAnim (two CKHkAnimatedCharacter (2,97, vt 0x629BC0) = Caesar, both at 840.48 78.75 -286.20,
  and Asterix), and the cameras' And doors stop the PlayAnims. Ended by itself, the actors stay where it left them.
  A skip (FUN_00461A80 -> FUN_00461B10) clears the scene's +0x21 bit 0x08 (flag 0x800). That sends FUN_00461B90 down its
  fade-out branch, and once black it calls FUN_00461E20 (thiscall scene, entry; ret 4; one call site 0x461CE9) for
  every scene-data entry with +0xC bit 0. FUN_00461E20 takes the entry's end place (*(entry+0x10), actor *(entry+4)),
  drops it to the ground below with the FUN_004493C0 ray, grounds a hero via the yellow pages +0x54 list (FUN_0041CB50),
  and sets it with FUN_00477770 (ECX = the actor's +0x10 part, its +4 node +0x40 position). Caesar's end place is
  78.75 and the ray found the floor at 64.07. Found by a float-jump watch on the actor's node +0x44
  (autotest S_WATCHRW x=10, stack 462074 <- 461CEE <- 460CF0 <- 46ACEB). Fix: coop.cpp SceneEndPlaceHook (patched call
  0x461CE9) calls FUN_00461E20, then puts a CKHkAnimatedCharacter back on its end place as given. Heroes are still
  grounded. The actor's +8 bit 0 (its own 0x1202 ground snap) is no guide: one Caesar part has it and still sits up
  there. Level 1's intro skip now also keeps an actor at 22.20 (left by the scene at 22.46; it was dropped to 16.26).
  S_CINEDUMP now logs each PlayAnim / PathFinding bloc's actor and position. Autotests caesar65 (skip: Caesar kept at
  78.75) and caesar65w (watched to its end; the reference: 78.75), both -Level 6, XXL_SKIPLOG=1 for the run-on +
  end-place trace; holdskip -Level 1 regression OK.
- Round 269 (Nov 14, commits 6ec37cf, 31f9f31; user: BACKSPACE no longer works as the keyboard cheat modifier since
  the P2 menu work, even with back rebound to P; and Asterix should steer the belly slide and the boat, Obelix jump).
  Cheat modifier: before round 263, player 2's back key (BACKSPACE) gave PAD_BACK, and PAD_BACK | PAD_RTHUMB was the
  mod buttons' modifier. Round 263 made Back the pause menu and the modifier R3 only, so BACKSPACE did nothing as a
  modifier. Rebinding back to P only unbound BACKSPACE. New [keyboard] cheat_modifier (KEY_CHEAT, default 0x08): pads.cpp
  PollKeyboard sets PAD_RTHUMB while it is held, with no rising edge (CoopP2CameraValues' camera centre reads
  pressed & PAD_RTHUMB). It is skipped when it is the back key. KEY_BACK's default is now 'P' (dist ini too).
  SetP2Key's swap covers it, so rebinding back onto BACKSPACE in the Controls menu moves cheat_modifier to the old back key.
  Autotest kbcheat (keyboard injected, not run while the user is at the PC). Ride split: shared.cpp Route SHARED_BOAT /
  SHARED_SLIDE now gives trio/boat actions to Asterix's owner, except action 4 (jump), which goes to Obelix's owner.
  The camera goes to Asterix, and the mode hint is swapped. boat49o (-Level 4, XXL_ACTIONLOG=1): player 2's stick is
  no longer answered (before: "boat[0]/trio[2] answered by player 2"), and player 2's A is answered as boat[4] / trio[4].
  slide22s doesn't reach a moving slide (the camera NaN, Obelix frozen), so only the hint is checked there.
- Round 270 (Nov 14, commit 1909e8d; user: both players do the Twister, the one who started first -- the left screen --
  keeps shaking after the later one's Twister has ended). The Twister's end, FUN_00498C10 (plain, attack 9) /
  FUN_00498D70 (Fusion-ready, 10), thiscall(char ended) ret 4, run in the hero's own tick: with ended != 0 and the
  other hero (trio vf 0x90 if hero +0x18 bit 0 (AI), else vf 0x98) still in the same attack state (+0x135), it skips
  the cleanup: shake stop (svc +0xC bit 0x1000), trio +0x5B0 bit 0x20, +0x58C+0x29 bit 2, the music fade
  (FUN_004813A0 / FUN_00480230) and the yp+0x6C register. That's for the AI companion's copy ending before the leader's
  spin. With two players the first spinner ends first, so its camera shook out the full 24 s (round 113: each hero's
  Twister shake is on its own camera). gamecam.cpp TwistEnd9/10Hook: in split screen (g_active, P2 joined), when the skip
  applies and the camera in place is the ending hero's (game state for g_liveOwner, or the swapped-in copy), the
  shake bit is cleared after the original. The rest of the skipped cleanup is left for the later spinner's end. Autotests
  twistboth (P1 first; before: game shake on to 33.3 s / elapsed 24.00; now stopped at elapsed 12.03) and
  twistboth2 (P2 first: the copy stopped at 12.04). twistend / twistenda are stale: their P2 Back step opens the pause
  menu since round 263, so use S_P1COMBO -1 for the combo cheat.
- Round 271 (Nov 14, commit edf6607; user: LV5-7 machine gun at squad_romans_at_once = 3, the bullets go through
  many of the Romans spawned after the three CKHkPyramidalTurtle). CKSrvCollision's projectile pass FUN_00447FF0
  tests each live projectile (+0x20, 8 B {hit list, shape}, count byte +0x1B6, 50 slots) against the projectile
  targets (+0x1B0, 0x48 B {hit list, up to 16 shapes, count byte +0x44}, u16 count +0x1B4). That array is read from
  the level file, and XXL-Editor's Duplicator copied a clone's collision tests but not its projectileTargets entry,
  so no cloned Roman (round 254) could be hit by a projectile. Duplicator.cpp now adds a copy of every target entry
  holding a cloned shape (shapes substituted); kForgeVersion "romanforge 4" rebuilds the cache; the forge report
  logs "projectile targets N -> M" (LV5 220 -> 276 = +56 Romans; LV1 229 -> 269, LV2 308 -> 366, LV3 365 -> 443,
  LV4 300 -> 352, LV6 304 -> 376; crates have no targets). Also in the pass: at most 30 hits a frame (0x1D cap),
  untouched. Autotest gun57 (-Level 5, XXL_ATONCE=3; S_PROJHITS a = the level's target count logs targets hit,
  read between collision ticks): 2 s bursts sweeping left / right (held X overheats the gun, which then fires
  high -- user), 30 hits on the level's targets, 20 on clones (#245, #270, #274...); before, no target past #220
  existed. User saw the hits consistent on the pad (Nov 14).
- Round 272 (Nov 14, commit f8c9a72; user: xxl_coop.lv2-8_asterix_tp_fail.log, xxl_coop.lifter_softlock.log).
  (1) LV2-8 second chance: Obelix (leading, LVL LEAD) died 47 units from Asterix's wait on the cutscene platform
  (40.4 57.1 -89.4, the level's 4820); after the revive Asterix stayed at the revive spot. CoopRestorePark skipped the
  put-back past 40 units -- cap removed (the level is checked). In the autotest the wait came from the platform's
  light pillar (event 0608, a flagged group: picture "as before its event"), which NoteSoloPark never kept (a
  pillar's undo stands); the user's picture was "as now" (FFFF). Now a pillar lock is kept too once the level has
  moved the companion on (6+ units from g_lock.at, or a later order than the 4802): undone to before the pillar
  would lose the platforms and zone 02B0's scene. A kept lock gives no lead-back leader (CoopUndoLockFor -1: the
  fallen hero leads again), and the restore sends the lock group's own pillar message again (0x900).
  (2) Same log: Obelix's health row stayed up with 1 health while he was down. He fell leading -- his row was the
  game's, shown (state 0x20, +0x270 = last shown health); the swap made it the other row, and the health part
  keeps state 2 while +0x270 is nonzero (round 205's flag clearing only stops a row coming up). Hud2AfterInterfaceTick
  zeroes +0x270 for a fallen other row that is up: dropped within 15 ms.
  (3) LV2-1 single player: Asterix waited on the corkscrew (event 0050), AI Obelix left on the lifter; the lifter's
  own wait (round 94: +0x34, ~8 s) fires zone 0B30 = trio 48C4 + 488E. The 48xx handler (0x482BB0): 0x85..0xB6 put
  the leader on marker (lo - 0x85), 0xBB..0xEC the companion (lo - 0xBB) -- both on marker 9 here, Asterix still
  AI order 3. NoteLockMarkerPut (in CoopNoteTrioEvent) notes a companion put while our lock holds it;
  WatchLockMarkerPut, within 1.5 s on a lifter's own ground (IndivHeroOnLifter), undoes the lock as R3 + B does but
  without the last-ground put-back (UndoLock putBack false). WatchLifterHint's round-94 undo stays co-op only.
  (4) LVL LEAD (g_solo.levelLeader) follows the lead whenever a sequence undo hands it back (GiveLeadLater, a fall's
  unkept undo: LevelLeadBack) -- user: splift21's lead came back to Asterix, LVL LEAD stayed Obelix.
  (5) Tried and reverted (user): dropping the lifter's last-ground put-back with a downed partner. Standing on the
  lifter the hero is inside the shaft's PF node, which ShutLifters shuts for the revive's queries, so WayToFallen
  got "not known" (never acted on), not "no": nobody was brought over (liftdown21 / liftdown21r failed).
  Autotests spfarpark28 (-Level 2, XXL_SPSECOND=1 XXL_REVIVEHOLD=2 XXL_HUDPOS=1: pillar, 02B0 flagged -- an AI hero
  placed or glided sets off no zone -- Obelix killed by shockwaves 47 units off, Asterix hit, revive; before: no
  put-back, row up with 3; now back at 40.5 57.2 -89.5, Obelix leads, row hidden) and splift21 (-Level 2: squad 29
  won, Asterix on the circle, Obelix on the lifter; 48C4 at +6 s: lock undone, both ride up, the circle locks
  Asterix again later). liftdown21, liftdown21r, liftdie21a, liftdie21b, lifterlock as before.
- Round 273 (Nov 14, commit 7398e4e; user's request: nerf the plain Twister, keep the Fusion's effect; second chance
  Twisters of their own; a slower background combo bar in second chance -- three options, in CO-OP Options).
  (1) The Twister's pull, FUN_00498270 (thiscall, no args, plain ret; prologue 83 EC 1C 53 55 56 57; called from
  the spin steps FUN_00497C90 / FUN_00498560 and the fused FUN_00498A30): Romans of the trio's list (trio vt+0xF8,
  null-terminated) within trio +0x3B8 of the tornado (hero +0x17C) get 0x4C0D -- caught: their 0x4C01 flags bit
  0x400000; IsA 0x1702 ones counted per squad (enemy +0x4C) into hero +0x198 (4 slots). Its clock hero +0x18C (+=
  dt), past +0x190 (plain, attack 9) / +0x194 (Fusion-ready, 10) over the most caught of one squad, calls each such
  squad's slot 41 (0x4C8CB0, (1, -1, a spot near the tornado)): one Roman still to come taken (+0x16 up to +0x18,
  0x4D09 = the gate counter, a helmet), clock to 0. With -1 slot 41 picks a random record with Romans left and caps
  n at that record's rest -- so n calls of 1, not one of n. Measured LV4-1 field (twnerf41, squad 282 to defeat):
  game's -- 18 -> 57 -> 239 -> 282 taken in 9 s of spin.
  [coop] twister_nerf (on, XXL_TWISTERNERF) + twister_nerf_per_roman (4, 1..10, XXL_TWISTERPER): TwistPullHook --
  not for sub-states 0x25..0x27 (fused), co-op or SoloMod(): clock held (-1e30 before the call, 0 after), then each
  caught IsA-0x1702 Roman not counted yet (g_caught: the ones still flagged; reset after 1 s with no pull) takes
  per_roman - 1 more off its squad (slot 41 (1, -1, +0x17C) each); the Roman itself is already "taken" (placed).
  twnerf41 nerf on: 18 -> 78 -> 99 (caught ones only); per_roman 1: no extra. twfuse41 (co-op, -Level 4): plain
  part counted per Roman, then fused 60 -> 203 -> 282 (the game's run-down).
  (2) Second chance (IndivActive, no player 2), each hero its own Twister, not tied to the nerf (user):
  ChooseAttackHook -- an AI hero not in 9/10 while the leader twists asks the chooser with the leader's +0x135
  hidden (no copy; a 9/10 result -> current); an AI hero in 9/10 (its own, begun while it led) asks as the player
  variant (+0x18 bit 0 cleared for the call): the AI case's exits (leader body sub-state 0, leader +0x3E9 9, +0x564)
  would end it at once. The switch (trio 4800/4801) only moves +0x18; the Life state changes at the hero's next
  Life tick (CKHkAsterixLife::Tick 0x4AA9C0: state = (f18 >> 1) & 0xF vs life +0x1D; handlers from the table at
  life +0x18 -- NOT the vtable: a vtable-slot patch never ran). The old state's exit (State0_Player 0x4AAAE0 /
  0x4AB900, State1_Ai 0x4AACE0 / 0x4ABB00, phase 2; prologue 8B 44 24 04 56 8B 71 10 for all four) calls the attack
  handler's exit (10: 0x493F70 -> 0x494130 -> Twister end 0x498D70), sets +0x135 0xFF, and puts the body +0x38E
  0xE3 -> 0xC0 (without keeping it: attack 10 with sub-state 0, hidden, never ending). SoloHeroSwitch marks a
  spinning hero (g_keepTwist, 1 s); KeepTwistExit hides +0x135 and restores the body around that exit; the entry
  leaves both alone. (Bug on the way: the mark was GetTickCount() | 1 compared unsigned -- 1 ms in the future on
  even ticks: 1 run in 3.) SoloSwitchRefusal: a twisting hero off its feet (the tornado lifts it) may be switched.
  TwisterNearness in the solo frame with range kOwnTwistersNear 8 (trio +0x3D8 is 3; the AI's own tornado drifts --
  4.3 apart in a run); once marked the AI's spin (FUN_00498560) follows the leader's. CoopFusionAllowed: second
  chance also needs combo left on the other hero's bar (IndivStats gauge >= 0).
  (3) [single_player] sp_second_chance_combo_drain (0.33, 0.05..3, XXL_SPCOMBODRAIN): RunDownCombo (the off-lead
  hero's bar; the leader's runs in the gm tick) scales dt in second chance (g.solo) -- full gauge and hit decay.
  (4) CO-OP Options: twister_nerf, twister_nerf_per_roman (1..10 step 1), sp_second_chance_combo_drain (0.05..1,
  step 0.01) -- 44 rows, 5 pages (coopopts: page 2 / 5 shown). gamecam TwistEnd logs the caller of a Twister's end
  in single player (40 cap).
  Autotests (-Level 2, XXL_SPISOLATION=0 XXL_SPSECOND=1 XXL_SPSWITCH=1) spowntw2 (Asterix's Twister, Obelix stands;
  switch, Obelix's own, X: Fusion -- 6/6 after the fixes) and spowntw2b (switch back while both spin: both kept,
  Asterix's X fuses -- 3/3); twnerf41 / twfuse41 (-Level 4); fuse2 (co-op) unchanged.
- Round 274 (Nov 14, commit after 7f4cc60; user: round 273 works; the off-lead full bar still ran out at the game's
  pace; the second chance Fusion's lightning one still line; userlog Game Files xxl_coop.log of 08:14).
  (1) The user's log: Asterix's Twister at 45.5, switch 46.9, his bar's HUD row (hud2 "bars p2", itf +0x24 & 0x7000)
  state 2 -> 3 at 51.7 -> 0 at 52.0 -- out 6.5 s after the Twister, the game's pace; X at 53.6 refused (no combo
  left). hud2 ticks the other hero's combo bar part (kComboPart FUN_00518190 / glow 0x519E70) silently with its
  stats in place; the part's states run on the clock's dt (param +0xC, the only field read): state 2 counts +0x2BC
  down then slides out (state 3) and zeroes gm +0xA4; state 4 (full) counts its fill +0x2E0/+0x2E8 down and at 0
  clears hits and the full state -- whatever RunDownCombo did to the gauge. Now the solo row's combo parts get a
  64-byte copy of the clock with +0xC x sp_second_chance_combo_drain. spbarslow2 (-Level 4, potion, the cheat fill
  via S_SPMOD D-pad left, switch with the bar full): x1 out in 23 s; x0.33 13.4 of 24 left after 30 s. (In autotests
  the HUD's combo bar never shows -- the potion's gauge takes its place, and without it Asterix fell -- so the
  HUD-part path itself is covered by the user's log and the decomp, not a run.)
  (2) The pair's "partner near" (+0x189) set by the AI's own spin (FUN_00498560, 0x49868B) is followed by
  FUN_00480930 -- thiscall on the TRIO (ECX = hero +0x14 at 0x498699), float 2 x trio +0x3AC, ret 4: the trio's
  link bolt object (+0x4E4, from yellow pages +0x58 class 0x194C, its vt+0x38 started), flags +0x524 |= 7, +0x4EC
  length, +0x4F0 5, +0x4E8 2; the spin / fused steps then move it (FUN_00480A30 endpoints +0x4F8..+0x50C, FUN_00480A90
  / 0x480EB0 random bolts, +0x528). TwisterNearness marked second chance pairs itself (range 8): the link never
  started, one still bolt. Now, with a range given (second chance), the first marking calls FUN_00480930 as the game.
  spowntw2 / spfuse2 (XXL_SPSECOND=0, the game's) got bursts of shots (0.35 s apart) in the fused tornado: the bolts
  move in both now. Co-op's marking (range 0) left as it was.
- Round 275 (Nov 14, commit ee7af04; user: switching during the Fusion handed over an invisible hero, the fused tornado
  stayed behind; second chance's "Fusion-ready" looked always on, even when the Fusion was not affordable).
  (1) SoloSwitchRefusal: a hero in sub-states 0x25..0x27 (merging / fused / its end) or the leader's +0x188 up ->
  "the Twister Fusion" (kSwitchNoView[6]: no sidekick view). spfuse41 (-Level 4, potion, both own Twisters; Obelix
  starts 6 units off -- beside the starting tornado he is lifted and the first switch refused "off its feet"): fused
  at 13.985, the switch at 16.969 refused, the fused tornado ran on.
  (2) The Fusion's price, logged: gm +0xBB = 40 levels, +0xBC+2 (Twister) 30, +0xBC+3 (Fusion) 1, full bar +0xA0 30
  -> the Twister takes 22.5, the Fusion 0.75; FUN_00493810 with less empties the bar and the fusion goes on anyway.
  CoopFusionAllowed (FuseHook's refusal and the combo hint) now needs the leader's gauge >= the price (individual:
  IndivStats, live for the leader via Current; else stats +0x10 bit 0x4000 and gm +0xA4), second chance also the
  other's bar still full. combohints FuseFrame's range 8 in second chance (as TwisterNearness). Attack states left
  alone: the AI's own Twister stays 10 after its bar runs out (the game's companion would drop to 9 with a fresh
  start pose -- round 204's "re-do").
- Round 276 (Nov 14, commit 15d2e8d; user: the "Fusion-ready state" = both spinning, the companion's tornado keeping to
  the player's at a set distance, the strikes and the lightning joining them -- still there with Obelix's bar empty).
  That look is the pair link: +0x189 on both (FUN_00498560's AI branch then follows the leader's +0x17C) plus the
  trio's link (FUN_00480930: bolt +0x4E4, strikes +0x528). In the game it ends with the bar: the companion's chooser
  gives 9, the Twister end FUN_00498D70 calls trio FUN_00480E90 (+0x528 vt+0x48, zeroed) and FUN_00480A70 (+0x4E4
  likewise), both thiscall no args plain ret. Second chance's own Twisters stay 10, so it stood on. Now:
  SoloFusionLink (solo frame, IndivActive, before TwisterNearness): a link up, neither hero in 0x25..0x27, and
  CoopFusionAllowed false -> both +0x189 / +0x188 cleared, the two trio calls. TwisterNearness (range > 0) links only
  while allowed. SpinStepHook (0x498560, prologue 83 EC 38 56 8B F1 57): an unlinked AI hero in second chance while
  not allowed runs it with trio +0x3D8 = -1 (no self-link within 3 units). spfuselink41 (-Level 4, no X): linked at
  13.6, undone at 17.7 as Obelix's bar fell under 0.75, stays undone, no lightning (shots linked / unlinked_2);
  spfuse41 still fuses, switch refused in it.
- Round 277 (Nov 14, commit 20fe9f9; user: the Fusion-ready look should stay while the companion's bar can still pay --
  switch to it and fuse).
  SoloFusionReady(t): both heroes attack 10 and CoopFusionAllowed(leader) or the other's IndivStats gauge >= the
  price (FusionCost) -- SoloFusionLink, TwisterNearness (range > 0) and SpinStepHook ask it. CoopFusionAllowed
  (X: FuseHook, the combo hint) = the leader's bar pays, the other's "still full" dropped. Found on the way (the
  first spfuselink41 runs): Asterix fused with 1.5 left, paid 0.75, stood under the price -- FuseHook's refusal ran on
  Obelix's join check (the leader's +0x188 is what the AI partner joins by, FUN_004984F0) and cleared it: Asterix
  merged (0x25) for good, Obelix spun on (0x23). The refusal now skips a leader already in 0x25..0x27 (paid). Also
  the "off its feet" switch refusal names the hero (LV4-1's crowd knocks Obelix airborne now and then: the test's
  first switch refused). spfuselink41 (-Level 4, Obelix 11 units off at the start): Obelix's bar under 0.75 at ~18.0,
  link kept on Asterix's 1.8, switch back at 18.47, Asterix's X 18.89, 0x25 x10 -> 0x26 x120; spfuse41 and twfuse41
  (co-op) fuse through 0x26.
- Round 278 (Nov 14, commit ce368e4; user: LV6-4 with to_defeat/at_once 3, the gate's 3000 jumps to ~1000 under the
  Twister, 1100 after a death -- xxl_coop.lv6-4_from_3000_to_1100.log). TrimStuck (experimental.cpp, the 6 s
  "none of its N Romans left to send came" safety net) fired on squad 8's pool 2 (950 -> 2850, at once 0: sent by
  the script in waves) while the Twister had beaten every Roman alive and the script paused -- 1900 taken with ~900
  of the level's still to come. The squad's counts survive a game over (only sent/alive reset), hence 120+30+950 =
  1100 after. Now the trim needs left-to-send <= own+extra (the level's own all sent: the "sent >= n" script end
  it was made for). twist64 (-Level 6, taps past the spy talk as fight64, 10 Twisters at 358 42 -290): before, trim
  at 27.3 s (2898 -> 978); after, 3000 -> 2578 steadily, no trim.
- Round 279 (Nov 14, commit 2158cf8; user's new difficulty settings: Romans' faster charge, a shield for every enemy
  (breakable as the guards' / infinite as LV2-3 squad 25), more hits to defeat, more blows on a shield, a shorter
  weaponless daze, the heroes' permanent shields 1-6 + extra 0-6 -- "shields" = the health bar). toughness.cpp, all
  live, all in CO-OP Options (page 4):
  - Enemy component (pool record +4, shared by the pool's Romans; hook +0x54) offsets from the deserializers
    (CKEnemyCpnt 0x4F4F00, CKSeizableEnemyCpnt 0x4F4370, CKBasicEnemyCpnt 0x4F4A80): +4 flags (0x10 has shield, 0x40
    infinite, 0x08 can be disarmed), +8 health (the hook takes it at a send, Shared_Class_2_88_vf16 -> hook +0x48),
    +0x110 shield points, +0x111 extra (seUnk28), +0x114 cover time, +0xF0 the daze (seComboStunTime 5.0), +0x154 the
    wind-up (beChargeDuration 1.5: FUN_004E3F60 reaction 0x2A while hook +0x260 <= it, then 0x29 the blow; the
    animation at 0.9 / it, CKHkBasicEnemy::vf36). Seizable +0x94.. +0x11C and basic +0x130.. +0x170 as XXL-Editor's
    field order (map in toughness.cpp's head).
  - Shield (Shared_Class_2_84_vf28 0x4CF970, kind 0 front, reaction not 1..5): hits in a row +0x148 += 1; <= points
    blocked (+0x24 |= 0x10000, cover); > points + extra blocked only with 0x40; between: lands. Never blocked while
    disarmed (+0x24 bit 0x8000) or a strong blow (kind 3). +0x148 goes back to 0 only as a block's cover ends
    (vf30 0x4CB840: +0x144 over cover time +0x114) or a send (vf15/vf16). A Roman reeling (reaction 1..5) skips it:
    combo blows after the first land. LV2-3 squad 25 (poolkinds -Level 2): flags 03007ED9, points 0, extra 2 --
    two blows land, then blocked.
  - Disarm: event 0x4C05 (Class_2_84::sendEvent class 0x4C case 5) or a reaction-4 blow with flag 0x08 sets +0x24
    bit 0x8000; vf30 then vf56; vf52 0x4CD260 clocks hook +0x128 to +0xF0 (stars past 0.66 of it, FUN_004CCC80) and
    clears the bit (the only place). Asterix's combo action 3 (S_P1COMBO 3) disarms a crowd.
  - Health bar: the hit FUN_00495FA0 drops a shield (shields > 3 and health <= shields*5 - 5) -- bought/picked
    shields are lost as their part empties, never under 3; a pit (FUN_0049DD30) takes one regardless (left so).
    Immediates patched live: 3 at 0x50146B / 0x501B5F / 0x4FE170 (gm vf12, vf13 respawn, vf18 new game) and the
    hit's floor 0x49611E; 15 at 0x501467 / 0x501B2F / 0x4FE174 (base*5); 6 at 0x507232/38 (add a shield),
    0x4BB050/A9/AF (bonus pool FUN_004BAFB0), 0x4BFD5E / 0x4C001F/25 (merchant). Clamp each frame: no more than the
    most (game place + IndivEachShieldCopy); raised to the base only as it changes / a level begins (options on).
    individual.cpp's shield pickup rule uses ToughnessHeroShieldsMax.
  - Modes: enemies with ExperimentalMode() == 2 (the squads' rules on; the game's own while apart / SP coop_only);
    the bar with mode != 0. Components keyed by pointer, originals re-taken when a component no longer holds what was
    put in (a reload); cleared out of a level. Shields only for basic Romans and leaders whose model has no lion /
    pirate / normand / dompt / barberousse texture (GuardModelTextures). Turtles, tower, jetpack: KeptAsIs.
  - CO-OP Options went past 48 rows (51): kCoopMax 64, rows 48+ on text slots 130.. (128 is the pause item's);
    hints' kNumStrings 176. K_SHIELD kind (off -> breakable -> infinite).
  Autotests: tough41 (-Level 4, XXL_TOUGHWATCH=1 XXL_OBELIXPOTION=0 + XXL_ROMANSHIELD/HEALTH/SHIELDHITS/CHARGE/
  DISARM, XXL_HEROSHIELDS/HEROEXTRA): wind-up 0.73 s at x2 (1.5), 6 hits at health x2, infinite blocks at hits in a
  row 3, breakable x1.5 -> 3 points blocks, S_DISARM daze clock 5.01 -> 2.01 at 0.4 (weaponless 6.5 s -> 3.5 s; a
  combo's knock-down before it adds ~2.8 s), bar 4+1: 3 -> 4 at the start, given shields stop at 5, hits no longer
  take the 4th; 2+0: 3 -> 2, health 10. coopopts2 (-Level 2): page 4 rows, roman_shield cycles, page 6 (rows 45-50).
  Not verified: a charging Roman hit through an infinite shield (the user's account of squad 25: the game's own).
- Round 280 (Nov 14, commit 22d524a; user's three requests). (1) A launcher and the belly slide wait for the other
  player, as the boat does: co-op only, the other hero within 12 units (6 up or down), not for a downed or
  level-held partner (CoopRideMateNear). Launcher: the X (hero +0x3EC bit 0x10) with its offer (+0x3F0 bit 0x4000,
  launcher at +0x544) -- the launch is started by the body machine after the take step FUN_00499CF0 (State0_Player:
  ReadCommandBlock, ..., 0x499CF0, ..., Hero_TickBodyMachine; the launcher state FUN_0049C030 enter sends 0x5131
  to the launcher, then 0x4804 / 0x4879, too late to refuse there); TakeHook (the 0x499CF0 detour) clears both bits
  for that tick: no launch, no punch, the prompt back next frame. Belly slide: FlagEventHook (proximity.cpp), a zone
  whose group starts the slide (GroupStartsSlide: CKAsterixSlideFP 0x6259E4 or trio 0x480B) with nobody down is
  held back and the hero put back from the detection service's frame-before place (IndivPutBackFrom: the downed
  hero's put-back of round 65 without a downed hero; PutBackFrom / PutBackSafe in individual.cpp). (2) Every such
  refusal (boat, machine gun, launcher, slide) shows "[hero icon] is waiting for you" on the far player's box
  (CoopWaitingForYou, HINT_MACHINE), the refusal sound on the waiting hero (once per 0.5 s). (3) Single player,
  sp_hero_switch (SoloMod): "[switch button]  Switch to [other hero]" (HINT_SWITCH, 6 s, player 1's box) once per
  game load after the join hint is gone, and again whenever sp_hero_switch_lock drops a game switch that leaves
  the level's lead (LVL LEAD) with the hero not leading (SoloDropsSwitch wrapper -> NoteSwitchHintAgain); a switch
  ends it. (4) Co-op hero swap: only the other player's box asks "Confirm hero switch [its button]", for as long as
  the asker holds its button (no 8 s timeout, no "waiting for" box); let go = gone and no confirm.
  Autotests: launchfar (P2's Obelix on sector 2's launcher, Asterix in the sas: refused, P1 box hint; Asterix
  beside: launched), launchfara (P1's Asterix, hint in P2's box), slidefar (LV2-2 cross detector: put back to
  -906.0 524.9 -905.2, hint in P2's box; Obelix 6 units off: slide starts), swhint21 (XXL_SPISOLATION=0
  XXL_SPSWITCH=1 XXL_SPSWITCHLOCK=1: first hint 6 s in, keg 0348 -> "Switch to Obelix" again, the switch hides it,
  zone 01E0 -> "Switch to Asterix"), heroswap (confirm while held swaps; let go -> prompt gone, a later press swaps
  nothing). Autotests show the keyboard's TAB icon in SP (the game's keyboard preset): the pad shows D-pad up.
- Round 281 (Nov 14, commit b340f44; user request: combos under the magic potion). potioncombos.cpp, [coop]
  potion_combos (on; CO-OP Options; co-op, single player with sp_isolation off), the hero's potion = +0x388 bit 4.
  - Club / Power Hammer: the shockwave is the trio's (FUN_0047CE70, thiscall(trio, pos, params at trio+0x45C);
    called at 0x493477 by the Club -- once per impact, 6-7 a Club -- and at 0x493B8C by the Power Hammer, esi =
    the hero; visual nodes trio +0x48C / +0x458). Its volume reaches the Romans as contacts (event 0x300 to
    Class_2_84::sendEvent 0x4CBA60; list {next, pair}, pair {vol a, vol b, ..., +0x1A type 'G' none}, volume +0x19
    kind, +0x20 owner): the other volume kind 4, owner the trio (CKGrpTrio vt 0x6256A0) -> push, +0x24 |= 0x80,
    with component flag 0x08 also 0x8000 (disarmed). Call sites patched with thunks noting the hero; within 2 s of
    a potion shockwave a trio kind-4 contact beats the Roman.
  - Beaten as a catapult stone (0x301 case, 0x4CBF48..0x4CC064): velocity +0x58 = (away x, 0.33, away z)
    normalised x component +0xFC, hitter +0x1A0 = 0, +0x24 |= 0x402. Bit 0x2 is state 1 first in the chooser
    (vf26 0x4D1220) whatever else is set; 0x4C02 does state 1 without the flight. The gate counter counts them.
  - Mole: FUN_004966E0 (step 0x1E..) sends 0x4C0A to the Romans over it until CKHkAsterix asteUnk0 (+0xB60, int,
    LV4: 8) are taken (also the 0x4C0A time: cap x 3 + +0xB74 + 1). Steps +0x38E & 0x3F 0x1C..0x20 (handler
    0x494160). Obelix's +0xB60 is something else (sound data). Tripled while Asterix has the potion, switched
    only between Moles.
  - Rage: Idefix's bites are event 0x4C0C to the Roman (case 0xC: +8 kind bite, +0x24 |= 0x80, vf41; answer byte
    1 if taken, 0 with +0x24 bit 0x20); the disarm follows from the Roman's own tick. Rage = Idefix +0xB91 (bites
    left) set from +0xB90 (3 on LV4) > 1, a plain RB bite is 1; tracked from > 1 until 0. Under the potion (gauge
    > 0) a taken bite in the Rage beats the Roman (pushed away from Idefix).
  - Club combo in autotests: player 2's taps 250 ms apart (combohint2's A, A, X, B) -- the S_P1COMBO 0 injection
    does nothing (P1 Obelix on the ground or landing), CLUB's tough41 timing did not take on LV4 this time.
  Autotests (-Level 4, XXL_POTIONCOMBOS=1 / 0): potcombo41 (Power Hammer 25 beaten / disarmed only; Mole 19 of 24
  / 8 of 8; Rage 3 bites 3 beaten / disarmed), potclub41b (two Clubs, 7 shockwaves each, 21 beaten), potclub41
  (P1 Obelix via XXL_P2HERO=asterix: the injected Club does not start -- kept for reference).
