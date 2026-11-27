# Changelog

## 1.8.0
- New fix: Trap Body Entry (switch `TrapBodyEntry`, on by default). A crash at `SkyrimSE.exe+03FF101` (1.6.1170)
  in the trap update, with `movups xmm0, [rax+0x30]` and RAX = 0. A trap was disabled or moved by a script while it
  touched an actor. For a solid body (falling rocks) the game removes the trap's entry only when it still finds the
  body's reference, and Disable clears that link first. For a phantom (spike beds) it never removes it. The fix
  removes the entry when the trap's collision leaves the physics world.

## 1.7.0
- New fix: Path Follower Race (switch `PathFollowerRace`, on by default). A crash at `SkyrimSE.exe+11BCBB0`
  (1.6.1170) during a fast travel, with `mov r8d, [rcx+0x80]` and RCX = 0. One thread cleared a far-away actor's
  path while another thread moved the actor along it. The game's 1.7.104 build fixes this itself: the path follower's
  update takes the lock that the path's writers take. This fix does the same on the builds before 1.7.104, and also adds
  flying actors.

## 1.6.2
- Reverted some changes.

## 1.6.1
- UI Message Queue: no longer crashes the game before the main menu with MiniMap Menu (Nexus 149508)
  MiniMap Menu's hook resumes the game's code 5 bytes into `AddMessage`, which was the middle of this fix's 18-byte jump.
  The jump is now 5 bytes. MiniMap Menu's hook still overrides my code (and all other mods hooking this) here,
  so this fix won't run with MiniMap Menu enabled.
- Disabled Attacker, Nif String Index, Shout Empty Word, Ragdoll World Lock and UI Message Queue hook their functions
  with SafetyHook. The jump at each function's entry is now 5 bytes, so a plugin that hooks the same function later
  the usual way no longer crashes the game.

## 1.6.0
- New fix: SKSE Stack Lookup (switch `SkseStackLookup`, on by default). A crash while saving at
  `skse64_1_6_1170.dll+5D51F` (SKSE 2.2.8) with `rax = 0x80000003`, or a mod event that never arrives. Before a save,
  SKSE checks that each script holding a ModEvent, UICallback or SpawnerTask handle is still running. That check
  reads the script's stack with Skyrim LE's layout. The fix makes it check only that the stack exists. An SKSE
  without that check is left alone.

## 1.5.0
- New fix: Ragdoll World Lock (switch `RagdollWorldLock`, on by default). A crash at `SkyrimSE.exe+0B6AA54`
  (1.6.1170) with "NPC COM" / "NPC L Thigh" and an hkbRagdollDriver in the crash log. The game switched a ragdoll
  joint on or off before the joint was in the physics world. The most likely cause is a counter in the physics
  world that two game functions change without taking the world's lock: one for the loaded area's bound, one for a
  ragdoll's saved pose. A lost write leaves the counter wrong, and from then on the world queues every change. Both
  functions now take the lock when it is free and queue their change when another thread holds it. They never wait
  for it.

## 1.4.0
- New fix: Audio Format (switch `AudioFormat`, on by default). A sound file in a format the game can't read (a
  32-bit float or ADPCM .wav, an .mp3 or .ogg renamed, a cut-off file) or whose header says 0 channels made the game
  crash in `XAudio2_7.dll+25181` when the sound started. Such a sound now stays silent and the log gives its id.
- New switch: Audio Format Scan (`AudioFormatScan`, off by default). Lists in the log every sound file in Data, loose
  and in uncompressed .bsa archives, that the game won't play, with the id Audio Format logs.

## 1.3.0
- New fix: Shout Empty Word (switch `ShoutEmptyWord`, on by default). Pressing the unlock key in the Magic menu on a
  mod's shout that has fewer than three words made the game crash at `SkyrimSE.exe+093BA03` (1.6.1170).
- New fix: Nif String Index (switch `NifStringIndex`, on by default). A mesh in which a block names a string past
  the end of the file's string list made the game crash while the mesh loads. The block now gets an empty string
  and the log shows the file.

## 1.2.0
- New fix: Foot IK Raycast (switch `FootIkRaycast`, on by default). As soon as more than 50 characters were loaded
  around you (a busy city gate, a crowd), the game moved its shared foot-placement table in memory while other
  characters' animations were still reading the old one: a crash at `SkyrimSE.exe+0A8497F` or `+05434A0` (1.6.1170).

## 1.1.0
- New fix: Decal Vertex Table (switch `DecalVertexTable`, on by default). A mesh with leftover, unused vertices (e.g.
  Capital Whiterun Expansion's scaffolding walkway) plus a decal landing on it (a footprint, a fire scorch mark) made
  the game write into memory that belongs to something else. The crash then came later, anywhere, blaming unrelated
  game code - or right away at `SkyrimSE.exe+0E53F5D` (1.6.1170).

## 1.0.0
- First release: nine fixes in one DLL, each with its own switch - `StartupHook`, `PathIdle`, `IsInCombat`,
  `UIMessageQueue`, `SkinnedDecal`, `FreeCamera`, `DisabledAttacker`, `ShadowMapStackAlign`, `TrespassPursue`.
