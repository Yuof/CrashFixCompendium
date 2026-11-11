# StartupHook

The game crashes during the first loading screen, before the main menu. The log says `Unhandled exception "EXCEPTION_BREAKPOINT" at 0x7FF7...` and names no module. The stack has `SkyrimSE.exe+0CD8FC0` (the input poll) or `SkyrimSE.exe+0FA508F` (the loading screen's renderer), often with a `JobListManager::ServingThread*` in R14.

It is rare.

## How to reproduce it
The window is a few microseconds wide, so I widen it. A test plugin ([repro/LateHookRaceProbe.cpp](repro/LateHookRaceProbe.cpp)) replaces `VirtualProtect` and sleeps 500 ms at the exact moment another plugin stands between the two halves of installing a hook.

1. Install a plugin that hooks one of the two places above after the game's data has loaded. My load order has five that hook the input poll, SmartTalk 1.0.6 among them, and a loading-screen mod's DLL that hooks the renderer.
2. Install the probe, tell it in its ini which of the two to delay, and start the game.

Without the fix the game crashes on every launch. With it, it reaches the main menu every time and logs one to five `[StartupHook] ... resumed` lines.

## Where I met it
A few times, always before the main menu, at the renderer and at the input poll. In a full dump of one, the instruction pointer sits inside a trampoline slot that, by the time the dump was written, holds a finished jump into SmartTalk.

## Why it crashes
Many SKSE plugins install hooks through CommonLibSSE's trampoline. A hook on a call has two parts. The plugin redirects the call in the game's code to a slot in the trampoline, and it writes a jump to its own function into that slot. Trampoline memory that nobody has used yet holds 0xCC, the breakpoint instruction.

CommonLib writes the two parts in this order: first the call, then the slot.

While SKSE loads plugins that order doesn't matter, because the game isn't running the hooked code yet. But some plugins install their hooks later, when the game tells them its data has loaded. By then the first loading screen is up. One thread polls input and another renders, and both go through exactly the calls these plugins hook. A thread that takes the call after its redirect and before the plugin fills the slot lands on 0xCC.

Each plugin compiles CommonLib in, so every plugin carries its own copy of this. In my load order six plugins hook one of the two places after data load.

## The code, with the fix in
This fix patches nothing, in the game or in any plugin. It registers an exception handler when its DLL loads, and the handler is the fix. So there is no decompile here. This is the handler (`src/fixes/StartupHook.cpp`):

```cpp
LONG CALLBACK Handler(EXCEPTION_POINTERS* a_info)
{
    const auto* record = a_info->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_BREAKPOINT) {
        return EXCEPTION_CONTINUE_SEARCH;                  // not a breakpoint: not mine
    }

    // A breakpoint inside a module's image is there on purpose (an assert, a debugger).
    // A trampoline is private memory that was allocated at run time. Only that is mine.
    const auto address = reinterpret_cast<std::uintptr_t>(record->ExceptionAddress);
    if (!Executable(address, MEM_PRIVATE)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // The thread came too early. Wait for the plugin to finish writing the slot:
    // FF 25 00 00 00 00 <8-byte address>, a jmp [rip], whose target is executable code.
    const auto start = GetTickCount64();
    while (!CompleteSlot(address)) {
        if (GetTickCount64() - start > kWaitMs) {          // 1 s and still no jump: a real crash, pass it on
            return EXCEPTION_CONTINUE_SEARCH;
        }
        Sleep(1);
    }

    a_info->ContextRecord->Rip = address;                  // run the slot again, now that it holds the jump
    const auto n = ++g_resumed;
    logger::info("[StartupHook] #{}: thread {} ran a hook slot at {:X} before it was written; "
                 "resumed after {} ms -> {}"sv,
        n, GetCurrentThreadId(), address, GetTickCount64() - start,
        Owner(AbsJumpTarget(address)));                    // module+offset of the hook: who hooked late
    return EXCEPTION_CONTINUE_EXECUTION;
}
```

A slot before and after the plugin's second write:

```
CC CC CC CC CC CC  CC CC CC CC CC CC CC CC          ; what the early thread executes
FF 25 00 00 00 00  <address of the plugin's hook>   ; jmp [rip]
```

## Why the fix looks like this
The thread that hits the breakpoint did nothing wrong. It ran a jump a moment before the jump existed. So the handler waits until the jump is there and sends the thread through it.

It leaves everything else alone. It acts only on a breakpoint in private executable memory, and it resumes only into a complete jump with an executable target. Anything else goes on to the next handler untouched, and so does this case after one second. A crash logger still gets its crash.

The log line names the module the hook leads to. That tells you which plugin hooked late.

A handler is a workaround. The cause is fixed as well, see below, but that fix is in a library, and it takes effect only when each plugin that hooks late ships a new build.

## What else I considered
The cause is fixed in CommonLib: CommonLibSSE-NG pull request #465 swaps the order, slot first, then the call. With the swap, the probe's own late hook stops crashing. This fix becomes unnecessary when a CommonLib release carries #465 and the late-hooking plugins have been rebuilt on it.

Asking the six authors to hook at plugin load would work as well, one plugin at a time, and only until another plugin does the same.

## What it doesn't cover
A late 6-byte hook fails as an access violation, and the handler passes that on. So does a thread that reads a slot in the nanoseconds while the plugin writes its bytes. Neither has turned up in a crash log. If one does, the handler gets an access-violation path.
