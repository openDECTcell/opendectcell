#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a verbatim TDM lane carrying G.722 for `dect-stubd -a playraw=`.

The lane convention, measured from the chip's own uplink (v22.raw, 53 s of a
live wideband call -- see docs/journals/call-g7.md, desk session 2026-10-23 part 2):

  * two G.722 code octets per 16-bit word,
  * **each word written into TWO consecutive frames**, 99.8 % of word runs
    being exactly length 2,
  * so 4000 distinct words/s = 8000 octets/s = G.722 at 64 kbit/s, which is
    exactly what the 80-byte long slot the chip reports carries.

Writing a new word every frame -- what we did until now -- feeds the lane at
twice the rate it consumes. A tone survives that (decimation by two leaves a
400 Hz sine at 400 Hz); speech does not, which is why every tone we sent was
recognisable and no speech ever was.

Duplicating each word also makes the result robust against the pairing phase:
whichever parity the chip samples on, it reads each word exactly once.

The octet order within the word is not settled; -s swaps it.

Usage:  tools/make-lane-g722.py [-s] IN.g722 OUT.raw
        IN.g722 is a raw G.722 stream at 64 kbit/s (8000 octets/s).
"""
import array, sys


def main(argv):
    swap = False
    args = [a for a in argv[1:] if a != "-s"]
    if "-s" in argv[1:]:
        swap = True
    if len(args) != 2:
        sys.stderr.write(__doc__)
        return 2
    with open(args[0], "rb") as f:
        src = f.read()
    n = len(src) // 2 * 2
    out = array.array('B')
    for i in range(0, n, 2):
        lo, hi = src[i], src[i + 1]
        if swap:
            lo, hi = hi, lo
        out.append(lo); out.append(hi)     # frame k
        out.append(lo); out.append(hi)     # frame k+1: the chip holds it
    with open(args[1], "wb") as f:
        out.tofile(f)
    print("%s: %d octets -> %d frames, %.2f s at 8000 frames/s%s"
          % (args[1], n, len(out) // 2, (len(out) // 2) / 8000.0,
             " (octets swapped)" if swap else ""))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
