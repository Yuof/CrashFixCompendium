def skse_stack(p):
    """SkseStackLookup. Needs zz_ObmgRepro.pex (zz_ObmgRepro.psc here) in Data\\Scripts. Starts k=<n> (default 10)
    scripts that each hold a ModEvent handle for 180 seconds, waits until all of them hold it, saves, and then waits
    until all of them have released it again. Without the fix the save crashes in skse64_*.dll, or a handle is
    dropped: skse64.log then shows "CleanDroppedStacks: Freed object" at the save and
    "TakeObject(<n>): Object was NULL." when the script releases it. With the fix neither happens.

    leak=1: one extra handle from Leak(), whose script ends at once. That is the control: the save must drop exactly
    that one ("Freed object" once) with the fix as well."""
    k = int(p.get("k", "10"))
    if p.get("leak") == "1":
        g.call("zz_ObmgRepro", "Leak")

    for _ in range(k):
        # Hold waits 180 s, so devbench answers 504 (still running) after its 5 s timeout: that is expected
        g.call("zz_ObmgRepro", "Hold", 180.0, timeout_ms=5000)
    g.until(f"{k} handles held", lambda: g.call("zz_ObmgRepro", "Held") == k, 90, 2)

    g.console("save zz_skse_stack")
    time.sleep(15)
    check("no crash at the save", g.running(), "")

    g.until("every handle released", lambda: g.call("zz_ObmgRepro", "Held") <= 0, 240, 2)
    log = skse_log_since_save()
    check("no live handle dropped", log.count("Object was NULL") == 0, log)
