# RagdollWorldLock

The game crashes while you walk around a city or the wilderness, usually with NPCs nearby. The crash log says `SkyrimSE.exe+0B6AA54`, `mov rax, [rdx]` with RDX = 0. Among the objects are an `hkpConstraintInstance` named "NPC COM [COM ]" / "NPC L Thigh [LThg]" and an `hkbRagdollDriver`. The stack runs through `SkyrimSE.exe+0B47887` and `+0AAD7BF` on a job thread, often with `Precision.dll` below them.

The crash is rare and needs no particular mod. Precision shows up in most logs because it puts ragdolls into the physics world more often.

The faulting read is a null pointer: the ragdoll driver uses a joint that is not in the physics world yet. The most likely reason the joint is not there is a race on a counter of the physics world, which two game functions change without taking the world's lock. This fix makes them take it.

## How to reproduce it
The crash needs that counter to be wrong, and a normal game gets there by chance only. A test plugin forces it.

1. Install the test plugin from [repro/ragdoll_force.cpp](repro/ragdoll_force.cpp).
2. `coc` to an exterior, draw a sword, switch to third person and hit an NPC a few times. With Precision, each hit gives the NPC an active ragdoll.

The game crashes at `SkyrimSE.exe+0B6AA54` with RDX = 0 every time. Script: [repro/ragfight.py](repro/ragfight.py).

The test plugin does three things: it removes one ragdoll joint from the physics world, it makes the driver's re-add wait in the queue as a wrong counter would, and it makes the driver's next call for that joint. Because it creates the wrong state itself, it crashes with the fix on too. The fix keeps the counter from going wrong. It does not change what a wrong counter leads to.

Two more test plugins show the fix at work. [repro/counter_watch.cpp](repro/counter_watch.cpp) puts a write watchpoint on the counter and records every write from a thread that does not own the world lock. Without the fix it records such writes from both functions. With the fix, every one of those writes comes from a thread that owns the lock. [repro/world_busy.cpp](repro/world_busy.cpp) holds the lock from another thread at the moment of such a write. The fix then queues the update, and the update has run 200 ms later.

## Where I met it
A player reported it on the mod page with crash logs from three load orders, one of them without Precision. All three have the signature above and the same leftover return address on the stack, `SkyrimSE.exe+0B38C7D`. That address is in `hkpWorld::addConstraint`, right after the line that queues an add.

## Why it crashes
An NPC's ragdoll is a set of rigid bodies held together by joints. While an NPC is animated, the ragdoll driver makes the ragdoll follow the animation. It runs inside the physics update and does two things per joint. If the joint is not in the physics world, it adds it. Then it switches the joint on or off: a joint between two body parts that follow the animation exactly gets switched off.

The physics world keeps a counter of changes in progress. While the counter is not 0, the world does not perform a new change. It queues the change and runs it once the counter is back at 0. Adding a joint follows that rule: with a counter other than 0 the add is queued, and the joint has no owner yet.

The driver does not check whether its add went through. It goes on to the switch, and the switch reads the joint's owner. For a queued joint the owner is null, and that read is the crash.

So the crash needs a counter other than 0 at the moment the driver adds a joint. At that point of the physics update it should be 0. Two functions can leave it wrong:

- the update after a body moves. The main thread reaches it when a cell attaches and the game applies a ragdoll's saved pose.
- the update after a phantom moves. The main thread reaches it when the loaded area's bound moves.

Both check the counter, raise it, do their work and lower it again. That sequence is only safe under the world's lock, and on these two paths the main thread does not hold it. When one of them runs while the physics update changes the counter on its worker thread, one write can be lost. The counter then stays at 1 or drops to -1, and from then on the world queues every change. All three logs show the queued add, which matches this explanation.

Havok Solver Crash Fix repairs the same counter after the fact. At the start of each physics update it resets a counter below 0. A counter stuck above 0 it resets only after about ten seconds, because a busy world also has a counter above 0.

## The code, with the fix in
The driver's joint loop, where the crash starts (decompiled):

```c
// hkbRagdollDriver::DriveToPose, SkyrimSE+AACBE0 (Address Library 58291)
for (int i = 0; i < ragdoll->numJoints; ++i) {
    hkpConstraintInstance* joint = ragdoll->joints[i];
    if (joint->owner == nullptr) {                               // joint + 0x10: not in a world
        hkpWorld::addConstraint(world, joint);                   // may only queue the add
    }                                                            // BUG: the driver does not check
    hkpEntity* a = joint->entities[0];
    hkpEntity* b = joint->entities[1];
    if (IsKeyframedOrFixed(a) && IsKeyframedOrFixed(b)) {        // both follow the animation exactly
        if (IsEnabled(joint)) {
            hkpConstraintInstance::SetEnabled(joint, false);     // reads joint->owner
        }
    } else {
        ...                                                      // may switch the joint on instead
    }
}
```

The add, which queues while the counter is not 0:

```c
// hkpWorld::addConstraint, SkyrimSE+B38C50 (Address Library 61308)
hkpConstraintInstance* hkpWorld::addConstraint(hkpWorld* world, hkpConstraintInstance* joint)
{
    if (world->criticalOperationsLockCount != 0) {               // world + 0xF8: changes in progress
        QueueOperation(world, AddConstraint, joint);
        return nullptr;                                          // joint->owner stays null
    }
    ...
    return AddConstraintImmediately(world, joint);               // sets joint->owner
}
```

The switch, and the read that faults:

```c
// SkyrimSE+B47870 (Address Library 61581)
void hkpConstraintInstance::SetEnabled(hkpConstraintInstance* joint, bool enable)
{
    if (enable) EnableConstraint(joint, joint->owner);
    else        DisableConstraint(joint, joint->owner);
}

// SkyrimSE+B6AA50 (Address Library 62288)
void DisableConstraint(hkpConstraintInstance* joint, hkpConstraintOwner* owner)
{
    owner->vfunc_0x30();                                         // B6AA54: mov rax, [rdx] with owner = null
    ...
}
```

None of that is patched. The patch is in the two functions that write the counter without the lock. The first:

```c
// hkpEntity::updateBroadPhaseAfterMove, SkyrimSE+B30FE0 (Address Library 60918)
// PATCHED: the first bytes jump to UpdateAfterMoveThunk, which runs this code under the world lock or queues it
hkpWorld* world = body->world;
if (world->criticalOperationsLockCount != 0) {
    QueueOperation(world, /* operation 0x17 */ body);            // a busy world: queue and return
    return;
}
world->criticalOperationsLockCount = 1;                          // raise: a plain store of 1
...                                                              // the broadphase update
world->criticalOperationsLockCount -= 1;                         // lower
```

The second has the same shape:

```c
// hkpPhantom::updateBroadPhase, SkyrimSE+B52FD0 (Address Library 61827)
// PATCHED: the first bytes jump to PhantomUpdateThunk
hkpWorld* world = phantom->world;
if (world->criticalOperationsLockCount + world->criticalOperationsLockCountForPhantoms != 0) {
    QueueOperation(world, /* operation 0x12 */ phantom, aabb);
    return;
}
world->criticalOperationsLockCount += 1;                         // a plain inc, no lock prefix
...
world->criticalOperationsLockCount -= 1;
```

Where the game calls the same pose function on purpose, it takes the world's write lock first (`SkyrimSE+EB0000`). The two paths above skip that.

My functions (`src/fixes/RagdollWorldLock.cpp`):

```cpp
// Runs a_now under the world's write lock when this thread owns it or can take it at once.
// Otherwise it queues a_op, the way the two functions do themselves for a busy world.
template <class Now>
void WithWorldLock(RE::hkpWorld* a_world, const Operation& a_op, bool a_fromRunQueue, Now a_now)
{
    auto* bhk = a_world ? static_cast<RE::ahkpWorld*>(a_world)->userData : nullptr;
    if (!bhk) {
        a_now();
        return;
    }
    auto&      lock = reinterpret_cast<LockState&>(bhk->worldLock);
    const auto me = GetCurrentThreadId();

    if (lock.writerThread == me) {                               // the physics update, or a caller that locked
        a_now();
        return;
    }
    if (_InterlockedCompareExchange(&lock.state, kOneWriter, 0) == 0) {   // the lock is free: take it
        lock.writerThread = me;
        a_now();
        bhk->worldLock.UnlockForWrite();
        return;
    }
    if (a_fromRunQueue) {                                        // the world's queue runner: see below
        a_now();
        return;
    }
    queue(a_world, &a_op);                                       // another thread holds the lock: queue
}

void UpdateAfterMoveThunk(RE::hkpEntity* a_entity)
{
    const Operation op{ .type = kOpUpdateAfterMove, .object = a_entity };
    const bool fromRunQueue = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == g_fromRunQueue[0];
    WithWorldLock(a_entity->world, op, fromRunQueue, [&] { g_updateAfterMove(a_entity); });
}
```

`PhantomUpdateThunk` is the same for the phantom's function.

Install checks the first bytes of both functions and finds the queue runner's call to each. If either differs, it patches nothing.

## Why the fix looks like this
The fix covers the part that writes the counter and nothing else: the check, the raise, the work and the lowering. Every caller goes through the patched functions. The physics update and the game's own locked callers already own the lock, so for them nothing changes.

It never waits for the lock. The main thread reaches the body's update while it holds a lock on the NPC's animation graph. The physics update takes that same kind of lock while it holds the world lock. If the main thread waited for the world lock there, each thread could end up waiting for the other.

So when another thread holds the lock, the fix does what the function already does for a busy world: it queues the update. The world runs it as soon as the counter is back at 0.

While the fix holds the lock, the function may work off the world's queue on its way out. It does the same when the game's own callers hold the lock around it.

One caller keeps the old behaviour. The world's queue runner calls these two functions to work off the queue. If it does so while another thread holds the lock, queueing again would hand it the same update at once, in a loop. Install finds the runner's two calls, and the fix recognises them by their return address.

## What else I considered
Waiting for the world lock is the simplest version, and it can deadlock for the reason above. Taking the lock higher up, around the whole pose loop or the grid update, has the same problem and holds the lock longer.

A lock with a timeout that falls back to the unlocked call brings the race back exactly when the lock is busy.

Writing the counter atomically on the main thread's side does not help. The game's own raises and lowers are plain read-modify-writes, so they can still overwrite an atomic write.

A null check on the joint's owner, in the driver or in the switch, would stop this one crash and hide the wrong counter. The world would still queue every later change, and other code that meets a queued body or joint fails elsewhere. Precision 2.0.6 has such a place: `Precision.dll+004DCEB` reads a body's world without a check.

Resetting the counter, as Havok Solver Crash Fix does below 0, cannot tell a stuck counter from a busy world at the moment of the crash.
