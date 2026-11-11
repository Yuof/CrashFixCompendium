# Repro scripts

These are the scripts I ran to reproduce the crashes. They are cut out of my test harness, so they don't run on their own: they drive a running game through [devbench](https://github.com/alandtse/devbench), an SKSE plugin that takes commands from outside the game. Read them as exact step lists. Every step is a console command or a Papyrus call you can also make by hand.

The helpers they use:

| Call | What it does |
|---|---|
| `g.console(cmd)`, `g.ask(cmd)` | run a console command (`ask` returns the console's answer) |
| `g.goto(cell)` | `coc <cell>` and wait for the loading screen to end |
| `g.travel(g.con(cmd))` | a console command that changes the cell, then the same wait |
| `g.call(script, function, *args, on=form)` | call a Papyrus function on a form, e.g. `ObjectReference.Disable` |
| `g.tool("scenario", steps=..., repeat=n)` | run a list of steps inside the game, frame-accurate, `n` times |
| `g.tool("inspect", kind="refs", ...)` | list references near the player |
| `g.tool("input", ...)`, `g.tool("menu", ...)`, `g.tool("gfx", ...)` | press a key, open a menu, read or call into a menu's Flash movie |
| `g.wait_hours(h)` | the wait menu, `h` hours |
| `g.spawn(base)` | `PlaceAtMe` of an actor base at the player |
| `walk(seconds)` | hold the forward key |
| `check(name, ok, detail)` | record a step's result; a failed check ends the run |

`0x14` is the player, `0x3B` the X marker, `0x38` the GameHour global, `0x1C789` Firebolt.

| File | Fix |
|---|---|
| [treedecal.py](treedecal.py), [treedecal_widen.cpp](treedecal_widen.cpp) | SkinnedDecal |
| [walkway.py](walkway.py) | DecalVertexTable |
| [swingdisable.py](swingdisable.py) | FreeCamera, DisabledAttacker |
| [trespass.py](trespass.py), [trespass_force.cpp](trespass_force.cpp) | TrespassPursue |
| [gatesweep.py](gatesweep.py) | FootIkRaycast |
| [shoutunlock.py](shoutunlock.py) | ShoutEmptyWord |
| [mknif.py](mknif.py), [nifstring.py](nifstring.py) | NifStringIndex |
| [longwait.py](longwait.py) | UIMessageQueue |
| [mkwav.py](mkwav.py), [uisound.py](uisound.py) | AudioFormat |
| [LateHookRaceProbe.cpp](LateHookRaceProbe.cpp) | StartupHook |

The `.cpp` files are test plugins, or the part of one that matters.
