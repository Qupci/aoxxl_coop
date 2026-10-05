XXL Inspector together with the XXL QoL patch
=============================================

Both want to be the game's d3d9.dll. The QoL patch chains to "apd3d9.dll"
when that file exists, so: keep the QoL patch as d3d9.dll and put the
Inspector next to it as apd3d9.dll.

The stock Inspector (xxl-inspector\bin\d3d9_xxl1.dll) cannot run that way: it
starts by hooking the game's entry point (too late when loaded by QoL), chains
to apd3d9.dll itself (= itself), and patches the window class and the
projection function. xxl-inspector-behind-qol.patch (applied to the local
xxl-inspector checkout) adds a "late load" mode:
  - started from Direct3DCreate9 instead of the entry point (no message box),
  - never chains to itself,
  - subclasses the game window (chaining to other mods' procedures),
  - leaves the projection function to the QoL patch (wide FOV, split screen),
  - ignores window messages until ImGui exists.
The stock behaviour as d3d9.dll is unchanged.

Built DLL: xxl-inspector\bin\apd3d9_xxl1_behind_qol.dll -> copy to
Game Files\apd3d9.dll. F1 toggles the Inspector window.
Build: build_inspector.bat-style cl/link with vcvars32 (VS 2022 Build Tools),
/DXXLVER=1, see the patch's header for the source files.

Co-op additions (2026-10-05):
  - Cheats window: "Asterix torch: Give (lit) / Throw away" buttons, shown when
    xxl_coop.asi is loaded (calls its export XXLCoopTorchCheat).
  - While an Inspector text field has the keyboard (ImGui WantTextInput), key
    messages are kept from the game, and the co-op mod's keyboard player is
    paused (export XXLInspectorWantsKeyboard) -- typing coordinates no longer
    joins player 2 on the keyboard.
