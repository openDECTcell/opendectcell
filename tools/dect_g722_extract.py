#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Extract the G.722 bearer stream from a dect-stubd capture.

A capture written by `dect-stubd -w FILE` holds every TDM slot of every
frame: `slots` 16-bit little-endian words per 8 kHz frame.  On the 7520 the
chip puts a connected call's audio on the word that the bound PCM port
drives -- raw DMA word 0 for port 6 -- and what it puts there is **not**
decoded PCM.  It is the coded bearer: a new 16-bit word every *two* frames,
so 4000 distinct words per second = 8000 bytes/s = 64 kbit/s, which is
G.722's rate and the long slot's B-field rate (observed 2026-10-19; the
duplication shows up as byte i == byte i-2 for 51 % of bytes and essentially
nothing at other lags).

So recovering the audio means taking every other frame and handing the bytes
to a G.722 decoder.  Because the duplication is exact, both phases give the
same stream; `--phase` exists only to check that.

  ./dect_g722_extract.py call.raw -o call.g722
  ffmpeg -f g722 -i call.g722 call.wav

With `--wav` the ffmpeg call is made here, if ffmpeg is on PATH.
"""

import argparse
import shutil
import subprocess
import sys


def extract(data, slots, word, phase, step):
    frame = slots * 2
    out = bytearray()
    for i in range(phase, len(data) // frame, step):
        off = i * frame + word * 2
        out += data[off:off + 2]
    return bytes(out)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("capture", help="raw capture from dect-stubd -w")
    p.add_argument("-o", "--out", help="write the G.722 stream here")
    p.add_argument("-c", "--slots", type=int, default=8,
                   help="slots per frame (default 8)")
    p.add_argument("-w", "--word", type=int, default=0,
                   help="raw DMA word to extract (default 0, the dect1 position)")
    p.add_argument("--phase", type=int, default=0, choices=(0, 1),
                   help="which frame of each duplicated pair to keep")
    p.add_argument("--step", type=int, default=2,
                   help="keep one frame in STEP (default 2; 1 disables de-duplication)")
    p.add_argument("--from-sec", type=float, default=0.0)
    p.add_argument("--to-sec", type=float)
    p.add_argument("--wav", help="also decode to this WAV with ffmpeg")
    a = p.parse_args()

    with open(a.capture, "rb") as f:
        data = f.read()
    frame = a.slots * 2
    lo = int(a.from_sec * 8000) * frame
    hi = int(a.to_sec * 8000) * frame if a.to_sec else len(data)
    stream = extract(data[lo:hi], a.slots, a.word, a.phase, a.step)

    out = a.out or (a.capture.rsplit(".", 1)[0] + ".g722")
    with open(out, "wb") as f:
        f.write(stream)
    print(f"{out}: {len(stream)} bytes = {len(stream) * 8 / 64000:.1f} s at 64 kbit/s")

    if a.wav:
        if not shutil.which("ffmpeg"):
            sys.exit("ffmpeg not found; decode manually: "
                     f"ffmpeg -f g722 -i {out} {a.wav}")
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error",
                        "-f", "g722", "-i", out, a.wav], check=True)
        print(f"{a.wav}: decoded (16 kHz mono)")


if __name__ == "__main__":
    main()
