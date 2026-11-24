# PathFollowerRace

The game crashes during a fast travel. The crash log says `SkyrimSE.exe+11BCBB0`, `mov r8d, [rcx+0x80]` with RCX = 0. The stack runs through `MovementAgentPathFollowerVirtual`, `ProcessLists::UpdateLowActorBatch` and `ProcessLists::UpdateNonHighActors`, usually below `PlayerCharacter::ExecuteCellTransition`. Among the objects are characters far from the player.

The crash is rare and needs no particular mod. Two of the game's own threads use one actor's path at the same time.

Bethesda fixed this crash in 1.7.104, and I copied that fix. In 1.7.104 the function that reads the path takes the lock that protects it. This fix makes the older builds do the same. It also covers flying actors, whose version of that function is still unlocked in 1.7.104.

## How to reproduce it
For the crash, a second thread has to clear an actor's path within a few microseconds of a certain read. In a normal game that is a matter of chance, so I force it with a test plugin.

1. Install the test plugin from [repro/path_follower_force.cpp](repro/path_follower_force.cpp).
2. Load a save and `coc Whiterun`.
3. Open the map and fast travel to Riverwood.

Without the fix the game crashes at `SkyrimSE.exe+11BCBB0` with RCX = 0 every time, with the fix never. Script: [repro/fasttravel.py](repro/fasttravel.py).

The test plugin waits until the update of a far-away actor has read the actor's path once. At that point it starts a thread that calls the game's own ClearPath for this actor, and it holds the update for 30 ms before the second read. Without the fix, ClearPath runs at once and the second read gets null. With the fix, ClearPath has to wait for the lock until the update is done, and the game runs on.

## Where I met it
Once, during a fast travel to Riverwood. The actor was a mammoth that roams with a second mammoth and a giant, far from the player.

## Why it crashes
An actor that walks somewhere follows a path, which is a list of points. The object that moves the actor along it is the actor's path follower. The follower owns the path and has a lock for it.

There are three kinds of follower. The standard one moves actors near the player, with physics. The virtual one is for actors far away and only steps them from point to point. The flight one is for flying actors.

Three functions of a follower write the path: SetPath, ClearPath and SetPathFollowing. All of them take the lock first. ClearPath releases the path and sets the follower's pointer to null.

The function that moves the actor is the follower's update. In the virtual follower it reads the path pointer again for every use, eight times in all, and it never takes the lock.

That only matters once two threads work on the same actor, and during a fast travel they do. Far-away actors are updated as a batch. When game time passes in one go, the main thread and three job threads split that batch between them, one actor per thread at a time.

Updating one actor can write another actor's path, though. When time passes, an actor with a Patrol package is teleported along its patrol route, and it takes its group with it. The teleport stops each member's movement, which ends in the member's ClearPath. All of that runs on the thread that updates the leader, while another thread may be in the middle of the member's own update.

If ClearPath lands between two of the update's reads, the later read gets null. That is the RCX = 0 in the log: the faulting instruction reads the point count at +0x80 of a null path.

A SetPath at the same moment doesn't crash at this address, but it is no better. It releases the old path while the update may still be using it.

The flight follower's update skips the lock in the same way. The standard follower's update doesn't: it takes the lock at its start and releases it at its end.

## The code, with the fix in
The virtual follower's update in 1.6.1170:

```c
// MovementAgentPathFollowerVirtual::PlannerUpdate, SkyrimSE+120D2F0 (Address Library 92541)
// PATCHED: the first bytes jump to VirtualUpdate, which runs this function under the follower's lock
void PlannerUpdate(IMovementPlannerAgent* planner, float* dt)
{
    MovementAgentPathFollowerVirtual* follower = planner - 0x18;
    int result = -1;                                              // BUG: no lock from here to the end
    do {
        if (follower->state != 1) return;                         // follower + 0x40: 1 = following a path
        result = FollowPath(follower, dt);
    } while (result == 1);
}
```

The same function in 1.7.104. The lock is the only difference:

```c
// MovementAgentPathFollowerVirtual::PlannerUpdate in 1.7.104 (Address Library 92541)
void PlannerUpdate(IMovementPlannerAgent* planner, float* dt)
{
    MovementAgentPathFollowerVirtual* follower = planner - 0x18;
    BSWriteLockGuard guard(follower->lock);                       // follower + 0x38: LockForWrite, Bethesda's fix
    int result = -1;
    do {
        if (follower->state != 1) break;
        result = FollowPath(follower, dt);
    } while (result == 1);
}                                                                 // UnlockForWrite
```

The step along the path, with the first two of the eight reads:

```c
// MovementAgentPathFollowerVirtual::FollowPath, SkyrimSE+120D900 (Address Library 92556)
int FollowPath(MovementAgentPathFollowerVirtual* follower, float* dt)
{
    ...
    float t;
    if (!BSPath::FindClosestPoint(follower->path, &current, &closest, &t)) {   // follower + 0x50: first read
        follower->state = 3;
    } else {
        uint32_t from, to;
        BSPath::SegmentForParam(follower->path, t, &from, &to);               // second read: null after a ClearPath
        ...                                                                   // six more reads of follower->path
    }
    ...
}
```

The read that faults:

```c
// BSPath::SegmentForParam, SkyrimSE+11BCBA0 (Address Library 90989)
void SegmentForParam(BSPath* path, float t, uint32_t* from, uint32_t* to)
{
    if (t == (float)(path->count - 1)) {                          // 11BCBB0: mov r8d, [rcx+0x80] with path = null
        ...
    }
    ...
}
```

ClearPath, which the other thread runs, and which does take the lock:

```c
// MovementAgentPathFollowerVirtual::ClearPath, SkyrimSE+120D550 (Address Library 92545)
void ClearPath(IMovementPathManagerAgent* agent)
{
    MovementAgentPathFollowerVirtual* follower = agent - 0x20;
    follower->lock.LockForWrite();                                // follower + 0x38
    Release(follower->request);  follower->request = nullptr;     // + 0x48
    Release(follower->path);     follower->path = nullptr;        // + 0x50
    if (follower->state == 1) follower->state = 3;
    else if (follower->state == 2) follower->state = 0;
    follower->lock.UnlockForWrite();
}
```

The flight follower's update (`MovementAgentPathFollowerFlight::FollowPath`, SkyrimSE+120C450, Address Library 92509) has the same shape. It reads its path at +0x48 three times and leaves its lock at +0xA0 alone.

My functions (`src/fixes/PathFollowerRace.cpp`):

```cpp
// What 1.7.104 does inside PlannerUpdate: the follower's lock around the whole update.
void VirtualUpdate(void* a_planner, void* a_dt, void* a3, void* a4)
{
    auto* lock = reinterpret_cast<RE::BSReadWriteLock*>(static_cast<char*>(a_planner) + 0x20);  // follower + 0x38
    lock->LockForWrite();
    g_virtualUpdate(a_planner, a_dt, a3, a4);  // the original PlannerUpdate
    lock->UnlockForWrite();
}

// The same for the flight follower, which no game build locks.
void FlightUpdate(void* a_follower, void* a_dt, void* a3, void* a4)
{
    auto* lock = reinterpret_cast<RE::BSReadWriteLock*>(static_cast<char*>(a_follower) + 0xA0);
    lock->LockForWrite();
    g_flightUpdate(a_follower, a_dt, a3, a4);  // the original FollowPath
    lock->UnlockForWrite();
}
```

## Why the fix looks like this
This is Bethesda's fix from 1.7.104, applied from outside: the follower's lock, taken for write, around the whole update. While the update holds it, a writer on another thread has to wait, so the update sees one path from its first read to its last.

I copied it because a lock around this update is not obviously safe. The update calls into pathfinding, into the actor and into the game's message queues, all with the lock held. If any of those calls waited for a lock that a waiting writer already holds, the two threads would wait for each other forever. Ruling that out from the code alone means knowing every lock on both sides. Bethesda shipped the lock, though, and 1.7.104 runs with it.

What makes that count for the older builds is how little changed. Between 1.6.1170 and 1.7.104 the update gained the lock, and the code around it stayed as it was: the step, the three writers and their callers, the Patrol teleport, the batch, and everything the update calls. With the fix in, 1.6.1170 takes its locks in the same order as 1.7.104.

The flight follower's half has no such backing, because Bethesda left that update unlocked. I added it anyway: the update has the same gap, and its writers take their lock the same way.

The cost is small. Each follower has its own lock, so threads that update different actors never wait for each other, and a lock nobody else holds is cheap to take.

Before it patches, Install checks that each follower's vtable points at the functions I expect (or at another plugin's hook of that slot), that each writer takes its lock at the offset I use, and that both updates start with the expected bytes. If any of that fails, it patches nothing. It also looks whether the virtual follower's update already takes the lock. On 1.7.104 it does, and there the fix patches only the flight follower.

## What else I considered
A writer could queue its change whenever another thread is inside the update, and the update could apply the queue on its way out. Nothing ever waits in that design, so it can't deadlock, whatever other locks the game holds. But it needs a table of running updates, a queue per follower and a reference on every queued path, and all of that would be new code that only this plugin runs. Bethesda's version is two calls and has shipped.

A null check before the second read stops this one crash. It does nothing about a SetPath that releases the path under the update.

Locking only the first two reads leaves the other six open.

The Patrol teleport could be kept from clearing its members' paths. That removes one caller, and any other code that clears an actor's movement from another thread still runs into the update.
