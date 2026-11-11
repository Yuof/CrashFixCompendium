# ShoutEmptyWord

You press the unlock key on a shout in the Magic menu and the game closes. The crash log says `SkyrimSE.exe+093BA03`, `mov rax, [r14]` with R14 = 0, in `MagicMenu::ProcessMessage`.

It only happens on a shout with fewer than three words, once you know one of its words. In practice that means a mod's shout.

## How to reproduce it
1. Console: `player.addshout E40C4`, then `player.teachword E40C5`. That is Battle Cry, a one-word shout.
2. Open the Magic menu, select Battle Cry and press the unlock key (R).

The game crashes on the key press, every time. With the fix you get the message box "Dragon Souls are required to unlock shouts." instead. A mod's one-word shout behaves the same; I used Dragon Avatar's "Dragon Breath: FIRE".

Skip step 1's `teachword` and nothing happens at all. The short shout alone is harmless. You have to know a word.

Battle Cry serves as a test case only. It is a racial power, and you can't learn its word in normal play.

Script: [repro/shoutunlock.py](repro/shoutunlock.py).

## Where I met it
A player reported it on the mod page: "hitting unlock on a shout with less than 3 programmed words causes the game to crash". Their log had the address above, R14 = 0, R12 = 2 and the word "Suum" from Dragon Avatar in RCX. Every short shout in my own load order is a racial power, a howl or an NPC's shout.

## Why it crashes
A shout has three word slots. The game has a small helper that answers "what is the last word of this shout the player knows?" as a slot number. The Magic menu asks it, then walks the slots from 0 to that number and calls each word.

The helper walks the slots until it finds a word the player doesn't know. It skips an empty slot the same way it skips a known word, and that is the bug. For a shout that is [known word, empty, empty] it runs off the end and answers 2. The menu then calls the "word" in slot 1, which is a null pointer.

R12 = 2 in the crash log is that answer, still in a register.

## The code, with the fix in
The fix replaces the helper. Its first 16 bytes become a jump to my function, so none of the game's helper runs any more. Here is the helper as the game ships it (decompiled):

```c
// TESShout, SkyrimSE+380CF0 (Address Library 23357)
int TESShout::LastKnownWord(TESShout* shout)
{
    int        i = 0;
    Variation* slot = &shout->variations[0];           // shout + 0x60, three slots of 0x18 bytes

    while (slot->word == nullptr                       // BUG: an empty slot counts as known
           || slot->word->GetKnown())                  // vfunc 0xB8
    {
        ++i;
        ++slot;
        if (i > 2) {
            break;
        }
    }
    return i - 1;                                      // [known, empty, empty] -> 2
}
```

And what runs in its place (`src/fixes/ShoutEmptyWord.cpp`):

```cpp
std::int32_t LastKnownWord(const RE::TESShout* a_shout)
{
    const auto& slots = a_shout->variations;

    // new: the shout ends at its last real word
    std::int32_t count = RE::TESShout::VariationIDs::kTotal;
    while (count > 0 && !slots[count - 1].word) {
        --count;
    }

    // the game's walk, but only up to there
    std::int32_t i = 0;
    while (i < count && (!slots[i].word || slots[i].word->GetKnown())) {
        ++i;
    }
    return i - 1;                                      // [known, empty, empty] -> 0
}
```

The caller that crashed, unchanged. Read it with the helper's old and new answer in mind:

```c
// MagicMenu::ProcessMessage, SkyrimSE+93B680 (Address Library 52028), the unlock key's branch
int last = shout->LastKnownWord();                     // 93B9DD
bool canUnlock = false;
if (last >= 0) {
    Variation* slot = &shout->variations[0];
    for (int n = last + 1; n != 0; --n) {
        TESWordOfPower* word = slot->word;
        if (word->GetKnown()                           // 93BA03: word is null in an empty slot
            && (word->formFlags & 0x10000) == 0) {
            canUnlock = true;
        }
        ++slot;
    }
    ...                                                // no souls: the message box; else the unlock prompt
}
```

## Why the fix looks like this
The menu's loop is fine if the helper returns the right number, so I fixed the helper. That also covers two more places in the Magic menu with the same loop: the unlock prompt and the code that spends the dragon soul.

My helper still skips an empty slot in front of a word. The game has shouts built like that ([empty, word, word], one of the dragons' frost breaths), and they must keep their answer.

The helper has eight callers. The other five:

- Shout use skips empty words on its own and gets the same result.
- Two callers that teach a word only test the answer against -1.
- The item card reads the spell of the answered slot. For a one-word shout it now shows the real spell's slot instead of the empty third one. On Dragon Avatar's shout the card looks the same with the fix on and off.

Install compares the helper's first 48 bytes with what I expect and leaves the game alone if they differ, for instance when another plugin already hooks it.

## What else I considered
The smallest possible patch is one byte: retarget a jump so the helper stops at any empty slot. It fixes the crash and breaks the shouts with an empty slot in front, which then get a different answer than the game gives them.

A hook that calls the original helper and corrects its result would work, but there is nothing in the original worth keeping. Its answer is the bug.

Null checks in the menu's loops would mean three patches in three places for one wrong number.
