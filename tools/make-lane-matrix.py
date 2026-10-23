#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a multi-window lane file and the matching `-a seqstep` schedule.

One attended call tests many (chip mode x lane format) combinations. The
daemon's `seq=MS` walks the 0x0230 configurations, MS ms per window, starting
when the call binds -- the same instant `playraw` restarts this file at offset
zero -- so windows built here stay aligned with the modes by construction.

Window 0 carries no re-assign: it records whatever the bind itself produced.
That is deliberate. The bind is not reliable (2026-10-23: vF49 reached
dest 6 = 085d with the enable word 3, vF412 read dest 6 = 0410 and enable 0 at
the same point from identical mails), so every window must be read together
with the route words from `memdump`, which say which state the chip was
really in. A window whose mode did not take is discarded, not scored.

Each window opens with a short run of zeros, which gives the acoustic
recording a clean boundary to align on whatever the format turns out to be.
Every window carries the SAME audio content, so envelopes can be compared
across windows directly.

Usage:  tools/make-lane-matrix.py [--plan format|duplex|wideband] SPEECH.lin SPEECH.g722 OUT.raw
"""
import array, math, sys

SR = 8000
WINDOW_S = 8.0
GAP_S = 0.5
DBFS = -12.0


def alaw_table():
    t = []
    for a in range(256):
        aa = a ^ 0x55
        seg, mant = (aa & 0x70) >> 4, aa & 0x0F
        v = (mant << 4) + 8 if seg == 0 else ((mant << 4) + 0x108) << (seg - 1)
        t.append(v if (aa & 0x80) else -v)
    return t


def encoder(table):
    import bisect
    pairs = sorted((v, i) for i, v in enumerate(table))
    vals = [p[0] for p in pairs]
    idxs = [p[1] for p in pairs]

    def enc(x):
        j = bisect.bisect_left(vals, x)
        if j == 0:
            return idxs[0]
        if j >= len(vals):
            return idxs[-1]
        return idxs[j - 1] if x - vals[j - 1] <= vals[j] - x else idxs[j]
    return enc


enc_alaw = encoder(alaw_table())


def normalise(x, dbfs=DBFS):
    rms = math.sqrt(sum(float(s) * s for s in x) / len(x))
    g = (32767 * 10 ** (dbfs / 20.0)) / rms
    return [max(-32768, min(32767, int(s * g))) for s in x]


# (label, 0x0230 flags:codec for this window or None for "leave the bind alone",
#  how the window's bytes are laid out)
# "format" walks lane formats against chip modes: what does the downlink want?
# "duplex" holds mode 2 and alternates a silent downlink with a speech one, so
# the uplink can be captured clean in the silent windows while the speech
# windows show whether the downlink persists over a long call. One window
# detours through mode 9 and the next returns to mode 2, testing whether the
# configuration recovers after being disturbed.
PLAN_FORMAT = [
    ("bind default          ", None,    "g722_held"),
    ("mode 8 (41:01)        ", "41:01", "g722_held"),
    ("mode 8, one octet/frame", "41:01", "g722_one"),
    ("mode 2 (01:03)        ", "01:03", "alaw_lo"),
    ("mode 2, A-law high    ", "01:03", "alaw_hi"),
    ("mode 2, linear 16-bit ", "01:03", "lin16"),
    ("mode 9 (41:03)        ", "41:03", "g722_held"),
    ("mode 9, A-law low     ", "41:03", "alaw_lo"),
]

PLAN_DUPLEX = [
    ("mode 2 (bind+remode)  ", None,    "silence"),
    ("mode 2, downlink on   ", "01:03", "speech"),
    ("mode 2, uplink clean  ", "01:03", "silence"),
    ("mode 2, downlink on   ", "01:03", "speech"),
    ("mode 9 detour         ", "41:03", "g722_held"),
    ("mode 2 recovery       ", "01:03", "silence"),
    ("mode 2, downlink on   ", "01:03", "speech"),
    ("mode 2, uplink clean  ", "01:03", "silence"),
]

# "wideband" tests the lane packing the coded (transparent) block wants, in
# the only two modes that arm it ([CR A-0043]: modes 8 and 9; the true 2-slot
# classes 6/7 are unreachable by any host mail). The room's leading candidate
# was two octets per frame, but the arithmetic puts that at twice the rate the
# air can take: the 80-byte long-slot B-field is 80 bytes per 10 ms DECT frame
# = 8000 octets/s, and the TDM lane runs at 8000 frames/s, so ONE octet per
# frame is the rate-matched packing -- and it is also the rate we measured on
# the uplink. So `g722_one` leads here and `g722_full` is tested second, not
# first. `g722_held` (word held two frames) is NOT retried: vF413 falsified it.
PLAN_WIDEBAND = [
    ("bind default          ", None,    "g722_one"),
    ("mode 8, one octet/frame", "41:01", "g722_one"),
    ("mode 8, octet in high  ", "41:01", "g722_one_hi"),
    ("mode 8, two octets/frame", "41:01", "g722_full"),
    ("mode 8, one octet, swap", "41:01", "g722_one_swap"),
    ("mode 9, one octet/frame", "41:03", "g722_one"),
    ("mode 9, two octets/frame", "41:03", "g722_full"),
    ("mode 9, octet in high  ", "41:03", "g722_one_hi"),
]

PLANS = {"format": (PLAN_FORMAT, 8.0), "duplex": (PLAN_DUPLEX, 25.0),
         "wideband": (PLAN_WIDEBAND, 10.0)}


def lay(kind, pcm, g722, frames, out):
    """Append `frames` frames of this window's content."""
    if kind == "g722_held":
        # two octets per word, each word written into two frames
        for i in range(0, frames, 2):
            k = i % (len(g722) - 1)   # tiled, so a window may outlast the source
            lo, hi = g722[k], g722[k + 1]
            out.append(lo); out.append(hi)
            out.append(lo); out.append(hi)
    elif kind == "g722_one":
        # one octet per frame, low byte: 8000 octets/s, the long slot's rate
        for i in range(frames):
            out.append(g722[i % len(g722)]); out.append(0)
    elif kind == "g722_one_hi":
        for i in range(frames):
            out.append(0); out.append(g722[i % len(g722)])
    elif kind == "g722_one_swap":
        # the eor #2 pairing [CR A-DSP-7]: adjacent 16-bit words swapped, so
        # the octets reach the chip in the other order of each pair
        for i in range(0, frames, 2):
            a, b = g722[i % len(g722)], g722[(i + 1) % len(g722)]
            out.append(b); out.append(0)
            out.append(a); out.append(0)
    elif kind == "g722_full":
        # two octets per 16-bit word, a new word every frame: 16000 octets/s,
        # twice the long slot's rate. [CR A-0043]'s leading candidate; kept
        # because it costs one window, not because the arithmetic supports it.
        for i in range(frames):
            k = (2 * i) % (len(g722) - 1)
            out.append(g722[k]); out.append(g722[k + 1])
    elif kind == "alaw_lo":
        for i in range(frames):
            out.append(enc_alaw(pcm[i])); out.append(0)
    elif kind == "alaw_hi":
        for i in range(frames):
            out.append(0); out.append(enc_alaw(pcm[i]))
    elif kind == "silence":
        out.extend(b'\0\0' * frames)
    elif kind == "speech":
        for i in range(frames):            # linear, one sample/frame, tiled
            v = pcm[i % len(pcm)] & 0xFFFF
            out.append(v & 0xFF); out.append((v >> 8) & 0xFF)
    elif kind == "lin16":
        for i in range(frames):
            v = pcm[i] & 0xFFFF
            out.append(v & 0xFF); out.append((v >> 8) & 0xFF)
    else:
        raise SystemExit("unknown layout " + kind)


def main(argv):
    plan_name, argv = "format", list(argv)
    if "--plan" in argv:
        i = argv.index("--plan")
        plan_name = argv[i + 1]
        del argv[i:i + 2]
    if len(argv) != 4 or plan_name not in PLANS:
        sys.stderr.write(__doc__)
        return 2
    plan, window_s = PLANS[plan_name]
    pcm = array.array('h')
    pcm.frombytes(open(argv[1], 'rb').read())
    if sys.byteorder == 'big':
        pcm.byteswap()
    g722 = open(argv[2], 'rb').read()

    gap = int(GAP_S * SR)
    body = int(window_s * SR) - gap
    if len(pcm) < SR or len(g722) < SR:
        sys.stderr.write("need at least 1 s of each input\n")
        return 1
    speech = normalise(list(pcm[:min(body, len(pcm))]))

    out = array.array('B')
    steps = []
    print("window  starts  mode            lane format")
    for n, (label, step, kind) in enumerate(plan):
        out.extend(b'\0\0' * gap)
        lay(kind, speech, g722, body, out)
        print("  %d     %5.1f s  %-14s  %s"
              % (n, n * window_s, step or "(bind default)", kind))
        if n:                      # window 0 carries no re-assign
            steps.append(step)
    open(argv[3], 'wb').write(out.tobytes())
    print("\n%s: %d frames, %.1f s, %d windows of %.1f s"
          % (argv[3], len(out) // 2, len(out) / 2 / SR, len(plan), window_s))
    print("\n-a fragment (keep this with the file -- they must match):")
    print("  seq=%d," % int(window_s * 1000)
          + ",".join("seqstep=" + x for x in steps))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
