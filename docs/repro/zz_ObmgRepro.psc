Scriptname zz_ObmgRepro
{Repro for SKSE CleanDroppedStacks: a save while running stacks hold ModEvent handles.}

; Holds a ModEvent handle on a stack that stays running (latent Wait) for secs seconds.
; StorageUtil "zz_obmg_held" counts the handles held right now (read with Held()).
Function Hold(float secs) global
	int h = ModEvent.Create("zz_ObmgRepro")
	StorageUtil.AdjustIntValue(None, "zz_obmg_held", 1)
	Utility.Wait(secs)
	ModEvent.Release(h)
	StorageUtil.AdjustIntValue(None, "zz_obmg_held", -1)
EndFunction

; Negative control: a handle never released; its stack ends here, so the next save must free it.
int Function Leak() global
	return ModEvent.Create("zz_ObmgRepro")
EndFunction

; How many Hold calls hold a handle now.
int Function Held() global
	return StorageUtil.GetIntValue(None, "zz_obmg_held", 0)
EndFunction
