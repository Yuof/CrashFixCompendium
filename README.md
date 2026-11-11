# Crash Fix Compendium

SKSE plugin for Skyrim Special Edition (SE 1.5.97 and Anniversary Edition runtimes): small fixes for rare crashes in one
DLL, each with its own on/off switch. VR is not supported.

| Key | Crash |
|---|---|
| `StartupHook` | An SKSE mod installs a hook while the start-up loading screen runs (`EXCEPTION_BREAKPOINT`) |
| `PathIdle` | An NPC saved while waiting to play a start, stop or turn animation crashes the save on load |
| `IsInCombat` | IsInCombat asked about an object during a console command |
| `UIMessageQueue` | Two threads queue a menu message at once and release its name twice |
| `SkinnedDecal` | Two hits give one tree or actor its first decals at the same moment |
| `FreeCamera` | The player's combat group updated on two threads while the free camera is on |
| `DisabledAttacker` | Precision 2.0.6: an attacker disabled or removed in the middle of a swing |
| `ShadowMapStackAlign` | Engine Fixes 7.0.15 to the 7.0.21 beta and alandtse's fork 7.13.0: its ShadowMap fix runs with a misaligned stack |
| `TrespassPursue` | A house or shop owner starts escorting someone out and the escort reads a setting the game never set. The fix makes the owner follow someone he lost sight of until they leave, instead of giving up at random |
| `DecalVertexTable` | A footprint, blood or hit mark lands on a mesh with unused vertices, and the game makes its table for the mesh's vertices too small |
| `FootIkRaycast` | More than 50 characters are loaded nearby, and the game moves its foot-IK table to a bigger block while other characters' animation still uses the old one |
| `ShoutEmptyWord` | The unlock key in the Magic menu on a shout with fewer than three words: the game steps into an empty word slot |
| `NifStringIndex` | A mesh in which a block names a string past the end of the file's string list: the game reads whatever follows the list as the string |
| `AudioFormat` | A sound file the game can't read (32-bit float, ADPCM, an .mp3 renamed) or whose header says 0 channels: XAudio2 gets a voice with 0 channels (`XAudio2_7.dll+25181`) |

`AudioFormatScan` (off by default, in the INIs' `[Reports]` section) is not a fix: it lists every sound file in Data,
loose and in uncompressed .bsa archives, that the game won't play, so the file behind an `AudioFormat` line can be
found.

Each fix's own comment in `src/fixes/<Key>.cpp` explains the bug and the patch. How to reproduce each crash, its
cause in the game's code and why the fix is what it is: [docs/](docs/README.md). What changed in each version:
[CHANGELOG.md](CHANGELOG.md).

## Switches
`SKSE/Plugins/CrashFixCompendium.ini` ships with the mod and holds the defaults; an update replaces it.
`CrashFixCompendium_Custom.ini` beside it is the player's: the plugin generates it on the first start with every line
commented out, adds a commented line for each new fix, and never changes anything else. An active line overrides the
shipped file. `CrashFixCompendium.log` has one status line per fix.

## Adding a fix
One `src/fixes/<Key>.cpp` ending with its `Register` call (its key, the old standalone DLL, its `Install`, and the
trampoline bytes it allocates: `SKSE::Init` reserves their sum once), and one line in the shipped INI. The build finds
the new file by itself. `ctest --preset release` then runs `Tools/check_source.py`, which checks the fix files and the
INI against each other.

## Requirements
- [SKSE64](https://skse.silverlock.org/)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)

## Building
Visual Studio 2022 or newer (C++; the preset uses the newest one installed), CMake 3.24+,
[vcpkg](https://github.com/microsoft/vcpkg) with `VCPKG_ROOT` set.

```
git clone --recurse-submodules https://github.com/Yuof/CrashFixCompendium
cd CrashFixCompendium
cmake --preset release
cmake --build --preset release
```

The DLL is `build/Release/CrashFixCompendium.dll`. `ctest --preset release` runs the offline tests of the switches and
the code helpers, and the checks of the source.

## License
[GPL-3.0-or-later](LICENSE), as required by [CommonLibSSE NG](https://github.com/alandtse/CommonLibVR) (GPL-3.0-or-later
with its modding exceptions), which this plugin links statically.
