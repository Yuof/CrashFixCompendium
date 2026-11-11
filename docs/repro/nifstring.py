def nifstring(p):
    """NifStringIndex. Before the launch, replace a mesh with a copy from mknif.py (the name index of its first block
    points past the file's string list). Then place an object that uses the mesh at the player, so the game reads the
    file. The game reads a mesh once per launch.

    base=<id>: default 0xDA74D, Barenziah's crown, mesh Clutter\\Quest\\BarenziahsCrownComplete.nif."""
    base = p.get("base", "0xDA74D")
    f = g.call("ObjectReference", "PlaceAtMe", {"form": base}, 1, False, False, on="0x14")["formId"]
    time.sleep(5)
    check("object placed", g.call("ObjectReference", "Is3DLoaded", on=f) is True, f"{base} as {f}")
    g.call("ObjectReference", "Delete", on=f)
