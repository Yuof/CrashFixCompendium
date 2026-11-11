def gatesweep(p):
    """FootIkRaycast. Stands at the Solitude main gate and waits through the morning, hour by hour (hours=<h,h,..>),
    dwell=<s> seconds at each, so the townspeople walk to their places and load. Then places spawn=<n> copies of the
    NPC base=<id> at the player. With about 40 characters already there, 15 more take the foot-IK table past its 50
    entries. despawn=1 disables the copies again.

    I ran it as: hours=6,8,10 spawn=15 base=0x1020A4 despawn=1   (0x1020A4 is a Solitude guard)"""
    hours = [int(h) for h in str(p.get("hours", "6,8,10,12,14,16,18,20,22")).split(",")]
    dwell = int(p.get("dwell", 70))
    g.travel(g.con("coc SolitudeOrigin"))
    g.ask(f"set gamehour to {hours[0] - 1}")
    for c in ("player.setpos x -64481", "player.setpos y 104738", "player.setpos z -8422"): g.ask(c)

    for h in hours:
        cur = g.call("Utility", "GetCurrentGameTime") % 1 * 24
        g.wait_hours(max(1, round((h - cur) % 24)))
        got = g.call("Utility", "GetCurrentGameTime") % 1 * 24
        time.sleep(dwell)
        npcs = len(g.tool("inspect", kind="refs", formType="NPC_", radius=4000)["refs"])
        check(f"hour {h}", min(abs(got - h), 24 - abs(got - h)) < 0.75, f"{got:.2f} h, {npcs} NPCs")

    if p.get("spawn"):
        made = [g.spawn(p["base"]) for _ in range(int(p["spawn"]))]
        time.sleep(dwell)
        npcs = len(g.tool("inspect", kind="refs", formType="NPC_", radius=4000)["refs"])
        check("spawned", npcs >= int(p["spawn"]), f"{npcs} NPCs")
        if p.get("despawn"):
            for f in made: g.call("ObjectReference", "Disable", False, on=f)
            time.sleep(15)
            check("despawned", all(g.call("ObjectReference", "IsDisabled", on=f) is True for f in made))
