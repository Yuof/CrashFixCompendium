# AudioFormat

A sound starts and the game crashes on the audio thread. The log says `XAudio2_7.dll+25181`, `mov ecx, [rbp]`, with `BSXAudio2DataSrc`, `BSXAudio2GameSound` and `BSAudioManagerThread` among the objects. A second form of the same problem is a divide by zero at `SkyrimSE.exe+0CC2080`, `div ecx`.

The sound file behind it is one the game can't play: a 32-bit float or ADPCM .wav, an .mp3 or .ogg renamed to .wav, a file cut off in the middle, or a file whose header simply says 0 channels.

## How to reproduce it
1. `py mkwav.py <out dir>` ([repro/mkwav.py](repro/mkwav.py)) writes four copies of a short beep: a good one, one whose header says 0 channels, one in 32-bit float, and one whose header says 0 bytes per sample frame.
2. Put one in place of a sound file the game plays, for example `Sound\fx\ui\ui_menu_ok.wav`, and start the game.
3. Play the sound. [repro/uisound.py](repro/uisound.py) plays it from a script.

The 0-channel copy crashes at `XAudio2_7.dll+25181` on every launch. The 0-bytes-per-frame copy crashes at `SkyrimSE.exe+0CC2080`. The float copy depends on how the game opens the sound: a sound with priority 64 or lower opens asynchronously and crashes, while a higher-priority sound such as the menu click takes another path that rejects the file. The good copy plays.

With the fix on, none of them crashes. The sound stays silent and the log says why:

```
[AudioFormat] a sound was not played: its file header says 0 channels (sound id 27300EAC/C007978D; use AudioFormatScan=1 to scan the file) (1 so far)
```

## Where I met it
In a player's crash report. Their log had the address above and a `LooseFileAsyncBase` functor among the objects, which points at a loose sound file opened asynchronously. My own load order has no such file among its 20,000 loose sounds.

## Why it crashes
To play a sound, the game creates an XAudio2 voice and tells XAudio2 the sound's format: channels, sample rate and so on. It takes the format from the header of the sound file.

XAudio2 2.7 does not validate the channel count. It keeps its mixing buffers in a table indexed by `channels - 1`, and with 0 channels that index wraps to 4 billion. The read lands terabytes past the table.

The game can end up with 0 channels in two ways. The plain one is a header that says 0, which nothing checks. The other is a header the game can't read at all. Its parser accepts PCM, xWMA and two other format tags and rejects everything else. On the synchronous path a rejected header ends the open with an error. On the asynchronous path the open reports success anyway, and the format keeps whatever the freshly allocated memory held, which is usually zeros.

Before it creates the voice, the game also computes the sound's length by dividing the data size by the bytes per sample frame (PCM) or by the bits per sample (xWMA). A header that says 0 there crashes in that division, before the game calls XAudio2.

## The code, with the fix in
The fix changes no code in the game. It replaces one entry in the vtable of `BSXAudio2GameSound`, so the game calls my function, and mine calls the game's.

My function (`src/fixes/AudioFormat.cpp`):

```cpp
// empty when XAudio2 can take the sound
std::string_view WhyUnplayable(const RE::BSXAudio2DataSrc* a_data)
{
    if (!a_data) {
        return "it has no data";
    }
    if (!(a_data->unk78 & kHeaderRead)) {              // the parser sets this bit when it found a format and data
        return "the game could not read its file header";
    }
    const auto& format = a_data->format;
    if (format.nChannels == 0) {                       // the XAudio2 crash
        return "its file header says 0 channels";
    }
    if (format.wFormatTag == kPcm && format.nBlockAlign == 0) {         // the two divisions
        return "its file header says 0 bytes per sample frame";
    }
    if (format.wFormatTag == kXwma && format.wBitsPerSample == 0) {
        return "its file header says 0 bits per sample";
    }
    return {};
}

// takes the place of vtable slot 0xB, Prepare
bool PrepareThunk(RE::BSXAudio2GameSound* a_sound)
{
    const auto why = WhyUnplayable(a_sound->src);
    if (why.empty()) {
        return g_prepare(a_sound);                     // a playable sound: the game's Prepare
    }
    ...                                                // log the 1st, 10th, 100th ... silenced sound
    return g_openDone(a_sound, false);                 // fail the open the way the game does for a missing file
}
```

The game's `Prepare`, unchanged (decompiled):

```c
// SkyrimSE+CBF610 (Address Library 68003)
bool BSXAudio2GameSound::Prepare(BSXAudio2GameSound* sound)
{
    BSXAudio2DataSrc* data = sound->src;                           // sound + 0xD0

    FUN_140cc2030(data);                                           // the sound's length. Divides by
                                                                   // format.nBlockAlign (PCM) or by the bits
                                                                   // per sample (xWMA): CC2080 with a 0 there
    sound->field_0x60 = BSXAudio2DataSrc::sub(data);

    sound->channels = data->format.nChannels;                      // sound + 0x140, from data + 0x22.
                                                                   // BUG: used as it is, 0 included
    sound->field_0x144 = audio->field_0x74;
    sound->buffer = Allocate(...);                                 // sound + 0x130
    sound->field_0x1A8 = sound->channels;

    sound->voice = BSXAudio2Audio::sub(audio, ...);                // sound + 0x128: creates the XAudio2 voice.
                                                                   // With 0 channels XAudio2 crashes in here.
    if (sound->voice == nullptr) {
        OpenDone(sound, false);                                    // SkyrimSE+CBB250: mark the open as failed
        return false;
    }
    ...
    sound->flags |= 0x900;                                         // sound + 0x5C
    return true;
}
```

With the fix, a sound with such a format never reaches this function.

Install checks that the vtable slot still holds the game's `Prepare` and that `OpenDone` is the code it expects. If either differs, it hooks nothing.

## Why the fix looks like this
`Prepare` is the last step before XAudio2, and every way of opening a sound goes through it. A check in front of it catches a bad format wherever it came from: a header that says 0, a header the parser rejected, a format that was never filled in.

A refused sound fails through the game's own function for a failed open. To the rest of the game it looks like a missing file, which the game has always handled: the sound stays silent and the rest of the game is unaffected.

The log gives the sound's id, a pair of hashes of its folder and file name. A hash doesn't name the file, so there is a second switch for that. `AudioFormatScan=1`, off by default and not a fix, reads the header of every sound file under Data, loose and in uncompressed .bsa archives, with the same rules as the game's parser. It lists each file the game can't play, together with the id that `AudioFormat` logs. Run it once after an `AudioFormat` line appears, then turn it off again.

## What else I considered
Making the asynchronous open report its failure, as the synchronous one does, repairs the unreadable-header case at its source. It does nothing for a header that reads fine and says 0 channels, so the check in front of `Prepare` would still be needed.

A check in the header parser is incomplete in the other direction: it can reject 0 channels, and the asynchronous path would ignore the rejection.

Validating inside XAudio2 is not possible. XAudio2 2.7 is a system DLL.
