"""AudioFormat: test copies of a short sound (a 0.25 s 880 Hz beep, 44.1 kHz) with broken headers.
  control.wav    16-bit PCM mono: plays
  zeroch.wav     16-bit PCM, header says 0 channels
  float.wav      32-bit float mono (format tag 3): the game's header parser rejects it
  zeroblock.wav  16-bit PCM mono, header says 0 bytes per sample frame
Run: py mkwav.py <out dir>"""
import math, struct, sys
from pathlib import Path

RATE, N = 44100, 11025


def wav(tag, ch, bits, data, block=None):
    block = block if block is not None else max(ch, 1) * bits // 8
    fmt = struct.pack("<HHIIHH", tag, ch, RATE, RATE * block, block, bits)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body


if __name__ == "__main__":
    out = Path(sys.argv[1]); out.mkdir(exist_ok=True)
    tone = [0.4 * math.sin(2 * math.pi * 880 * i / RATE) for i in range(N)]
    pcm = b"".join(struct.pack("<h", int(s * 32767)) for s in tone)
    flt = b"".join(struct.pack("<f", s) for s in tone)
    files = {"control.wav": wav(1, 1, 16, pcm), "zeroch.wav": wav(1, 0, 16, pcm, block=2),
             "float.wav": wav(3, 1, 32, flt), "zeroblock.wav": wav(1, 1, 16, pcm, block=0)}
    for name, data in files.items():
        (out / name).write_bytes(data)
        print(f"{out / name}: {len(data)} bytes")
