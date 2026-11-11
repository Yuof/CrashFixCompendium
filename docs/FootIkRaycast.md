# FootIkRaycast

With more than 50 characters loaded around you, the game can crash in an animation job. The log says `SkyrimSE.exe+0A8497F` (`call [r10+0x08]`) or `SkyrimSE.exe+05434A0` (`call [r10+0x10]`), with a `hkaFootPlacementIkSolver*` in RDI and `hkbFootIkDriver` on the stack.

## How to reproduce it
I can reproduce the cause on demand: the game frees a block of memory that animation jobs still use. The crash is less predictable, because something has to reuse that memory before a job reads it again.

1. Go where many characters load. The Solitude main gate in the morning gets 39 to 50 of them.
2. Place 15 more NPCs at the player. The 51st character makes the game's foot-IK table grow.

Script: [repro/gatesweep.py](repro/gatesweep.py) with `hours=6,8,10 spawn=15 base=0x1020A4 despawn=1`.

Without the fix, every time the table grows it frees the block the animation jobs are using. With the fix it grows in place and frees nothing.

To see the crash itself I forced it: a test plugin made the table reallocate on every fourth new entry. Four ray casts went through freed memory, and then the game crashed at `+05434A0`.

The gate crowd alone is not enough. It stays between 40 and 50.

## Where I met it
At the Solitude gate with a big city overhaul loaded: the `+0A8497F` form, in the animation job of a townsperson. Site and stack match this mechanism, which makes it the most likely cause.

## Why it crashes
Foot IK keeps characters' feet on the ground. Each character casts rays downwards, and the game caches the results per character in one table, `BGSFootIkRaycastInterfaceDB`. It creates the table once, at startup, with 50 entries.

Every frame, each character's animation job asks the table for its entry. The lookup takes the table's lock, finds or assigns the character's slot, releases the lock and returns a pointer into the table's array. The job keeps that pointer for the rest of its foot-IK pass, and the solver calls through it.

When all slots are taken, the lookup grows the array by 50. Growing past the array's capacity allocates a new block, copies the entries over and frees the old block. Every other job that is mid-pass still holds a pointer into the old block. Its next ray cast reads a vtable out of freed memory and calls through it.

The lock doesn't help. It protects the lookup. The pointer leaves the lock with the caller.

## The code, with the fix in
The fix re-points one call in the table's constructor. The decompiled constructor with that call replaced:

```c
// SkyrimSE+542980 (Address Library 32736). Runs once at startup, before any character exists.
void BGSFootIkRaycastInterfaceDB::Create()
{
    BGSFootIkRaycastInterfaceDB* db = &g_footIkDB;                 // a static object, SkyrimSE+315CB40
    db->vtable = &BGSFootIkRaycastInterfaceDB::vftable;

    ...                                                            // db + 0x18: a hash map, character -> slot
                                                                   // number, sized for 50
    db->slots = {};                                                // db + 0x40: BSTArray<CachedRaycastData>

    FootIkRaycast::ResizeThunk(&db->slots, 50);                    // PATCHED at 542A89, was:
                                                                   // db->slots.resize(50)

    g_footIkDBPointer = db;                                        // SkyrimSE+31B2818: what the lookup reads
}
```

My function (`src/fixes/FootIkRaycast.cpp`):

```cpp
bool ResizeThunk(void* a_array, std::uint32_t a_size)
{
    g_resize(a_array, kReserve);                       // resize(1024): one block, large enough to never move
    const bool result = g_resize(a_array, a_size);     // resize(50): back to 50 entries, the block stays
    ...                                                // log the capacity
    return result;
}
```

The game's resize, unchanged. It shows why the two calls leave a capacity of 1024 with 50 entries in use:

```c
// SkyrimSE+544990 (Address Library 32769). One entry is 0x38 bytes.
bool BSTArray<CachedRaycastData>::resize(BSTArray* array, uint newSize)
{
    ...
    if (newSize == array->size) {
        return true;
    }
    if (array->capacity < newSize) {
        if (!Reallocate(array, newSize, array->size, sizeof(CachedRaycastData))) {    // new block, copy,
            return false;                                                             // free the old one
        }
    }
    else if (newSize < array->size) {
        DestroyEntries(array, newSize, array->size - newSize);     // shrink: the block and capacity stay
        array->size = newSize;
        return true;
    }
    ConstructEntries(array, array->size, newSize - array->size);   // grow inside the capacity: same block
    array->size = newSize;
    return true;
}
```

And the lookup's grow, unchanged. With the fix it stays inside the capacity until 1024:

```c
// SkyrimSE+542F30 (Address Library 32742), under the table's lock
CachedRaycastData* BGSFootIkRaycastInterfaceDB::GetOrAssignSlot(BGSFootIkRaycastInterfaceDB* db, void* character)
{
    ...                                                            // take the lock, look the character up
    if (entry->slot == -1) {                                       // no slot yet
        uint slot = /* the first slot whose flag at +0x2E says "free" */;
        if (/* none */) {
            slot = db->slots.size;
            db->slots.resize(db->slots.size + 50);                 // 5430C2: frees the old block if it
        }                                                          // has to reallocate
        ...                                                        // reset the slot, remember it for the character
    }
    ...                                                            // release the lock
    return &db->slots.data[slot];                                  // the caller keeps this pointer
}
```

Install wants exactly one call to the resize in the constructor, with `mov edx, 50; mov rcx, rbx` in front of it. It also wants the table not to exist yet. Otherwise it patches nothing.

## Why the fix looks like this
I reserve the memory and leave the entry count alone. The array has 50 entries as before, so everything that walks the table costs what it always did. But its block has room for 1024, so growing never moves it, until more than 1024 characters use foot IK at the same moment. Past that point the game behaves as it does without the fix.

It costs 1024 x 0x38 bytes, 56 KB. Nothing per frame.

I do it in the constructor because nothing can hold a pointer into the table that early. The constructor runs well after SKSE loads the plugins, so a patch written at plugin load arrives in time.

If the table already exists when I install, I leave it. Resizing it then could free a block in use, which is the bug.

## What else I considered
The simple version is to give the table 1024 entries once the game's data has loaded. Two things rule it out. With 1024 entries every walk over the table gets up to 20 times longer, and the game walks it about 2,400 times a second. And characters already register at the main menu, so a resize at that point would free a block in use. That is the bug itself.

Reserving at that same point solves the cost and keeps the timing problem.

Keeping the old block alive is the third option: hook the free inside the resize and delay it. That has no upper limit, but jobs with old pointers would then read and write a stale copy of their entry for as long as they hold it.

Changing the `50` in the constructor to a bigger number has the walk cost again.
