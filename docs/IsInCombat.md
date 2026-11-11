# IsInCombat

The game crashes while a console command runs. Often there is no crash log at all, only the Windows event: SkyrimSE.exe, 0xc0000005, fault offset 0xcc90cf. When there is a log, it says `SkyrimSE.exe+0CC90CF`, `cmp [rdi], eax`, under `TESObjectREFR::GetDisplayFullName`.

That address alone proves nothing. It is the game's generic read lock, and the FreeCamera race ends there too.

## How to reproduce it
1. Open the console and click a chair. Any reference that isn't an actor will do.
2. Type `IsInCombat`.

Without the fix the game crashes. With it the console says "Wooden Bar Stool is not in combat". Actors are unaffected either way: `player.IsInCombat` answers "Prisoner is not in combat", and with a hostile bandit nearby "is in combat".

## Where I met it
In the middle of a console `cow`. Nobody had typed `IsInCombat`.

The mod involved was [Smart NPC Potions](https://www.nexusmods.com/skyrimspecialedition/mods/40102). It does nothing wrong. It restarts a quest, `NPCPotion_NPCQuest`, every five seconds, and the quest's three aliases each test `IsInCombat` first on every reference in the loaded area. That comes to more than 65,000 such tests on things that aren't actors in two minutes. Those normally run on job threads, where they are harmless.

A console `cow` or `coc` changes that. The cell change makes the game run its story manager right there, on the thread that executes the console command. If the quest restarts in that moment, its aliases fill while the console command is still running, and every one of those tests runs in console mode.

So you need two things at once: a console command that moves the player, typed by you or sent by a mod or tool, and a quest that tests `IsInCombat` on things that aren't actors. Stop the mod's two quests and those tests stop.

## Why it crashes
While a console command runs, condition functions print their result to the console. `IsInCombat` prints "X is in combat" or "X is not in combat".

The function only accepts an actor as its subject. For anything else it sets its subject to null and its answer to
0. Then, in console mode, it prints the subject's name, and it asks the null subject for it.

Two other functions print a name this way, `GetActorWarmth` and `IsOverEncumbered`. Both check their subject first.

## The code, with the fix in
The fix changes no code in the game. `IsInCombat` has an entry in the game's table of script commands, with two function pointers: one for the console and scripts, one for conditions. I swap both for my own functions, which call the game's.

My functions (`src/fixes/IsInCombat.cpp`), which are the fix:

```cpp
// The one case the game crashes on: console mode, and a subject that is not an actor.
bool Caught(RE::TESObjectREFR* a_ref, double& a_result)
{
    const bool isActor = a_ref && a_ref->GetFormType() == RE::FormType::ActorCharacter;
    if (isActor || !RE::ConsoleLog::IsConsoleMode()) {
        return false;                                  // not that case: let the game answer
    }

    a_result = 0.0;                                    // the answer the game was about to give
    ...                                                // log the 1st, 10th, 100th ... catch
    return true;
}

// the condition function
bool ConditionThunk(RE::TESObjectREFR* a_thisObj, void* a_param1, void* a_param2, double& a_result)
{
    return Caught(a_thisObj, a_result) || g_condition(a_thisObj, a_param1, a_param2, a_result);
}

// the console / script handler
bool ExecuteThunk(const RE::SCRIPT_PARAMETER* a_paramInfo, RE::SCRIPT_FUNCTION::ScriptData* a_scriptData,
    RE::TESObjectREFR* a_thisObj, RE::TESObjectREFR* a_containingObj, RE::Script* a_scriptObj,
    RE::ScriptLocals* a_locals, double& a_result, std::uint32_t& a_opcodeOffsetPtr)
{
    if (Caught(a_thisObj, a_result)) {
        RE::ConsoleLog::GetSingleton()->Print("%s is not in combat",        // the line the game meant to print
            a_thisObj ? a_thisObj->GetDisplayFullName() : "");
        return true;
    }
    return g_execute(a_paramInfo, a_scriptData, a_thisObj, a_containingObj, a_scriptObj, a_locals, a_result,
        a_opcodeOffsetPtr);
}
```

The game's condition function, unchanged (decompiled). The console handler parses its arguments and calls this one, so both paths end here:

```c
// SkyrimSE+3318A0 (Address Library 21576)
bool IsInCombat(TESObjectREFR* subject, void* param1, void* param2, double* result)
{
    Actor* actor = nullptr;
    *result = 0.0;

    if (subject != nullptr) {
        if (subject->formType == 0x3E) {               // an actor
            actor = (Actor*)subject;
        }
        if (actor != nullptr && actor->IsInCombat()) {
            ...                                        // with a parameter: a further test
            *result = 1.0;
        }
    }
    // From here on, `actor` is null for a chair, and for no subject at all.

    if (ConsoleLog::IsConsoleMode()) {                 // a flag in thread-local storage
        if (fabs(*result) <= 0.0001) {
            ConsoleLog::Print("%s is not in combat", actor->GetDisplayFullName());    // BUG: actor is null
        } else {
            ConsoleLog::Print("%s is in combat", actor->GetDisplayFullName());
        }
    }
    return true;
}
```

With the fix, a non-actor in console mode never reaches this function.

Install finds the table entry by the command's name. It wants both pointers and exactly one parameter, and it changes nothing otherwise.

## Why the fix looks like this
The game already had the right answer. It set the result to 0 and then crashed printing it. So my functions return that 0 and, on the console path, print the line with the reference's real name.

Actors, and every call outside console mode, go straight to the game's functions. Nothing changes for them.

I find the command by name, so the fix needs no code addresses at all.

I log by milestones because one console `coc` can run a quest fill over thousands of references.

## What else I considered
A null check patched into the game's function, on the two calls that fetch the name, also works. It prints " is not in combat" with no name in front, though, and it needs code addresses. The table swap needs none and prints the name.

The same swap is in Engine Fixes' queue as pull request #109. Once an Engine Fixes release has it, this fix is no longer needed.

Patching Smart NPC Potions would not fix the bug. Any quest with such a condition gets there the same way.
