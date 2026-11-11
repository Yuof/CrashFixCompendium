# TrespassPursue

You walk into someone's house while they are home, the owner starts to escort you out, and the game crashes. Two forms, both on a job thread, both with `BGSProcedurePursue` on the stack (`SkyrimSE.exe+0475A23`) and a `BGSPackageDataBool` among the objects:

- `SkyrimSE.exe+0454CEF`, `call [r8+0x08]`, reading a small or invalid address
- an access violation executing an invalid address such as 0x2, with `SkyrimSE.exe+0454CF3` next on the stack

**This fix changes how the escort behaves.** Read "Why the fix looks like this" before you rely on it.

## How to reproduce it
1. Enter an occupied house at a time you trespass there. I use `coc RiftenHouseofClanSnowShod` at 13:00. Each owner at home builds one or two escort packages.
2. Leave, come back, and repeat eight times.

Script: [repro/trespass.py](repro/trespass.py).

Whether it crashes is a matter of chance. It depends on what the memory allocator left in one slot the game forgets to fill, and about 6% of the packages get the crashing value. A few runs of the script are usually enough for a crash.

That rate is too low for an A/B, so I force the value. A test plugin writes it into the slot right inside the game's builder ([repro/trespass_force.cpp](repro/trespass_force.cpp)). Forced, the game crashes every time without the fix and never with it. With the fix on, each launch logs `parameter 2 set to no data`.

The crash doesn't depend on Stealth Fixes, which hooks the same code. It does seem to depend on the allocator: with the game's own allocator instead of Engine Fixes' one, the slot never held the crashing value. Engine Fixes' allocator reuses recently freed blocks without clearing them.

## Where I met it
Entering Snow-Shod Manor at 13:00: the second form, executing 0x2, with an owner's package among the objects. Any script or mod that puts the player into an occupied house can do the same, and so can a player who walks in.

## Why it crashes
The "you need to leave" escort is a package the game builds in code. It has one procedure, Pursue, and three pieces of data: the trespasser (uid 0), the trespasser's cell (uid 1) and a Bool (uid 2).

Pursue takes four parameters:

| Parameter | Name | The builder gives it |
|---|---|---|
| 0 | Target | uid 0, the trespasser |
| 1 | WhileWithinLocation | uid 1, the cell |
| 2 | UntilWithinLocation | nothing |
| 3 | WarnToLeave | uid 2, the Bool |

The builder never sets parameter 2. Setting parameter 3 grows the parameter array from two slots to four, and the grow doesn't clear the new slots. Parameter 2 keeps whatever bytes the allocator handed over. What happens next depends on its low byte:

- 0 or 1: Pursue reads the trespasser or the cell as the "until" location. Both work as locations.
- 3 or more: no data has that uid. The game handles that.
- 2: the Bool. Pursue treats it as a location, steps back eight bytes to reach an interface the Bool doesn't have, and calls through whatever lies there.

The dump of one crash shows the four slots ending in 00, 01, 02, 02.

## The code, with the fix in
The fix re-points the three calls to the builder. Each now goes to my function, which calls the builder and then sets parameter 2. One of the callers, decompiled with the call replaced:

```c
// SkyrimSE+66E960 (Address Library 37414): give an owner the escort package
void Actor::InitiateTrespassPursuePackage(Actor* owner, TESObjectREFR* trespasser, BGSPackageDataLocation* where)
{
    TESPackage* package = CreatePackage(kPackage);
    ...                                                // copy combat style, flags and owner quest from the
                                                       // owner's current package

    TrespassPursue::BuildTreeThunk(package->GetCustomData());      // PATCHED at 66EA25, was:
                                                                   // customData->BuildTrespassPursueTree()
    package->GetCustomData()->BuildTrespassPursueData(trespasser, where);
    package->procedureType = 0x31;
    owner->PutCreatedPackage(package, true, true);
    ...
}
```

The builder, unchanged. The missing assignment is visible:

```c
// SkyrimSE+498050 (Address Library 29666)
void TESCustomPackageData::BuildTrespassPursueTree(TESCustomPackageData* data)
{
    BGSProcedurePursue*        pursue = CreateProcedure("Pursue");
    BGSProcedureTreeProcedure* item = CreateTreeItem("Procedure");
    data->templateParent = nullptr;
    data->version = 0;
    BGSProcedureTreeProcedure::sub(item);              // Ghidra's name. The item holds the procedure at + 0x10.
    data->tree = item;                                 // data + 0x20
    pursue->vfunc_0x108(1);

    pursue->uids.ConvertDataPointersToUids(&data->dataList);
    pursue->uids.SetUidAt(0, 0);                       // Target              = the trespasser
    pursue->uids.SetUidAt(1, 1);                       // WhileWithinLocation = the cell
    pursue->uids.SetUidAt(3, 2);                       // WarnToLeave         = the Bool. Grows the array to 4.
                                                       // BUG: nobody sets slot 2, UntilWithinLocation
}
```

My function (`src/fixes/TrespassPursue.cpp`), shortened:

```cpp
void BuildTreeThunk(void* a_data)
{
    g_target[I](a_data);                               // the game's builder
    FixParam2(a_data);
}

void FixParam2(void* a_data)
{
    auto* item = /* a_data + 0x20 */;                  // the tree item
    auto* proc = /* item + 0x10 */;                    // its procedure
    if (!proc || /* vtable */ != RE::VTABLE_BGSProcedurePursue[0].address()) {
        return;                                        // only a Pursue
    }
    auto* slots = /* proc + 0x08 */;                   // the uid array: 8 bytes a slot, uid in the low byte
    if (!slots || /* proc + 0x18, the array's size */ != 4) {
        return;                                        // only the 4-slot shape the builder leaves
    }
    slots[2 * 8] = kNoDataUid;                         // 0xFF: no data in the package has this uid
    ...                                                // log, and count how often the slot held the Bool's uid
}
```

And the place that used to crash, unchanged. Follow uid 2 and uid 0xFF through it:

```c
// SkyrimSE+454BC0 (Address Library 28181)
void BGSProcedureBase::ResolveLocationParam(BGSProcedureBase* proc, Context* ctx, uint param, BGSAIWorldLocation** out)
{
    ...                                                // release the old *out
    *out = nullptr;

    uint64_t* slot = &proc->uids.data[param];
    IPackageData* data;
    if (!proc->resolved) {                             // proc + 0x20
        data = FindByUid(ctx->dataList, (uint8_t)*slot);            // uid 2 finds the Bool. uid 0xFF finds nothing.
    } else {
        data = (IPackageData*)*slot;
    }

    // Location data keeps this interface 8 bytes into the object, so the code steps back by 8.
    // For the Bool that lands on unrelated memory.
    auto* location = data ? (BGSPackageDataLocation*)((char*)data - 8) : nullptr;

    if (location != nullptr) {
        *out = location->vfunc_1(ctx);                 // 454CEF: on the Bool, a call through an invalid pointer
    }
}
```

Install looks for each call by its target. If a caller doesn't have exactly one call to the builder, I leave that caller alone and say so in the log. The three callers: the function above, a second function that contains a copy of it (`SkyrimSE+695D30`), and the one that rebuilds the tree when a save loads (`SkyrimSE+7A40B0`).

## Why the fix looks like this
I fix the value where the game makes it. Guarding the crash site would only stop this one crash.

Nothing in the package is meant for parameter 2, so I give it the value the game itself uses for "no data", 0xFF. The lookup compares one byte against the package's uids 0, 1 and 2, finds nothing and returns null, and the code above handles null.

I patch all three callers because every trespass package comes out of this one builder, including the ones rebuilt from a save.

**The behaviour change.** In the unpatched game, parameter 2 is random: usually the trespasser or the cell, now and then nothing, now and then the crash. With the fix it is always nothing. What you notice:

- An owner who can see you behaves as before. He watches you and tells you to go.
- An owner who loses sight of you now follows you, to about 150 units, until you leave the cell. With the leftover values 0 or 1 he gave up at once and often started again a second or two later.

The warning and the alarm still come.

This is a deliberate choice. The unpatched behaviour is already random, and "no data" is the one value that matches what the package's parts describe.

## What else I considered
A null guard at the crash site stops the crash and leaves the wrong value in place.

The conservative fix changes only the crashing value: leave 0 and 1 alone and turn a 2 into 1, the cell. That keeps the game exactly as it is, apart from the crash. It also keeps the give-up-and-restart loop, which is a bug of its own with the same cause, so I fixed both.
