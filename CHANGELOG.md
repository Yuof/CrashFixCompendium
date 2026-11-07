# Changelog

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
