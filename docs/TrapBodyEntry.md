# TrapBodyEntry

The game closes in a place with traps, some time after a trap there was removed. The crash log says `SkyrimSE.exe+03FF101`, `movups xmm0, [rax+0x30]` with RAX = 0, in `TrapTargetEntry` (Address Library 26034). Below it on the stack is the trap update: `TrapEntry`, `bhkTrapListener`, `bhkWorld` and the `Job_Cell_animations` job.

It needs a trap that is disabled while it touches an actor. That can be a trap with a solid collision body, such as falling rocks or a falling block, or a trap with a phantom, such as a spike bed. A script that moves the trap with `SetPosition` does the same, because the game disables the trap and enables it again at its new place. The crash only comes once the freed memory is used again, so the address alone does not name the trap.

## How to reproduce it
A trap with a solid body:

1. Console: `tgm`.
2. Papyrus: `PlaceAtMe` of `TrapFallingRockSm01` (Skyrim.esm `079922`, a small falling rock) on the player. Its body overlaps the player, so the trap creates an entry with the player as its target.
3. Papyrus: `Disable()` on the rock, then `Delete()`.

A trap with a phantom: the same steps with `TrapWoodSpikeBed01` (Skyrim.esm `073265`, a spike bed).

The steps need Papyrus, for example through a console mod that calls functions. The console's own `disable` leaves the trap's collision in the world for a while, so the entry is not left behind at that moment.

Without the fix the trap's entry stays in the trap listener's list after its collision is freed. With the fix the entry goes at step 3, and `CrashFixCompendium.log` says `[TrapBodyEntry] removed the entry of a trap body that left the world (1 so far)`, or `a trap phantom` for the spike bed. The log line counts at 1, 10, 100 and so on.

The crash itself needs the freed memory to be reused with a 0 where the update reads, so it does not come on demand. A debugger can stand in for the reuse. After step 3 it points the collision of every entry whose collision left the world at a block of zeroed memory, which is what reused memory looks like to the update. Without the fix the next trap update then crashes at `SkyrimSE.exe+03FF101` with RAX = 0, the same stack as in the crash log, every time. With the fix no entry is left, and the game runs on.

Scripts: [repro/trapbody.py](repro/trapbody.py) runs the steps for the rock and the spike bed, and a `SetPosition` variant with a falling block from VIGILANT. [repro/trapbody_force.cdb](repro/trapbody_force.cdb) is the debugger step.

## Where I met it
In a test session that placed VIGILANT's falling blocks and rocks at the player and deleted them again. The crash came later, in the trap update. Every entry of the cell's trap listener pointed at a freed body.

## Why it crashes
A trap hurts whoever touches it. The game tracks this with a trap listener. When a trap's collision touches an actor, the listener creates a trap entry. The entry holds the trap's collision, the trap's reference and a list of targets, the actors that touch it. Every frame, the trap update walks the entries and hits each target.

A trap's collision is a solid body (it collides, like a falling rock) or a phantom (it only detects contact, like the spikes of a spike bed). Both kinds make trap entries the same way. An entry must go when its collision leaves the physics world, because the game can free the collision after that. The physics world calls the listener for every body and every phantom it removes, through two separate callbacks.

For a body, the listener looks up the body's reference: from the body to its 3D node, and from the node to the reference that owns it. Then it removes the entry of that reference. Disable takes the 3D away in two steps:

1. It clears the node's link to its reference.
2. It removes the body from the physics world.

At step 2 the listener finds no reference, so it returns and removes nothing.

For a phantom, the listener removes the phantom from a second list, its trigger list, and stops there. It never looks at the trap entries.

Either way the collision is freed, and the entry still points at it.

`SetPosition` on a trap goes through the game's MoveTo code, which runs the same Disable and then enables the trap at its new place. The trap gets a new body there, and the entry still points at the old one.

The trap update keeps reading the freed collision. While the memory still holds the old collision, the update takes its normal path. Once something else uses the memory, the collision's type byte no longer reads 1 (an entity). The update then takes its other path and reads the motion pointer, which is now 0. RAX = 0 in the crash log is that pointer.

## The code, with the fix in
The listener's callback for a body that leaves the physics world, as the game ships it (decompiled):

```c
// TESTrapListener, its entity listener part at +0x10: SkyrimSE+400AD0 (Address Library 26062)
void EntityRemoved(hkpEntityListener* self, hkpEntity* body)
{
    TESTrapListener* listener = (TESTrapListener*)((char*)self - 0x10);
    uint layer = body->collidable.broadPhaseHandle.collisionFilterInfo & 0x7F;
    if (layer != 14 && layer != 16 && layer != 30) {         // trap, cloud trap, character controller
        return;
    }
    TESObjectREFR* ref = FindCollidableRef(&body->collidable);   // 26003: body -> 3D node -> its reference
    if (ref == nullptr) {
        return;                                              // BUG: after Disable the link is gone, the entry stays
    }
    if (layer == 30) {
        ...                                                  // an actor's controller: drop it from every target list
        return;
    }
    TrapEntry* entry = listener->entries;                    // listener + 0x8
    while (entry && entry->trapHandle != ref->GetHandle()) { // entry + 0x20
        entry = entry->next;                                 // entry + 0x10
    }
    RemoveTrapEntry(listener, entry);                        // 25718: unlink and delete
}
```

The callback for a phantom:

```c
// TESTrapListener, its phantom listener part at +0x28: SkyrimSE+400BA0 (Address Library 26065)
void PhantomRemoved(hkpPhantomListener* self, hkpPhantom* phantom)
{
    TriggerEntry* trigger = self->triggers;                  // listener + 0x30: the trigger list
    if (trigger == nullptr || !IsTriggerLayer(phantom)) {    // bhkCollisionFilter's trigger layer mask
        return;
    }
    while (trigger && trigger->collidable != &phantom->collidable) {   // trigger + 0x18
        trigger = trigger->next;                             // trigger + 0x10
    }
    if (trigger) {
        ...                                                  // unlink it from the trigger list and delete it
    }
}                                                            // BUG: the trap entry (listener + 0x8) stays
```

Disable's order, in `Set3D` when the 3D goes away:

```c
// TESObjectREFR::Set3D(new3D), SkyrimSE+2E47C0 (Address Library 19729)
if (new3D == nullptr) {
    if (BSFadeNode* node = old3D->AsFadeNode()) {
        node->userData = nullptr;                            // 2E4C75: the node forgets its reference
    }
    ...
}
...
FUN_1402E5380(this, nullptr);                                // 2E5191 (19730): calls DetachHavok, or queues it;
                                                             // the physics world removes the body and calls
                                                             // EntityRemoved above
```

Where the freed collision is read:

```c
// TrapTargetEntry, SkyrimSE+3FEF40 (Address Library 26034), for each target of an entry
hkpCollidable* collision = entry->collidable;                // entry + 0x18: still the freed collision
if (collision->broadPhaseHandle.type == 1 && owner(collision)) {   // an entity: the body's path
    ...
} else if (target->flags & 1) {
    position = collision->motion->position;                  // 3FF101: motion (+0x10) is 0 in the reused memory
}
```

The physics world calls both callbacks through the listener's vtables. The fix swaps the two slots (`VTABLE_TESTrapListener[1]` and `[2]`, slot 2 each). The body side:

```c
// hkpWorld's entity-removed dispatch, SkyrimSE+B4D740 (Address Library 61760), called by hkpWorld::removeEntity
for (int i = world->entityListeners.size - 1; i >= 0; --i) {    // world + 0x1B8
    hkpEntityListener* listener = world->entityListeners[i];
    if (listener) {
        listener->EntityRemovedCallback(body);                   // vtable slot 2. PATCHED: for the trap listener this
                                                                 // is EntityRemovedThunk below (was EntityRemoved)
    }
}
```

The phantom side has the same shape:

```c
// hkpWorld's phantom-removed dispatch, SkyrimSE+B4DCC0 (Address Library 61764), called by hkpWorld::removePhantom
for (int i = world->phantomListeners.size - 1; i >= 0; --i) {   // world + 0x1C8
    world->phantomListeners[i]->PhantomRemovedCallback(phantom); // vtable slot 2. PATCHED: PhantomRemovedThunk below
}
```

The functions in the slots now (`src/fixes/TrapBodyEntry.cpp`):

```cpp
void EntityRemovedThunk(void* a_part, RE::hkpEntity* a_entity)
{
    g_entityRemoved(a_part, a_entity);                       // the game's callback first, unchanged
    RemoveEntries(a_part, kEntityListener, &a_entity->collidable, g_bodies, "body"sv);
}

void PhantomRemovedThunk(void* a_part, RE::hkpPhantom* a_phantom)
{
    g_phantomRemoved(a_part, a_phantom);                     // the game's callback first, unchanged
    RemoveEntries(a_part, kPhantomListener, &a_phantom->collidable, g_phantoms, "phantom"sv);
}

void RemoveEntries(void* a_part, std::ptrdiff_t a_partOffset, const RE::hkpCollidable* a_collidable, ...)
{
    const auto layer = a_collidable->GetCollisionLayer();
    if (layer != RE::COL_LAYER::kTrap && layer != RE::COL_LAYER::kCloudTrap) {
        return;                                              // not a trap's collision
    }
    auto* listener = static_cast<std::byte*>(a_part) - a_partOffset;
    if (At<std::uintptr_t>(listener, 0) != RE::VTABLE_TESTrapListener[0].address()) {
        return;                                              // not laid out as Install checked
    }

    // every entry that still points at this collision goes, by the game's own remove function
    for (auto* entry = At<void*>(listener, kEntries); entry;) {
        auto* next = At<void*>(entry, kNext);                // the remove function deletes the entry
        if (At<const RE::hkpCollidable*>(entry, kCollidable) == a_collidable) {
            g_removeEntry(listener, entry);
            ...                                              // the log line, at 1, 10, 100, ... removals
        }
        entry = next;
    }
}
```

## Why the fix looks like this
The entry belongs to the trap's collision, so the fix ties its removal to that collision leaving the physics world. For bodies, that is the moment the game itself removes an entry when it finds the reference. The fix matches the entry by the collision instead of by the reference, and the collision is still valid at that point.

The game's callbacks run first and unchanged. When the game finds the reference and removes the entry itself, the fix's walk finds nothing more to remove. Character controllers (layer 30) and every other layer pass through untouched.

The remove function deletes any entry it is given, also one that is not in the list. The fix only passes entries it found in the list.

The callbacks run on the thread that removes the collision, the same place where the game removes an entry itself. The fix takes no lock of its own.

The cost is one walk of the listener's entry list each time a trap's collision leaves the world. The list holds one entry per trap that touches an actor.

Install checks the instructions that use the listener's offsets:

- the entry list at `+0x8` and the listener at `-0x10` in the body callback, and the next entry at `+0x10`
- the phantom's collision at `+0x20` in the phantom callback
- the collision at `+0x18` in the trap update
- the entry list in the remove function

It also checks that each vtable slot holds the game's callback, or another plugin's hook in front of it, which the fix then calls instead. On a build where one of them differs, nothing is patched. On every call, the fix checks that the listener it computed starts with the trap listener's vtable.

## What else I considered
Clearing the node's link after the body is removed, instead of before, would let the game's own lookup succeed for bodies. Set3D runs for every reference whose 3D goes away, so that order change reaches every removal in the game, not only traps. It also does nothing for phantoms.

Checking the collision in the trap update before each read would catch a freed collision only while its memory is unchanged. After a reuse, no check of the old memory is reliable.
