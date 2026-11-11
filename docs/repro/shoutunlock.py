def shoutunlock(p):
    """ShoutEmptyWord. Gives the player a one-word shout and its word, opens the Magic menu, selects the shout and
    presses the unlock key (R). Without the fix the game dies on the key press. With it the handler ends in a
    message box, which is the "unlock handled" check.

    shout=<id> word=<id>: default Battle Cry 0xE40C4 and its word 0xE40C5.
    plugin=<file>: the two ids are that plugin's own form ids (for a mod's shout).
    teach=0: the word is NOT taught. That is the control: a one-word shout alone does nothing."""
    shout, word = int(p.get("shout", "0xE40C4"), 16), int(p.get("word", "0xE40C5"), 16)
    if p.get("plugin"):
        look = lambda i: int(str(g.call("Game", "GetFormFromFile", i, p["plugin"])["formId"]), 16)
        shout, word = look(shout), look(word)
    teach = p.get("teach", "1") != "0"

    g.ask(f"player.addshout {shout:X}")
    if teach: g.ask(f"player.teachword {word:X}")

    g.tool("menu", action="open", name="MagicMenu")
    g.until("MagicMenu open", lambda: "MagicMenu" in g.menus(), 10, 0.5)
    time.sleep(2)

    # find the shout's row in the menu's list and select it (SkyUI's list; the vanilla menu has other paths)
    lst = "_root.Menu_mc.inventoryLists.itemList"
    get = lambda path: g.tool("gfx", op="get", menu="MagicMenu", path=path).get("value")
    n = int(get(lst + "._entryList.length") or 0)
    idx = next((i for i in range(n) if get(f"{lst}._entryList.{i}.formId") == shout), None)
    check("shout listed", idx is not None, f"{n} entries, shout {shout:X} at {idx}")
    g.tool("gfx", op="call", menu="MagicMenu", path=lst + ".doSetSelectedIndex", args=[idx, 0]); time.sleep(0.5)
    sel = get(lst + ".selectedEntry.formId")
    check("shout selected", sel == shout)

    g.tool("input", action="tap", device="keyboard", key="r", durationMs=120, owner="ingame")   # unfixed: dies here
    time.sleep(3)
    box = g.tool("menu", action="list").get("messageBoxOpen") is True
    text = g.tool("gfx", op="get", menu="MessageBoxMenu", path="_root.MessageMenu.MessageText.text").get("value") if box else None
    check("unlock handled", box is teach, f"message box {box} {text!r}")
    for m in ("MessageBoxMenu", "MagicMenu"):
        g.tool("menu", action="close", name=m); time.sleep(1)


# On Dragon Avatar's one-word shout I ran it as: plugin=feraldragonavatar.esl shout=0x898 word=0x899
