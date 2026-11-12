def ragfight(p):
    """RagdollWorldLock. In third person with a sword and shield, spawns a bandit in front of the player, holds it
    in place and swings at it swings=<n> times. Repeats that cycles=<n> times with a fresh bandit.

    Each hit makes Precision put the bandit's ragdoll into the physics world, so the ragdoll driver runs for it.
    The test plugin ragdoll_force.cpp then forces the crash's state on that driver.

    I ran it as: cycles=4"""
    c.stage(); c.reset_player(); c.equip("sword+shield"); c.set_pov("3p"); g.console("tgm")
    for i in range(1, int(p.get("cycles", 8)) + 1):
        f = c.spawn()                                        # a bandit at 100 units in front, pinned
        try:
            g.call("Actor", "SetRestrained", True, on=f)
            fs, _, _ = c.face(f)
            c.scen([fs])                                     # turn the player to face it
            hp0 = g.call("Actor", "GetActorValue", "Health", on=f)
            for _ in range(int(p.get("swings", 6))):
                g.tool("input", action="tap", device="keyboard", key="mouseLeft", durationMs=90)
                time.sleep(0.7)
            hp1 = g.call("Actor", "GetActorValue", "Health", on=f)
            check(f"cycle {i}", True, f"bandit health {hp0} -> {hp1}")
        finally:
            c.remove(f)                                      # stop its AI, then delete it
        c.reset_player()
