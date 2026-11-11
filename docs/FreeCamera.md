# FreeCamera

You move around in the free camera (`tfc`, or a mod's photo or preview camera) while a fight starts or ends, and the game crashes. One race, three crash logs:

- `SkyrimSE.exe+0CC90CF`, `cmp [rdi], eax`, a read of 0x160
- `SkyrimSE.exe+083D0CD`, `mov eax, [rdx+rcx*4]`, with an index near 4 billion
- `VCRUNTIME140.dll`, in memmove, `mov r8, [rdx]`, under `CombatGroup::RemoveMember`

The log names `PlayerCharacter`, `CombatGroup`, or both. The first address alone proves nothing: it is the game's generic read lock, and other bugs end there too.

## How to reproduce it
1. `tfc`.
2. Spawn an enemy that attacks you. When you are in combat, disable it. Repeat with a fresh enemy.

Without the fix the game crashes within about 20 rounds. Script: [repro/swingdisable.py](repro/swingdisable.py) with `tfc=1 n=40`.

With the fix it never does. Most crashes are the `+0CC90CF` form; the other two occur less often.

Without `tfc` nothing happens at all, because the main thread never runs the code in question.

## Where I met it
Several times, always in my screenshot tool, which keeps the game in the free camera and moves it around. The overlap occurs in almost half of the screenshot runs and never without the free camera.

Normal play doesn't use the free camera, so normal play is safe. Screenshot sessions and photo-mode mods are not.

## Why it crashes
The player belongs to a combat group while in combat. A job on a worker thread looks after that group every frame: it creates the group, adds and removes the player, and drops it when the fight ends.

`Main::Update` has a special case for the free camera. In that mode it runs a cut-down update of the player on the main thread, and that update ends by calling the same combat-group upkeep. So in the free camera two threads update the same group in the same frame, and neither takes a lock.

All three crashes are that overlap:

- One thread stores a new group in the player and reads the field back. The other thread cleared it in between. The first thread takes the lock of a null group: 0x160 is the lock's offset.
- Both threads remove the player from the group. The second computes how much to move from a count the first already lowered, and memmove gets a wrapped source.
- Both threads decrement the member count. It wraps to 0xFFFFFFFF, and the next walk over the members leaves the array.

## The code, with the fix in
The fix removes one call. This is the whole function, decompiled with the patch in:

```c
// SkyrimSE+733E60 (Address Library 40450). Main::Update calls this instead of the normal player update
// while the camera is in its free state. FinishLoadGame calls it too, with skipUpdate = true.
void PlayerCharacter::UpdateInFreeCamera(PlayerCharacter* player, float delta, bool skipUpdate)
{
    player->dropAngleMod = 0.0f;
    player->lastDropAngleMod = 0.0f;
    player->playerFlags.unk1_0 = 0;
    UpdateCamera(PlayerCamera::GetSingleton());

    if (!skipUpdate) {
        TESCameraState* state = PlayerCamera::GetSingleton()->currentState;
        if (state != nullptr && state->id > 8) {
            player->currentProcess->RandomlyPlaySpecialIdles();
        }

        ...                                            // clear the player's run-once package; a branch for a
                                                       // sleeping player

        if (!player->IsDead() && player->actorState1.lifeState != 1 && player->sleepSeconds == 0) {
            FUN_1406c4d90(player, 0);
        }

        Actor::Update(player, delta);                  // the actor's own update

        if (FUN_1406ec7a0(player->currentProcess) != 0) {
            FUN_1407e1060(FUN_1406ec7a0(player->currentProcess), delta, player);
        }
        player->currentProcess->CheckForExpiredDetectionEvent();

        // PATCHED: the function used to end this block with
        //     UpdatePlayerCombatGroup(player, delta);     // SkyrimSE+763C60, Address Library 41238
        // Five bytes of nop sit there now. The combat manager's job makes the same call every frame
        // on its worker thread, and that one stays.
    }

    TESWorldSpace* world = player->GetWorldSpace();
    if (world != nullptr && FUN_140307490(world) != 0) {
        ...                                            // one call with the worldspace and the world root node
    }
}
```

In bytes: the `call` at `SkyrimSE+733FEC` (`E8 6F FC 02 00`) becomes `0F 1F 44 00 00`. Install looks for the call by its target and wants exactly one in the function. Otherwise it patches nothing.

## Why the fix looks like this
The cut-down player update has two callers: the free-camera branch of `Main::Update`, and the code that finishes loading a save. The worker's job updates the group every frame no matter which camera is on. So this call never did anything the job doesn't do. It only did it a second time, on the wrong thread. I took it out, and the free camera now treats the combat group like every other camera does.

Loading a save still updates the group. That path has its own call.

One behaviour changes. The jobs don't run while a menu pauses the game or after `tfc 1`. In the free camera with the game paused, the group used to get its update from the main thread and now gets none. Every other camera has always behaved that way under a pause.

## What else I considered
A lock around the combat-group functions works just as well. It stops the crash too. The lock is contended a couple of thousand times per session, with waits up to 218 microseconds. I kept the nop because the call is a duplicate, and removing a duplicate is better than guarding it. If this race ever shows up without the free camera, the lock is the fix to use.
