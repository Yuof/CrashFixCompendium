# DisabledAttacker

An NPC swings at someone, and something disables or unloads that NPC in the middle of the swing. The game crashes at `SkyrimSE.exe+070DB20`, `mov rax, [rcx+0x08]` with RCX = 0. The stack has `Precision.dll`, and the log names `PendingHit::Run`, `HitRefs::Update` and `AttackCollisions::Update`.

You need Precision - Accurate Melee Collisions 2.0.6 for it.

## How to reproduce it
Not reliably. The window is the few frames between Precision queueing a hit and running it.

1. Let a bandit attack you.
2. Disable it right after its swing connects.

Script: [repro/swingdisable.py](repro/swingdisable.py) without `tfc`. It times the disable to the swing.

Even aimed like that, about 1% of the disables fall into the window. When one does, the fix catches it and the log says:

```
pending hit of FF001FA0 dropped
```

A direct test is quicker: call Precision's `PendingHit::Run` from a test plugin with a hit that has no attacker. With the fix on, the game keeps running and logs the dropped hit.

## Where I met it
In scripts that disable actors: one deleted a bandit in mid-attack, another hid every actor in a hall while one of them was swinging. In play it takes a script or a console command that removes an NPC in mid-swing.

## Why it crashes
Precision doesn't apply a melee hit the moment it detects one. It puts the hit in a queue and runs it at a later update.

Running the hit asks the game which target the attacker's swing points at. The game answers that through the attacker's AI process. An actor that got disabled or unloaded after the hit went into the queue has no AI process any more, and the game reads through the null pointer.

A few lines further on, Precision reads the AI process a second time for its hitstop effect. It doesn't check there either.

## The code, with the fix in
The fix patches `Precision.dll`, not the game. The first bytes of `PendingHit::Run` become a jump to my function, which decides whether to run Precision's.

My function (`src/fixes/DisabledAttacker.cpp`):

```cpp
struct PendingHit
{
    RE::NiPointer<RE::Actor> attacker;                 // the first member in 2.0.6, and all I need
};

void RunThunk(PendingHit* a_hit)
{
    const auto* attacker = a_hit->attacker.get();
    if (attacker && attacker->GetActorRuntimeData().currentProcess) {
        g_run(a_hit);                                  // the attacker is still there: Precision's Run
        return;
    }

    // No attacker, or one without an AI process: nobody is left to deal this hit. Drop it.
    if (const auto n = ++g_skipped; Milestone(n)) {    // log the 1st, 10th, 100th ...
        logger::info(
            "[DisabledAttacker] pending hit of {:08X} dropped: "
            "the attacker has no AI process ({} so far)"sv,
            attacker ? attacker->GetFormID() : 0, n);
    }
}
```

Precision is open source, so I can show its side as source too. This is `PendingHit::Run` in 2.0.6 with the same check where it belongs, which is what I proposed upstream. The three marked lines are the proposal. Everything else is Precision's:

```cpp
void PendingHit::Run()
{
	if (!attacker || !attacker->GetActorRuntimeData().currentProcess) {   // proposed
		return;                                                           // proposed
	}                                                                     // proposed

	if (bOnlyFX) {
		RunFXOnly();
		return;
	} else if (attackCollision && attackCollision->bIsRecoiling) {
		// Don't do anything, a hit with caused by a recoil is already queued
		return;
	}
	...
	// further down: Actor::CalculateCurrentHitTargetForWeaponSwing(attacker), which reads through
	// attacker->currentProcess, and the hitstop block, which reads currentProcess->high
}
```

My function does from outside what those three lines do from inside.

Install patches one build only: the released 2.0.6. It compares the DLL's build timestamp and the function's first 15 bytes, and leaves any other Precision alone. The function sits at `Precision.dll+5CA50`. The offset comes from Precision's PDB and means nothing in another build.

## Why the fix looks like this
A hit whose attacker is gone can no longer be applied. Dropping it is the only correct outcome, and it is what Precision would do with the check in place. Every other hit runs unchanged.

The check sits in front of the whole function, so it covers both unchecked reads at once.

I allocate the stub that holds the function's moved first bytes myself. SKSE's trampoline lives next to the game's executable, too far from Precision.dll for a 32-bit jump.

## What else I considered
The right place for the check is Precision itself, and the three lines above are pull request #6 on its repository. I don't install my own build of Precision, though. I want fixes as separate DLLs and other people's mods as their authors shipped them. When a Precision release has the check, this fix is no longer needed.

Guarding the game's function looks reasonable, since the first crash is inside the game. But Precision reads the AI process again right after that call returns. A guard in the game would only move the crash a few lines down.
