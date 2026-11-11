def uisound(p):
    """AudioFormat. Before the launch, replace a sound file with a broken copy from mkwav.py. Then play the sound
    record that uses it, plays=<n> times (default 5) at the player.

    sound=<SOUN id>: default 0x137E7, UIMenuOK, file Sound\\fx\\ui\\ui_menu_ok.wav. That sound has priority 128 and
    opens its file on the queued path. For the async path use a sound with priority 64 or lower, for instance a door
    sound."""
    snd, n = p.get("sound", "0x137E7"), int(p.get("plays", 5))
    ids = []
    for _ in range(n):
        ids.append(g.call("Sound", "Play", {"form": "0x14"}, on=snd))
        time.sleep(1.5)
    check("sounds played", len(ids) == n, f"Sound.Play results {ids}")
