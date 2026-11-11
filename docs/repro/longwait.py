def longwait(p):
    """UIMessageQueue. Changes worldspace cell and back, then waits hours=<h> (default 168) in Riverwood, waits=<n>
    times (default 6). Cell changes and long waits make the game and the HUD plugins send many menu messages from
    several threads at once.

    This does not crash on demand. I used it to count how often two threads were inside AddMessage together."""
    h = int(p.get("hours", 168))
    g.travel(g.con("cow Tamriel 0 -6")); g.ask("set gamehour to 12")
    for k in range(1, int(p.get("waits", 6)) + 1):
        g.goto("Riverwood"); g.travel(g.con("cow Tamriel 0 -6")); g.goto("Riverwood")
        t0 = g.call("Utility", "GetCurrentGameTime")
        g.wait_hours(h)
        d = (g.call("Utility", "GetCurrentGameTime") - t0) * 24
        check(f"wait {k}", h - 0.1 <= d <= h + 0.6, f"{d:.2f} h")
