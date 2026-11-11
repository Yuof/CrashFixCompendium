def swingdisable(p):
    """FreeCamera (with tfc=1) and DisabledAttacker (without).

    A bandit spawns in front of the player and fights. The script disables it, n=<n> times (default 40), each time
    with a fresh bandit.

    tfc=1: free camera on. The disable comes at a random moment once the player is in combat. That is all the
    combat-group race needs: a combat that starts and ends while the free camera is on.

    Without tfc: the disable comes just after the bandit's swing should connect, which is the window between
    Precision queueing the hit and running it.

    `c` is my combat test module: c.stage() moves the player to a flat test spot, c.equip() gives the player a
    loadout, c.spawn() places a bandit a few steps in front of the player, and c.aim_at_hit(actor, offset_ms)
    watches the actor's attack animation and returns at the predicted contact time plus the offset."""
    import combat as c
    rng = random.Random(int(p.get("seed", 1)))
    c.stage(); c.reset_player(); c.equip("sword+shield")

    if p.get("tfc"):
        g.console("tfc")
        t0 = time.time()
        while not (on := g.tool("camera").get("freeCam") is True) and time.time() - t0 < 10: time.sleep(0.1)
        check("free camera on", on)

    spent = []
    for i in range(1, int(p.get("n", 40)) + 1):
        f = c.spawn()
        g.call("Actor", "StartCombat", {"form": c.P}, on=f)
        fought = True
        if p.get("tfc"):
            # the PLAYER has to be in combat: only then does the player get a combat group
            t0 = time.time()
            while not (fought := g.call("Actor", "IsInCombat", on=c.P) is True) and time.time() - t0 < 15:
                time.sleep(0.1)
            time.sleep(rng.uniform(0, 1.5))
        else:
            c.aim_at_hit(f, -rng.uniform(0, 60))

        g.call("ObjectReference", "Disable", False, on=f)
        time.sleep(0.5)
        spent.append(f)
        g.console("player.restoreav health 5000")
        check(f"cycle {i}", fought and g.call("ObjectReference", "IsDisabled", on=f) is True)

    if p.get("tfc"):
        check("free camera held", g.tool("camera").get("freeCam") is True)
    time.sleep(3)
    for f in spent: g.call("ObjectReference", "Delete", on=f)
