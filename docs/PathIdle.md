# PathIdle

A save crashes a second or two after it loads, every time. The log says `SkyrimSE.exe+0CEC680`, `mov r8, [rdx]`, "Tried to read memory at 0x...E8". The stack names the constructor of `MovementMessagePlayIdle`, and the objects include `MovementAgentPathFollowerStandard` and `PathFollowerStatePlayIdle`.

Crash Logger labels that address `69161+0xB0`. That is misleading: the address is the start of its own function, a string copy.

## How to reproduce it
You need a save that caught an NPC in one particular state of its path follower. Load it, and the game crashes about two seconds later, on every load.

With the fix on, the load logs `[PathIdle] path follower ...: state 14 without its idle -> state 1`, and the game goes on.

Making such a save takes many attempts, because the state lasts a few frames. Saving during a fight on a beach full of creatures caught one in about 2% of the saves.

## Where I met it
Saving and loading on a beach with horkers and soldiers around. The crash comes right after the load. In the save, a horker loads in the bad state.

## Why it crashes
Every NPC that walks a path has a path follower, a small state machine. Before an NPC starts walking, stops or turns sharply, it plays a short animation, which the game calls an idle. State 14 of the machine means: I have picked the idle, and I am waiting a few frames before I play it.

When the wait ends, state 14 hands the idle to the next state. It doesn't check that it has one. The states on either side of it, 13 and 15, do check.

In a running game that never matters, because state 14 always has its idle. A save is where it goes wrong. The game saves the state number and the wait. It doesn't save the idle. So an NPC saved in state 14 comes back in state 14 with no idle, waits out its few frames, and builds a "play this idle" message from a null pointer. The 0xE8 in the crash log is an offset into that missing idle.

## The code, with the fix in
The fix changes no code in the game. It replaces one entry in the path follower's vtable, so the game calls my function, and mine calls the game's.

My function (`src/fixes/PathIdle.cpp`), which is where the fix lives:

```cpp
// agent + 0x110  BSReadWriteLock   the follower's own lock
// agent + 0x118  uint32            state:  1 = follow the path,  2 = play the idle,  14 = wait to play it
// agent + 0x130  smart pointer     the idle picked for the next start / stop / turn
// agent + 0x148  pointer           the current state's object

bool LostIdle(void* a_agent)
{
    return At<std::uint32_t>(a_agent, kState) == kWaitIdle && !At<void*>(a_agent, kIdleData);
}

// Takes the place of vtable slot 2, Update. The game calls it on the agent's sub-object at + 0x18.
std::uintptr_t UpdateThunk(void* a_sub, void* a_2, void* a_3)
{
    auto* agent = static_cast<std::byte*>(a_sub) - 0x18;
    if (LostIdle(agent)) {                                 // the check state 14 lacks
        auto& lock = At<RE::BSReadWriteLock>(agent, kLock);
        lock.LockForWrite();
        const bool reset = LostIdle(agent);                // again, now that nobody else can change it
        if (reset) {
            g_clearIdle(agent);                            // what the game's SetPath does when it
            At<std::uint32_t>(agent, kState) = kFollowPath;   // throws a picked idle away: clear the
            At<void*>(agent, kStateObj) = nullptr;         // idle, go to state 1, no state object
        }
        lock.UnlockForWrite();

        if (reset) {
            logger::info("[PathIdle] path follower {:X}: state 14 without its idle -> state 1"sv,
                reinterpret_cast<std::uintptr_t>(agent));
        }
    }
    return g_update(a_sub, a_2, a_3);                      // the game's Update
}
```

The game's side, unchanged (decompiled). First the piece of `Update` that handles states 14 and 15:

```c
// MovementAgentPathFollowerStandard::Update, SkyrimSE+11B0870 (Address Library 90711)
switch (agent->state) {
    ...
case 14:                                               // waiting to play the picked idle
    if (agent->wait != 0) {                            // agent + 0x160
        break;                                         // not yet
    }
    EnterPlayIdle(agent, &message);                    // BUG: no look at agent->idle first
    goto stateChanged;

case 15:
    if (agent->idle != nullptr) {                      // its neighbour checks
        ...
```

Then the function it calls:

```c
// SkyrimSE+11B3A20 (Address Library 90750)
void MovementAgentPathFollowerStandard::EnterPlayIdle(Agent* agent, void* message)
{
    ...
    FUN_1412281a0(&agent->playIdleState,               // agent + 0x48: the play-idle state object
                  message,
                  &agent->idle,                        // agent + 0x130: null after a load
                  &owner);                             // further down, this builds the
                                                       // MovementMessagePlayIdle, which reads idle + 0xE8
    agent->state = 2;
    agent->stateObject = &agent->playIdleState;
    ...
}
```

With the fix, an agent in state 14 without an idle never gets as far as `case 14`. My function has already put it in state 1.

Before it hooks anything, Install looks for seven instruction patterns in the game's code that use these offsets, including state 14's handler itself. If one is missing, it hooks nothing.

## Why the fix looks like this
State 14 gets the check its neighbours have, and I put it in front of the game's `Update` so I don't have to patch the middle of a 2,400-byte function.

The recovery follows the game. Its own `SetPath` discards a selected idle in exactly these three steps, under this lock. After the reset the NPC's states carry on as in normal play.

The price is one comparison per follower per update.

When Install doesn't find the code it expects, it hooks nothing, and the save crashes as before.

## What else I considered
Setting the state to 1 and nothing else also stops the crash. But the game's `SetPath` clears the idle and the state object too, and I saw no reason to do less than the game does.

A null check patched into state 14 itself would be the direct fix. It means patching the middle of that 2,400-byte function, where the vtable swap touches no code at all.

Making the game save the idle would fix the cause and do nothing for the saves that already have the problem. Those are the ones people want to load.
