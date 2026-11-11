# NifStringIndex

The game crashes while a mesh loads, or a few seconds later. The crash log says `SkyrimSE.exe+0CEC826` or `+0CEC83E`, inside the copy of a `BSFixedString`, with `NiObjectNET::LoadBinary` on the stack.

The mesh is broken: one of its blocks asks for a string the file doesn't have.

## How to reproduce it
You need a .nif with a string index at or past its string count. I make one from a good mesh:

1. `py mknif.py <mesh.nif> <out dir>` ([repro/mknif.py](repro/mknif.py)). It rewrites the name index of the file's first block and writes two copies: `_edge` (index = the string count, one past the end) and `_far` (index = 0x10000000).
2. Put a copy in place of the original mesh and start the game.
3. Load an object that uses the mesh. I place Barenziah's crown at the player ([repro/nifstring.py](repro/nifstring.py)).

Both copies crash the game every time without the fix, and never with it. The `_far` copy crashes at once. The `_edge` copy sometimes lasts about 20 seconds after the crown appears and then crashes at `+0CEC83E`: the bad string sits in the loaded object, and the crash comes when something uses it.

With the fix on, the log names the file:

```
[NifStringIndex] "data\MESHES\clutter\quest\barenziahscrowncomplete.nif": a block asks for string 13 of 13,
read as an empty string (1 so far)
```

## Where I met it
A player reported it on the mod page after debugging it themselves: a crash "relating to nif string indexes being longer than what is actually in the file, the suspect file had 2 strings while the name of a block was asking for 3". They didn't say which mod ships that mesh. My own game has no such mesh, so all my crashes come from the ones I modified for the test.

## Why it crashes
A .nif stores each string once, in a list in its header. Blocks refer to strings by number. When the game loads a block it reads that number and copies the string out of the list.

The reader knows one special number: 0xFFFFFFFF means "no string". It uses every other number as an index and never compares it with the size of the list. A number past the end makes it read whatever memory follows the list as a string. Usually that is not a valid string, and the string's reference count faults when the game touches it. If it happens to be a valid string, the block silently gets a wrong name.

Nineteen functions call the reader, among them the loaders of nodes, geometry, extra data and animation sequences. Any block type that stores a string can trigger it.

## The code, with the fix in
The fix replaces the reader. Its first 16 bytes become a jump to my function, so none of the game's reader runs any more. Here is the reader as the game ships it (decompiled):

```c
// NiStream, SkyrimSE+D1FF90 (Address Library 70352)
void NiStream::ReadString(NiStream* stream, BSFixedString* out)
{
    NiBinaryStream* file = stream->iStr;               // stream + 0x298
    uint32_t        index;
    file->position += file->Read(&index, 4);

    if (index == 0xFFFFFFFF) {                         // the only check
        BSFixedString empty;
        *out = empty;
        return;
    }
    *out = stream->fixedStrings.data[index];           // stream + 0x280. BUG: index can be anything
}
```

And what runs in its place (`src/fixes/NifStringIndex.cpp`):

```cpp
void ReadString(RE::NiStream* a_stream, RE::BSFixedString* a_out)
{
    std::uint32_t index = kNoString;                   // 0xFFFFFFFF
    a_stream->iStr->read(&index, 1);

    const auto& table = a_stream->fixedStrings;
    if (index < table.capacity()) {                    // new: the index must be inside the list
        *a_out = table.begin()[index];
        return;
    }

    *a_out = RE::BSFixedString();                      // past the end reads like "no string"
    if (index == kNoString) {
        return;
    }
    if (const auto n = ++g_badIndices; Milestone(n)) { // log the 1st, 10th, 100th ... bad index
        logger::warn(
            "[NifStringIndex] \"{}\": a block asks for string {} of {}, read as an empty string ({} so far)"sv,
            a_stream->inputFilePath, index, table.capacity(), n);
    }
}
```

## Why the fix looks like this
I wrote the reader again with the one comparison it lacks. Fixing it there covers all nineteen callers.

A bad index gives the block an empty string. That is what the file format's own "no string" gives, so every caller already handles it.

The log names the file, so you can find the mesh and tell its author.

I compare against the list's allocated size, which keeps the read inside the list's memory.

Install compares the reader's first 81 bytes with what I expect and leaves the game alone if they differ.

## What else I considered
Refusing the whole mesh is the stricter option. It removes an object from the game over one bad name, where an empty name leaves the object usable.

Guards in each of the nineteen callers would do the same job nineteen times.

A log line without the file name would tell you a mesh is broken and not which one. The file name is what makes the line useful.