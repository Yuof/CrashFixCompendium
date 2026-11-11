# The crashes, one page each

Some of these crashes are rare. Rare crashes tend to stay undiagnosed, which is the reason for collecting them here.

## Reading the pages
All addresses are for SkyrimSE.exe 1.6.1170. "Address Library 30112" is the function's id in Address Library for SKSE Plugins.

The code blocks of game functions are Ghidra decompiles with variables renamed, irrelevant parts cut and comments added. For DecalVertexTable and FreeCamera, which change the game's bytes, the decompile is of the patched function. Where a fix re-points a call, the block shows the call going to my function. My own functions are source from `src/fixes/`.

Class and field names follow CommonLibSSE-NG where it has them. Where it has none, the name is my own label for what the code does at that offset, and the comment gives the offset.

The scripts behind the repros are in [repro/](repro/README.md).
