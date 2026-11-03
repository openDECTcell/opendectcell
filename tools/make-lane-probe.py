#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a self-identifying downlink lane-format probe for `dect-stubd -a playraw=`.

One call tests four candidate formats for the TDM bearer lane. Each speech
segment is preceded by N short beeps, so the listener can name the segment
that was intelligible without tracking elapsed time, and a dropped call costs
only the tail.

Why beeps work as markers under every hypothesis: a linear sine misread as
A-law or mu-law still peaks at its own frequency (it loses purity, not pitch
-- measured, see docs/journals/call-g7.md "Desk session, 2026-10-23 morning"). So the
markers stay audible whatever the lane turns out to be, and they double as the
liveness control that every audio test on this path needs.

Why speech and not a tone: a tone proves pitch and nothing else, because every
candidate format reproduces a sine's fundamental. Only speech separates them.

Usage:  tools/make-lane-probe.py SPEECH.lin OUT.raw
        SPEECH.lin is signed 16-bit LE mono at 8000 Sa/s, any length >= 8 s.
"""
import array, math, sys

SR = 8000
SEG_S = 8.0            # seconds of speech per format
SPEECH_DBFS = -12.0    # docs/tasks-2026-10-23.md A3: play at a sane level


def alaw_decode_table():
    t = []
    for a in range(256):
        aa = a ^ 0x55
        seg, mant = (aa & 0x70) >> 4, aa & 0x0F
        v = (mant << 4) + 8 if seg == 0 else ((mant << 4) + 0x108) << (seg - 1)
        t.append(v if (aa & 0x80) else -v)
    return t


def ulaw_decode_table():
    t = []
    for u in range(256):
        uu = (~u) & 0xFF
        seg, mant = (uu & 0x70) >> 4, uu & 0x0F
        v = ((((mant << 1) + 33) << seg) - 33) << 2
        t.append(-v if (uu & 0x80) else v)
    return t


def encoder(table):
    """Nearest-value encoder built from a decode table: small, and obviously
    the exact inverse of the decoder the analysis scripts use."""
    pairs = sorted((v, i) for i, v in enumerate(table))
    vals = [p[0] for p in pairs]
    idxs = [p[1] for p in pairs]
    import bisect

    def enc(x):
        j = bisect.bisect_left(vals, x)
        if j == 0:
            return idxs[0]
        if j >= len(vals):
            return idxs[-1]
        return idxs[j - 1] if x - vals[j - 1] <= vals[j] - x else idxs[j]

    return enc


AL, UL = alaw_decode_table(), ulaw_decode_table()
enc_alaw, enc_ulaw = encoder(AL), encoder(UL)


def tone(freq, secs, dbfs=SPEECH_DBFS):
    amp = 32767 * (10 ** (dbfs / 20.0)) * math.sqrt(2)
    return [int(amp * math.sin(2 * math.pi * freq * n / SR))
            for n in range(int(secs * SR))]


def normalise(samples, dbfs=SPEECH_DBFS):
    rms = math.sqrt(sum(float(s) * s for s in samples) / len(samples))
    g = (32767 * (10 ** (dbfs / 20.0))) / rms
    return [max(-32768, min(32767, int(s * g))) for s in samples]


def lane_linear(samples, out):
    for s in samples:
        u = s & 0xFFFF
        out.append(u & 0xFF)
        out.append((u >> 8) & 0xFF)


def lane_law(samples, enc, high, out):
    for s in samples:
        b = enc(s)
        out.append(0 if high else b)
        out.append(b if high else 0)


FORMATS = [
    ("linear 16-bit LE", lambda s, o: lane_linear(s, o)),
    ("A-law, low byte",  lambda s, o: lane_law(s, enc_alaw, False, o)),
    ("A-law, high byte", lambda s, o: lane_law(s, enc_alaw, True, o)),
    ("mu-law, low byte", lambda s, o: lane_law(s, enc_ulaw, False, o)),
]


def main(argv):
    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    src = array.array('h')
    with open(argv[1], 'rb') as f:
        src.frombytes(f.read())
    if sys.byteorder == 'big':
        src.byteswap()
    need = int(SEG_S * SR)
    if len(src) < need:
        sys.stderr.write("need at least %.1f s of speech, got %.1f s\n"
                         % (SEG_S, len(src) / SR))
        return 1
    speech = normalise(list(src[:need]))

    beep, gap = tone(1000, 0.15), [0] * int(0.25 * SR)
    out = array.array('B')
    print("cycle:")
    for n, (name, lay) in enumerate(FORMATS, start=1):
        marker = []
        for _ in range(n):
            marker += beep + gap
        marker += [0] * int(0.5 * SR)
        lane_linear(marker, out)          # markers are always linear
        lay(speech, out)
        print("  %d beep(s) -> %-18s  %.1f s marker + %.1f s speech"
              % (n, name, len(marker) / SR, SEG_S))
    with open(argv[2], 'wb') as f:
        out.tofile(f)
    print("\n%s: %d frames, %.1f s per cycle, %d bytes"
          % (argv[2], len(out) // 2, len(out) / 2 / SR, len(out)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
