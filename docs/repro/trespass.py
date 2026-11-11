def trespass(p):
    """TrespassPursue. Walks into an occupied house at a time the player trespasses there, stays dwell=<s> seconds,
    leaves, and does it again: trips=<n> times. Each owner at home starts the "you need to leave" escort and builds
    one or two escort packages per run.

    I ran it as: a=RiftenHouseofClanSnowShod b=Riverwood trips=8 hours=13 dwell=20"""
    a, b = p.get("a", "RiftenHouseofClanSnowShod"), p.get("b", "Riverwood")
    hours = [float(h) for h in p["hours"].split(",")] if p.get("hours") else []
    for k in range(1, int(p.get("trips", 8)) + 1):
        if hours:
            h = hours[(k - 1) % len(hours)]
            g.call("GlobalVariable", "SetValue", h, on="0x38")          # GameHour
            got = g.call("GlobalVariable", "GetValue", on="0x38")
            check(f"trip {k} hour", abs(got - h) < 0.25, f"{got:.2f}")
        g.goto(a); check(f"trip {k} in {a}", _in() == a, _in())         # _in(): the player's current cell
        if p.get("dwell"): walk(float(p["dwell"]))
        g.goto(b); check(f"trip {k} left {a}", _in() != a, _in())
