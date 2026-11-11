# Changelog

## 1.4.0
- New fix: Audio Format (switch `AudioFormat`, on by default). A sound file in a format the game can't read (a
  32-bit float or ADPCM .wav, an .mp3 or .ogg renamed, a cut-off file) or whose header says 0 channels made the game
  crash in `XAudio2_7.dll+25181` when the sound started. Such a sound now stays silent and the log gives its id.
- New switch: Audio Format Scan (`AudioFormatScan`, off by default). Lists in the log every sound file in Data, loose
  and in .bsa archives, that the game won't play, with the id Audio Format logs.

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
