# Co-op mod for Asterix & Obelix XXL (2004, PC)

Adds LEGO-TT-style drop-in co-op to the original PC release: player 2 (P2) presses a button on a second controller and takes over whichever hero player 1 (P1) is not currently playing as.

##  What's unique about this co-op mod

- Individual Hero mode
	- Each hero has separate health and combo bar
	- Death one of the heroes will start a "revive" sequence, where companion holds "Grab" button near fallen to revive. Revive will restore half of the original health.
- PC version general action icons are replaced with bindable controller button icons (all icons are made by XenoYparxi)
- Co-op difficulty
	- Romans spawns twice as much, and a required fight also has twice as much romans to defeat.
	- Difficulty is dynamic: when players are together - difficulty in a level is increased, and when apart - difficulty is restored.
- Single-player Second Chance
	- It's Individual Hero mode but for single-player.
	- Has unique Twister Fusion gameplay, where each hero has to get active Twister Combo in order to activate Twister Fusion.
- Hero switch button for single-player, switch heroes on demand.
- A hint system
- Mod's options can be changed in-game pause menu, including P2 controls.
- Skip cutscenes
- Language support: English, French, German, Italian, Spanish.
- More gameplay changes, such as:
	- Twister Combo nerf, Twister no longer rapidly decreases a squad's roman counter (Twister Fusion is unchanged).
	- Magic Potion + Combo interactions. Power Hammer or Club combo shockwaves 1 hit defeats enemies like a catapult's projectile. Mole combo can take x3 as much romans.
	- [[#Upcoming features]] will be included here as well when completed.

## Included enhancements

- `xxl_widescreen2d` restores wide 4:3 UI for any resolution size + 4x resolution font re-render for every language
- `xxl_timerfix` fixes timing issue with DSOAL that is implictly applied in IndirectSound wrapper

## How to install

Prerequisites:
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases)
- [DSOAL](https://github.com/kcat/dsoal/releases)
- [XXL1 QoL patch by AdrienTD](https://www.speedrun.com/id-ID/aoxxl/resources/gvmu0)

To install the co-op mod:
1. Download release package
2. Extract the archive where game executable is (`Asterix.exe`)
3. Make sure that prerequisite dll's exist in the same game executable location.

## Local co-op over internet

Current solution is to use **Steam Link** for Steam's Remote Play feature for added Non-steam games in your Steam library.

Person over the internet:
1. Install Steam Link, which can be downloaded on Windows [here](https://media.steampowered.com/steamlink/windows/latest/SteamLink.zip) ([from this source](https://steamcommunity.com/app/353380/discussions/8/5943120984229696131/)).
2. Sent a 4 digit PIN to the host person.

Host person with the game:
1. Add game executable as non-steam game to your Steam library, it can be `axxl.exe` or `Asterix.exe`
2. Go to `Settings > Remote Play (tab) > Computers & Devices (section) > Pair Steam Link (button)`
3. Use 4 digit PIN that was sent from the internet person.

Now the Internet person can connect to host PC via Steam Link remotely, where you/them can launch the game from the Steam library. When done playing, it is recommended to Unpair the connected Steam Link on the same Remote Play tab in the Steam settings, so that person wouldn't be able to connect to your PC by mistake later.

## Known bugs

If you encountered a bug, then please share it with saved `xxl_coop.log` (that gets written while game is running).

It is recommended to play with `cheats = on`, so you can save some softlock situations, where the most notable actions you can do are:
- Teleport other hero right beside your hero `R3 + A`. Usually done in co-op mode.
- Unlock any "wait here" locks on the controlled hero by `R3 + B`. Also usually done in co-op mode.

List of known issues:
- Switching to Asterix in LV2-6 will make Obelix as sidekick go to the Asterix's "wait here" place, and can't be switched back (unless you go into co-op and do manual unlock).
	- **Solution**: if heroes are connected in pathfinding, then hero switching is allowed when sidekick is currently locked in place, where the previous hero you were leading will take the place on "wait here" marker.
	- **How to continue playing**: Enter co-op; Press `R3+B` on Obelix's controller
- Obelix boarding the LV5 boat and completing it will let Asterix be behind
	- **How to continue playing**: Enter co-op; Press `R3+A` on Obelix's controller to bring Asterix
- Unknown crash `xxl_coop.unknown_crash.log`, details:
```
[13166.672] coop: Obelix takes the lead from Asterix (the hero switch button) -- leader now Obelix
[13166.688] CRASH (first chance): exception C0000005 at axxl.exe+0x166015
[13166.688] CRASH   reading address B20C1245
[13166.688] CRASH   eax=B20C1241 ebx=048FBCCC ecx=04DEB97C edx=FFFFFFFF esi=04DFC0B4 edi=00000000 ebp=048FBCCC esp=0019F954
[13166.688] CRASH   stack: 41DB27 420467 42056F 41E48E 41DA65 41DA30 41CF0D 41E8D1 413EED 61D098 40A58C 409AF3
[13166.688] CRASH   co-op: p2=- suspended=0
[13166.688] state[CRASH  ]: Asterix f18=63 mode=normal ai life=01 sit=FF body=16 ai=00 order=0 f1A=00 busy=0 pos=(567.18 -447.52 -191.99)
[13166.688] state[CRASH  ]: Obelix  f18=60 mode=normal PLAYER life=00 sit=00 body=0E ai=FF order=0 f1A=00 busy=0 pos=(567.60 -447.89 -193.71) LEADER
[13166.688] state[CRASH  ]: Idefix  f18=03 mode=normal ai life=FF sit=FF body=03 ai=00 order=0 f1A=00 busy=0 pos=(568.53 -447.52 -192.51)
[13166.688] CRASH (unhandled): exception C0000005 at axxl.exe+0x166015
[13166.688] CRASH   reading address B20C1245
[13166.688] CRASH   eax=B20C1241 ebx=048FBCCC ecx=04DEB97C edx=FFFFFFFF esi=04DFC0B4 edi=00000000 ebp=048FBCCC esp=0019F954
[13166.688] CRASH   stack: 41DB27 420467 42056F 41E48E 41DA65 41DA30 41CF0D 41E8D1 413EED 61D098 40A58C 409AF3
```
- After manually switching to Obelix in LV1-1 and entering the village, you can't switch manually back to Asterix anymore. Loading a different level doesn't fix this problem, only full game restart.
- Leader mismatch in co-op and single-player `sp_hero_switch_lock = on`
	- Were found in co-op playthrough (without toggling it). Yet to be documented with exact scenarios to reproduce. 
	- SP LV5-4 Trio Catapult activation with Asterix doesn't change the `LVL LEAD` to Asterix on landing.
- Twister Combo hint always shows after buying in LV4
- Pressing DPad Up in the in-game save menu will play the block sound that doesn't allow a hero switch
- Sometimes the sparkles on pressing the button sequence in the combo hint can be behind a prompt box (noticed in LV3-1).
- Vitalstatistix model in LV6-4 is floating due the cutscene skip
- Flask texture gets corrupted at some point
- Romans can still spawn in view. Test it in LV2-3 cave
- Skipping the spy in LV2-1, and leaving Asterix on the cable car, will make an awkward talk with the spy (no softlock though).
- With modified charge speed romans can reset their charge (noticed on the destroyed bridge sector in LV4)
- Modified Roman combat lock requirement has a sudden jump in amount in the LV6 1500 fight (encountered with 3x at once/3x requirement)

## Small design fixes/additions

- Add colored glowing on heroes after leader switching, so it can be visually easy to understand (without relying on a sound or camera change). Basically the same way as in XXL2.
- Add Roman's "stunned" state time multiplier as an option in co-op difficulty.
- Asterix's torch HUD should be shown on the screen of the player that controls Asterix. Flask too (on the right side of P1 vertical split-screen (left screen), and on the left side in P2 screen (right screen)).
- "{hero} needs help!" should be shown on the alive player's side, where after getting close enough to revive, it will switch to "Hold B to revive" prompt, but that prompt with timer will be on the other side of the screen for reference
- When switching heroes on Twister, the romans should stop spawning, like it is when the one who activated the twister makes the romans spawn stop 
- Player shouldn't be able to revive a companion when twister is active
- Probably need to slightly increase revive catapult range, so there won't be an edge where it still can fire
- Hide "Use" prompt on the machine gun for Obelix, if romans nearby are preventing to use it.
- Sidekick view shouldn't be shown in the shop

## Upcoming features

- Bonus rewards like in XXL2.
	- Shows a specific sequence of button presses to perform
	- The vanilla x3 and x10 are now locked behind this system
	- Some rewards require a full combo bar (shield, flask), and can include purchased combos in required button sequence of a reward
	- Rewards are based on the current heroes situation
		- is it a big fight, or a small fight based on amount (doesn't depend on the presence of combat lock) -> only big fights have flask/shield rewards.
		- low hp -> higher chance to get 5x/10x hams; otherwise you'll get more of 3x/5x hams and multipliers
	- Sequence reward that requires a companion interaction are disabled during revive sequence
- New combos (for fun):
	- A new combo with active magic potion for Asterix:
		- After activating it, on X (action) press Asterix will very quickly glide (like a teleport but with after images) to a random Roman (with a prolonged green trail depicting his path) with a punch in midair, that will stun Roman, where a second X press will send that Roman off (defeat), and at third X press will repeat a cycle by going to the next available Roman. There is a slight delay between possible X presses. When Roman is in a stun state, you can press B (grab) that Roman and do "whip" blast with them on the ground creating a small shockwave that can defeat nearby Romans, where pressing X next will repeat the cycle. Asterix's quick glide will do 1 hit to Romans in his path. 
		- Combo button sequence is `A > Y > X`
		- Can be purchased starting from Normandy for slightly cheaper price than Mole Combo.
	- A new combo for Obelix:
		- After activating it, Obelix gets from a ground a huge boulder, where the are a few options:
			- Pressing X will make Obelix go into spinning action, where rapidly pressing X makes the spin faster (lasts up to 8 seconds), where each hit with the bolder on Romans acts as regular X action hit, where with enough hits an enemy can be defeated like with regular hits (only on fast spinning speed). Has a unique "break shield" trait, where shield hits aren't counted in order to hit Romans. On timeout, Obelix just lets go of the boulder, which will depending on spin speed: defeat/disarm everything on its flying path in random direction + a small shockwave on its end.
			- Pressing Y will make Obelix launch the boulder straight, which will disarm those enemies that are getting hit by it
			- Pressing B does a blind throw upwards that will act as a catapult's projectile which will defeat enemies on impact of the shockwave
		- Has the same button sequence as Mole Combo, and the same environment limits (i.e. solid ground required). 
		- With active magic potion, instead of 1 hit, the enemies are defeated instantly (and also never disarmed). The spinning animation is faster too, along with the boulder's "let go" range.
		- Can be purchased starting from Gaul for the slightly higher price as Mole Combo.
- "Power up on demand" feature
	- Asterix gets 1 full magic potion and 1 drop of potion that can be activated on demand. Flask container is breakable, when Asterix gets 3 hits, then the flask container breaks and only a drop of flask can be always used. A drop is still available after using full magic potion too. A drop of magic potion is not quite the "full magic potion", but it is used in order to increase the strength temporarily (the same duration as the full magic potion), where Romans will take twice as much hits (or have twice as less hp). Intended to be used when companion is down, but can be used anytime. Slot refills are at the save points (from the druid's cauldron obviously). Obelix doesn't has "full magic potion" slot, instead he has 1 "Fury" slot that have the same functionality as a drop of magic potion. Refill happens on Obelix getting full health (3 full default health shields), and there is a 2 minute wait between uses. Asterix's Magic Potion drops and Obelix's "Fury" aren't shared between heroes, each hero can use it individually with this feature.

## Acknowledgments

- [XXL Editor by AdrienTD](https://github.com/AdrienTD/XXL-Editor)
- [XXL Inspector by AdrienTD](https://github.com/AdrienTD/xxl-inspector)