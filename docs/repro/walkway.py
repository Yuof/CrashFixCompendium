def walkway(p):
    """DecalVertexTable. Needs Capital Whiterun Expansion: its SurScafWalkwayNew.nif has a shape with unused
    vertices. Goes to the walkway's cell at noon, puts k=<n> (default 8) X markers in a ring of radius=<r> (500)
    around the walkway reference ref=<id>, height=<h> (400) above it, and casts Firebolt from every marker at it:
    volleys=<n> (default 60), gap=<ms> (400) apart. Scorch marks are simple decals, like footprints.

    The reference id 0x22005947 is the walkway in my load order; the first byte depends on yours."""
    import math
    k, vol, gap = int(p.get("k", 8)), int(p.get("volleys", 60)), int(p.get("gap", 400))
    radius, height = float(p.get("radius", 500)), float(p.get("height", 400))
    spell, ref = p.get("spell", "0x1C789"), p.get("ref", "0x22005947")
    g.travel(g.con("cow WhiterunWorld 5 -2")); g.ask("set gamehour to 12"); g.console("tgm"); time.sleep(3)

    pos = [g.call("ObjectReference", f"GetPosition{a}", on=ref) for a in "XYZ"]
    check("walkway ref", all(isinstance(v, (int, float)) for v in pos), f"ref {ref} at {pos}")
    x, y, z = pos

    srcs = []
    for i in range(k):
        s = g.call("ObjectReference", "PlaceAtMe", {"form": "0x3B"}, 1, False, False, on=ref)["formId"]
        a = 2 * math.pi * i / k
        g.call("ObjectReference", "SetPosition", x + radius * math.cos(a), y + radius * math.sin(a), z + height, on=s)
        srcs.append(s)

    cast = [{"tool": "papyrus", "args": {"action": "call", "script": "Spell", "function": "RemoteCast",
                                         "self": {"form": spell},
                                         "args": [{"form": s}, {"form": "0x14"}, {"form": ref}]}} for s in srcs]
    r = g.tool("scenario", timeout=2 * vol * (gap / 1000 + k * 0.05) + 120, steps=cast + [{"wait": gap}],
               repeat=vol, continueOnError=True)
    res = r.get("results") or []
    bad = [x for x in res if x.get("ok") is False]
    check("volleys cast", r.get("ok") is not False and len(bad) < len(res) / 10, f"{len(res)} steps, {len(bad)} failed")
    time.sleep(65)
    for s in srcs: g.call("ObjectReference", "Delete", on=s)
