def fasttravel(travels=2, start="Whiterun", hour=None):
    """PathFollowerRace: fast travel to Riverwood, `travels` times. Each one starts at `start` (a cell for coc) and passes
    game time, which runs the far-away actors' batch. With path_follower_force.cpp installed, the first fast travel
    crashes at SkyrimSE+11BCBB0 without the fix. I ran it with travels=2 from Whiterun."""
    for k in range(1, travels + 1):
        g.goto(start)
        if hour is not None:
            g.console(f"set gamehour to {hour}")
        g.travel({"tool": "papyrus", "args": {"action": "call", "script": "Game", "function": "FastTravel",
                                              "args": [{"form": "RiverwoodMapMarker"}]}})
        x, y, _ = g.scene()["position"]
        check(f"travel {k}", (int(x // 4096), int(y // 4096)) == (4, -12), "arrived at Riverwood's map marker")
