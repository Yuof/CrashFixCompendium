# ShadowMapStackAlign

The game closes with no crash log. Windows' event log says: faulting module EngineFixes.dll, exception 0xc0000005 (offset 0xF2084 in Engine Fixes 7.0.19). With Engine Fixes' debug symbols, the faulting instruction is `movdqa [rbp-39h], xmm0` inside tbbmalloc, under Engine Fixes' ShadowMap fix, on the main thread.

You can only get this with SSE Engine Fixes 7.0.15 up to the 7.0.21 beta, or alandtse's fork 7.13.0, with `bBSLightingShaderPropertyShadowMap` on.

Neither the diagnosis nor the fix is mine. Both come from [issue #102](https://github.com/aers/EngineFixesSkyrim64/issues/102) on Engine Fixes' repository, whose author found the misaligned stack and wrote the corrected stub. This fix packages it so that it applies to the Engine Fixes you already have.

## How to reproduce it
The crash players get needs a rare path inside the memory allocator, so it doesn't come on demand. Its cause does: a stack that is 8 bytes out of alignment on every call.

1. Build a test version of this fix whose stub (below) skips the alignment, so it keeps Engine Fixes' 8-byte offset. In that build the stub calls a small wrapper of mine first, which counts and logs and then calls Engine Fixes' function.
2. Start the game and look at anything that casts a shadow.

The misaligned test stub crashes on the very first call, at `movaps [rsp+70h], xmm0` in the log call's formatter. That is the same kind of fault as the real crash. It comes at once because my wrapper's log call stores an SSE register right away, where the real code does it only occasionally.

The fix's stub goes through more than 100,000 calls without one misaligned entry.

## Where I met it
Once, in Whiterun. The minidump's stack runs from `BSLightingShaderProperty::GetRenderPasses_ShadowMapOrMask` into Engine Fixes' stub, on into its C++ function, into its render-pass cache, into tbbmalloc, and ends in the allocator's cleanup of its large-object cache, storing an SSE register to a stack address that ends in 8.

Looking for that stack led me to issue #102, which describes this crash and already holds the patch.

## Why it crashes
On 64-bit Windows, the stack pointer must be a multiple of 16 at every call. Compilers rely on it. They store SSE registers to the stack with instructions that fault if the address isn't aligned.

Engine Fixes' ShadowMap fix jumps out of the middle of one of the game's functions into a small piece of hand-written machine code, a stub. The stub calls Engine Fixes' C++ function and jumps back.

At the point where the game's function gets interrupted, its stack is aligned. The stub then pushes one 8-byte register before its call. Now the stack is off by 8, and it stays off for everything the C++ function does.

Most code never stores an SSE register to the stack and never notices. The allocator's cache cleanup does. The C++ function allocates memory, and occasionally that allocation triggers the cleanup.

A misaligned stack faults. It doesn't corrupt anything on the way.

## The code, with the fix in
The fix re-points one jump in the game's code: the jump Engine Fixes put there. It doesn't touch EngineFixes.dll. Both stubs are a dozen instructions of assembly, so there is nothing to decompile. Here they are side by side.

Engine Fixes' stub, where the jump at `SkyrimSE+14AF2C5` goes without this fix:

```asm
mov  rcx, rsi              ; 1st argument for the C++ function
mov  rdx, [rsp+0xF8]       ; 2nd argument, from the game function's stack
mov  r8, <C++ function>
push rbx                   ; BUG: one push. The stack is now 8 off.
sub  rsp, 0x20             ; shadow space
call r8                    ; the C++ function runs on a misaligned stack
add  rsp, 0x20
pop  rbx
mov  r15, rax              ; its result, where the game's function expects it
jmp  [rip]                 ; back into the game's function
```

The corrected stub, where the jump goes with the fix. The instruction sequence is the one from issue #102; `Stub()` in `src/fixes/ShadowMapStackAlign.cpp` emits it:

```asm
mov  rcx, rsi              ; same 1st argument
mov  rdx, [rsp+0xF8]       ; same 2nd argument. Read before any push, so the offset still fits.
push rbx
push rbp
mov  rbp, rsp              ; remember where the stack was
and  rsp, -16              ; round it down to a multiple of 16
sub  rsp, 0x20             ; shadow space
mov  rax, <C++ function>   ; the same function, address taken from Engine Fixes' stub
call rax                   ; runs on an aligned stack
mov  rsp, rbp              ; restore the stack
pop  rbp
pop  rbx
mov  r15, rax
jmp  [rip]                 ; the same way back
```

Install follows the jump from the game's function to Engine Fixes' stub and compares all 51 bytes of it with the stub I know, leaving the two addresses open. It also checks that the stub jumps back to where I expect. Only then does it write the corrected stub and re-point the jump.

## Why the fix looks like this
The stub is the issue author's work: same arguments, same function, same way back, with the stack saved, aligned for the call and restored. I use it unchanged.

My contribution is how it is applied. The issue's patch changes Engine Fixes' source, so it takes effect only in a new Engine Fixes build. This fix applies the same correction at run time to the Engine Fixes DLL you have installed, by swapping the stub after Engine Fixes has set it up.

That is also why it is strict about what it patches. It only replaces a stub it recognises byte for byte. In every other case it does nothing and says why in the log: Engine Fixes isn't installed, its ShadowMap fix is off, or the jump leads to code I don't know. That last case covers an Engine Fixes release that includes the patch. This fix then turns itself off.

## What else I considered
Rebuilding Engine Fixes with the issue's patch works. But I'd have to redo it at every Engine Fixes update, and I'd be running a private build of a core mod.

The issue's author published an interim build. It is someone else's DLL for one game version, and it needs a different Engine Fixes version than I run.

Turning the ShadowMap fix and the render-pass cache off loses both features, and the issue says that turning off one without the other gives a different crash.

Updating Engine Fixes doesn't help: the newer releases still have the stub. And a pull request would only repeat the code that already sits in the issue.
