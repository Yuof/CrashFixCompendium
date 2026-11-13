# SkseStackLookup

The game closes while it saves: a quicksave, an autosave or a save from the menu. Crash Logger shows `EXCEPTION_ACCESS_VIOLATION` in SKSE's own DLL, at `skse64_1_6_1170.dll+5D51F` `mov r15, [rax+0x18]` with SKSE 2.2.8 (`+5D626` with 2.2.6), and `RAX` is `0x80000003`. Among the objects are `SKSEPersistentObjectStorage*` and a `SaveStorageWrapper*`. The last line in skse64.log is `Saving SKSEPersistentObjectStorage data...`.

The same bug has a quiet form: a mod event that a mod creates, holds across a save and then sends never arrives. skse64.log then shows `SKSEPersistentObjectStorage::TakeObject(<n>): Object was NULL.` when the mod's script calls `Send` or `Release` with it.

SKSE 2.2.6 and 2.2.8 both contain the check. It takes effect whatever mods you run, as long as one of them holds a handle while the game saves. The fix is mine. I sent the same change to SKSE as [pull request #65](https://github.com/ianpatt/skse64/pull/65).

## How to reproduce it
1. Compile the script [`zz_ObmgRepro.psc`](repro/zz_ObmgRepro.psc) and put the .pex in `Data\Scripts`. Its global function `Hold` creates a ModEvent handle, waits, then releases the handle.
2. Load a save.
3. Call the global function `zz_ObmgRepro.Hold` with the argument `180` ten times, with a tool that calls Papyrus functions from outside the game ([devbench](https://github.com/alandtse/devbench)'s `papyrus call`) or from a console extension. Each call is a script that holds one handle for three minutes.
4. Open the console and enter `save test` within those three minutes.

Without the fix, some of these saves crash, and others drop a handle without any message until the script releases it. Which one happens depends on memory the game reuses, so it takes a few attempts. With the fix, the save never crashes and every handle survives it. The steps as I ran them: [`skse_stack.py`](repro/skse_stack.py).

## Where I met it
A player sent me two crash logs from saves that closed the game, both with this signature. I reproduced it in my own load order with the script above.

## Why it crashes
`ModEvent.Create`, `UICallback.Create` and `SpawnerTask.Create` each create an object inside SKSE and return a handle to it. SKSE keeps those objects in a storage and writes them into the co-save, so that a handle stays valid across a save and load.

`ModEvent.Send` or `ModEvent.Release` with the handle removes the object from the storage. A script that ends before either call leaves its object there. So before every save, SKSE goes through the storage and drops each object whose script has ended.

For each object, SKSE looks up the script's stack in the Papyrus VM's table of running stacks. A stack that is not in the table has ended, and its object is dropped. When the stack is in the table, SKSE reads two more pointers through it and drops the object if the second one is 0.

Those two reads follow the layout of a stack in Skyrim LE, a 32-bit game. In SE and AE, the 8 bytes at the stack's offset `+0x18` are not a pointer. They are a 4-byte header of a small array, the stack's list of memory pages, followed by 4 bytes the game never writes. The header's top bit marks the pages as stored inside the stack, and its low bits hold the array's capacity of 3: `0x80000003`.

When the 4 unwritten bytes are 0, SKSE reads `0x80000003` as an address and then reads at `0x8000001B`. That is the crash. `RAX = 0x80000003` in the crash log is that header.

When those 4 bytes still hold old data, the value points somewhere in the game's memory. SKSE reads whatever is there. A non-zero value keeps the object, which is the right outcome by chance. A zero value drops the object of a script that is still running. When that script later sends or releases its handle, the object is gone.

## The code, with the fix in
SKSE's check, from its source (`PapyrusVM.cpp`, SKSE 2.2.8):

```cpp
VMStackInfo* VMClassRegistry::GetStackInfo(UInt32 stackId)
{
	VMStackInfo* result = NULL;

	stackLock.Lock();

	VMStackTableItem* item = allStacks.Find(&stackId);    // the VM's table of running stacks

	if (item != NULL &&
		item->data != NULL &&                              // the stack itself
		item->data->unkData != NULL)                       // BUG: stack + 0x18, the page array's header, read as a pointer
	{
		result = item->data->unkData->stackInfo;           // BUG: reads at 0x80000003 + 0x18
	}

	stackLock.Release();

	return result;
}
```

Its only caller, before the save writes the storage (`PapyrusObjects.cpp`):

```cpp
void SKSEPersistentObjectStorage::CleanDroppedStacks()
{
	...
	for (UInt32 i=0; i<data_.size(); i++)
	{
		Entry& e = data_[i];
		...
		if (registry->GetStackInfo(e.owningStackId) != NULL)
			continue;                                      // the script is running: keep its object

		delete e.obj;                                      // the script has ended: drop its object
		...
	}
}
```

The game's stack constructor, where the 4 bytes after the header stay unwritten:

```cpp
// SkyrimSE+1432290 (Address Library 104479)
BSScript::Stack* BSScript::Stack::Stack(Stack* this, IMemoryPagePolicy* policy, IProfilePolicy* profilePolicy, ...)
{
	this->policy = policy;                   // +0x08
	this->profilePolicy = profilePolicy;     // +0x10
	this->refCount = 0;                      // +0x00
	*(uint32_t*)&this->pages = 0x80000000;   // +0x18: the page array's header, inline storage, capacity 0
	                                         // +0x1C: never written, holds what the memory held before
	...
}
```

In SKSE 2.2.8 the compiler inlined `GetStackInfo` into `CleanDroppedStacks`. The fix rewrites the last 13 bytes of that inlined check:

```asm
; skse64_1_6_1170.dll+5D50D (SKSE 2.2.8)
mov  rax, [rax+8]       ; the stack, from the table entry
test rax, rax
je   done               ; no stack: drop the object
mov  rax, [rax+18h]     ; BUG: the page array's header as a pointer      PATCHED: mov r15, rax
test rax, rax           ;                                                 PATCHED: nop
je   done               ;                                                 PATCHED: nop
mov  r15, [rax+18h]     ; BUG: reads at 0x8000001B                        PATCHED: nop
done:                   ; r15 = 0 drops the object, anything else keeps it
```

With the patch, `r15` holds the stack pointer itself. The object is kept exactly when its script's stack is in the VM's table.

## Why the fix looks like this
`CleanDroppedStacks` keeps an object exactly while its script runs. A stack stays in the VM's table while it waits for a latent function such as `Utility.Wait`, and leaves the table when the script ends. So the table lookup alone gives the right result, and the fix keeps the lookup and removes the two reads.

The patch is the same change as my pull request, applied to the SKSE you have installed. It touches only SKSE's DLL, in place, and needs no trampoline.

Install finds SKSE's DLL as the module that holds SKSE's messaging interface, so the runtime's DLL name doesn't matter. It looks for the check's bytes in the DLL's code, with the register of the last read left open. It patches only when there is exactly one match. An SKSE that contains the fix has no match, and the fix stays off and logs why. A DLL with more than one match is left untouched too.

## What else I considered
I could correct SKSE's struct layout instead, so that the two reads find real fields. In Skyrim LE they led to the function at the bottom of the script's call stack. In SE and AE that function sits behind the stack's first memory page, which a stack does not always have, and which is not inside the stack once it has more than three pages. The table lookup needs none of these fields.

SKSE's newer releases target a newer game version. Once an SKSE release for your game version carries pull request #65, this fix finds nothing to patch and turns itself off.
