def treedecal(p):
    """SkinnedDecal. Riverwood at noon. Takes the nearest tree whose mesh name contains model=<s> (default pine),
    puts k=<n> (default 8) X markers in a ring of 700 units around it, 150 above its base, and casts Firebolt from
    every marker at the tree in the same frame: volleys=<n> of them, gap=<ms> apart.

    reset=1 disables and enables the tree before each volley. A tree gets its decal node on its first hit only, so
    without the reset a run opens the race window once."""
    import math
    k, vol, gap = int(p.get("k", 8)), int(p.get("volleys", 150)), int(p.get("gap", 300))
    spell = p.get("spell", "0x1C789")
    g.goto("Riverwood"); g.ask("set gamehour to 12"); g.console("tgm"); time.sleep(3)

    refs = g.tool("inspect", kind="refs", model=p.get("model", "pine"), radius=2000, limit=100)["refs"]
    me = g.scene()["position"]
    refs = [r for r in refs if r.get("position")]
    check("tree found", bool(refs), f"{len(refs)} refs with '{p.get('model', 'pine')}' within 2000")
    t = min(refs, key=lambda r: math.dist(r["position"], me))
    x, y, z = t["position"]

    srcs = []
    for i in range(k):
        s = g.call("ObjectReference", "PlaceAtMe", {"form": "0x3B"}, 1, False, False, on=t["formId"])["formId"]
        a = 2 * math.pi * i / k
        g.call("ObjectReference", "SetPosition", x + 700 * math.cos(a), y + 700 * math.sin(a), z + 150, on=s)
        srcs.append(s)

    # one RemoteCast per marker, all in one scenario step list: they land in the same frame
    cast = [{"tool": "papyrus", "args": {"action": "call", "script": "Spell", "function": "RemoteCast",
                                         "self": {"form": spell},
                                         "args": [{"form": s}, {"form": "0x14"}, {"form": t["formId"]}]}} for s in srcs]
    tc = lambda f, *a: {"tool": "papyrus", "args": {"action": "call", "script": "ObjectReference", "function": f,
                                                    "self": {"form": t["formId"]}, "args": list(a)}}
    reset = [tc("Disable", False), {"wait": 100}, tc("Enable", False), {"wait": 500}] if p.get("reset") == "1" else []

    r = g.tool("scenario", timeout=2 * vol * (gap / 1000 + k * 0.05 + (1 if reset else 0)) + 120,
               steps=reset + cast + [{"wait": gap}], repeat=vol, continueOnError=True)
    res = r.get("results") or []
    bad = [x for x in res if x.get("ok") is False]
    check("volleys cast", r.get("ok") is not False and len(bad) < len(res) / 10,
          f"tree {t['formId']} {t.get('model')}; {len(res)} steps, {len(bad)} failed")
    time.sleep(5)
    for s in srcs: g.call("ObjectReference", "Delete", on=s)


# I ran it as: treedecal reset=1 volleys=40
