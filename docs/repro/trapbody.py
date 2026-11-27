"""TrapBodyEntry: a trap that touches the player is disabled, or moved with SetPosition.

disable_route(form_id): a trap placed on the player (its collision overlaps the player, so the trap keeps an entry
with the player as its target), then Disable and Delete. Ran with 0x079922, the vanilla small falling rock
(TrapFallingRockSm01, a rigid body on the TRAP layer 14), and 0x073265, the vanilla spike bed (TrapWoodSpikeBed01, a
phantom on the same layer). Papyrus Disable takes the collision out of the world at once; the console's disable does
it only later.

setposition_route(): VIGILANT's falling block (zzzCHTrapFallingBlock01, a rigid body on layer 14) placed on the player,
keyframed, moved 500 units away with SetPosition (the game disables it and enables it at the new place, with a new
body), then Disable and Delete.

Without the fix the trap listener keeps each entry after its collision is gone. With it, CrashFixCompendium.log gets
"[TrapBodyEntry] removed the entry of a trap body / phantom that left the world" and the listener's list is empty
again. trapbody_force.cdb then stands in for the reuse of the freed memory: it crashes the game only where such an
entry is left.
"""
import time


def place_at_player(form_id, plugin):
    base = g.call("Game", "GetFormFromFile", form_id, plugin)["formId"]
    ref = g.call("ObjectReference", "PlaceAtMe", {"form": base}, 1, False, False, on="0x14")["formId"]
    g.until("3D loaded", lambda: g.call("ObjectReference", "Is3DLoaded", on=ref), 5, 0.25)
    return ref


def disable_route(form_id):
    g.console("tgm")
    ref = place_at_player(form_id, "Skyrim.esm")
    time.sleep(3)
    g.call("ObjectReference", "Disable", False, on=ref)
    time.sleep(3)
    g.call("ObjectReference", "Delete", on=ref)
    time.sleep(3)


def setposition_route():
    g.console("tgm")
    ref = place_at_player(0x0AE9D4, "Vigilant.esm")
    g.call("ObjectReference", "SetMotionType", 4, True, on=ref)      # keyframed, so it doesn't roll
    x, y, z = g.scene()["position"]
    g.call("ObjectReference", "SetPosition", x, y + 500.0, z, on=ref)
    time.sleep(3)
    g.call("ObjectReference", "Disable", False, on=ref)
    g.call("ObjectReference", "Delete", on=ref)
    time.sleep(4)
