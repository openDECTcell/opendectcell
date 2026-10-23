#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Decode a beacon dump (F4.29) from a dect-stubd log.

The question this answers: **does our base announce wideband / CAT-iq, or
only basic GAP?** We have no over-air capture — no SDR, no sniffer — so we
read the assembled beacon out of chip RAM instead, which is better anyway:
no demodulation and no capture errors.

Map from [CR A-0073], static-derived and therefore to be CONFIRMED by the
first dump as much as read:

  0x2269  live beacon base; the first 0x37 bytes are header and identity
  0x22a0  rotating A-field tail slots, stride 8, roughly 12 of them
          a slot is ACTIVE when its marker byte is 0xe0
  0x2459  FP_CONFIG broadcast buffer (what 0x022B stages)
  0x2477  the active length of that buffer

Identity material lives at 0x2285/0x228a/0x228f/0x2294. This script does not
print it: it is our chip's RFPI and must not leave the device. Pass --identity
only if you are looking at it locally and know why.

The 0x2477 length is derived from the MAIL's payload-length field
(payload_len-2, clamped to 30), not from capability bytes -- [CR A-0074] --
and it is written by exactly two paths: 0x022B sets it, 0x022C clears it. So
a bare 0x022B(0x41) can itself arm a nonzero length. F4.29 and F4.29b differ
by exactly that mail, which is what makes a nonzero attributable.

Usage:  tools/decode-beacon.py vF429.log [--identity]
        tools/decode-beacon.py vF429.log --diff vF429b.log
"""
import collections
import re
import sys

BASE = 0x2269
SLOTS = 0x22A0
SLOT_STRIDE = 8
SLOT_COUNT = 13
FP_CONFIG = 0x2459
FP_CONFIG_LEN = 0x2477
IDENTITY = (0x2285, 0x228A, 0x228F, 0x2294)
ACTIVE = 0xE0


def read_dumps(path):
    """Every 0xFFF1 sub-0x08 reply, as {address: bytes}, newest last."""
    out = []
    pat = re.compile(r'rx\s+src 23 dst 22 prim fff1 len \d+ 08 ([0-9a-f ]+)')
    for line in open(path, errors='replace'):
        m = pat.search(line)
        if not m:
            continue
        b = [int(x, 16) for x in m.group(1).split()]
        if len(b) < 6:
            continue
        addr = b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24
        n = b[4] | b[5] << 8
        out.append((addr, bytes(b[6:6 + n])))
    return out


def assemble(chunks):
    """Merge the chunks into one address->byte map per sweep."""
    mem = {}
    for addr, data in chunks:
        for i, v in enumerate(data):
            mem[addr + i] = v
    return mem


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    show_identity = "--identity" in argv
    chunks = read_dumps(argv[1])
    if not chunks:
        print("no 0xFFF1 memory replies in that log")
        return 1
    mem = assemble(chunks)
    lo, hi = min(mem), max(mem)
    print("read %d chunks covering 0x%05x..0x%05x (%d bytes)"
          % (len(chunks), lo, hi, len(mem)))

    missing = [a for a in range(BASE, 0x2500) if a not in mem]
    if missing:
        print("WARNING: %d bytes of 0x2269..0x2500 were not read "
              "(first gap 0x%05x). The read window may top out lower; "
              "take a second dump for the tail." % (len(missing), missing[0]))

    def get(a, n):
        if any(a + i not in mem for i in range(n)):
            return None
        return bytes(mem[a + i] for i in range(n))

    print("\n--- A-field tail slots (active = marker 0xe0) ---")
    active = 0
    for i in range(SLOT_COUNT):
        a = SLOTS + i * SLOT_STRIDE
        s = get(a, SLOT_STRIDE)
        if s is None:
            print("  slot %-2d 0x%05x  (not read)" % (i, a))
            continue
        mark = "ACTIVE" if s[0] == ACTIVE else "      "
        if s[0] == ACTIVE:
            active += 1
        print("  slot %-2d 0x%05x  %s  %s" % (i, a, mark, s.hex(" ")))
    if active == 0:
        print("\n  NO ACTIVE SLOTS. Either the map is wrong for this build or "
              "the builder had not filled them. Send the room these bytes "
              "rather than guessing -- one wrong address is cheap to fix from "
              "a real dump.")

    print("\n--- FP_CONFIG broadcast buffer ---")
    ln = get(FP_CONFIG_LEN, 1)
    if ln is None:
        print("  length at 0x%05x not read" % FP_CONFIG_LEN)
    else:
        n = ln[0]
        print("  length byte at 0x%05x = %d" % (FP_CONFIG_LEN, n))
        if n == 0:
            print("  EMPTY -- we stage no broadcast config, so we announce "
                  "only the chip's built-in defaults. Consistent with "
                  "[CR A-0066].")
        elif n > 0x80:
            print("  not a sane length; treat the map as unconfirmed")
        else:
            body = get(FP_CONFIG, n)
            print("  body: %s" % (body.hex(" ") if body else "(not read)"))

    for a in (0x2431, 0x244E, 0x24F0):
        s = get(a, 16)
        print("  config @0x%05x: %s" % (a, s.hex(" ") if s else "(not read)"))

    if show_identity:
        for a in IDENTITY:
            s = get(a, 5)
            print("  identity @0x%05x: %s" % (a, s.hex(" ") if s else "-"))
    else:
        print("\n(identity region not printed; pass --identity if you need it "
              "locally -- it must never be committed or sent to the room)")

    if "--diff" in argv:
        other = argv[argv.index("--diff") + 1]
        omem = assemble(read_dumps(other))
        print("\n--- diff against %s (the 0x022B A/B) ---" % other)
        both = sorted(set(mem) & set(omem))
        diff = [a for a in both if mem[a] != omem[a]]
        print("  %d of %d shared bytes differ" % (len(diff), len(both)))
        for a in diff[:40]:
            where = ""
            if SLOTS <= a < SLOTS + SLOT_COUNT * SLOT_STRIDE:
                where = "  slot %d" % ((a - SLOTS) // SLOT_STRIDE)
            elif a == FP_CONFIG_LEN:
                where = "  <- FP_CONFIG length"
            elif FP_CONFIG <= a < FP_CONFIG + 0x20:
                where = "  <- FP_CONFIG body"
            print("    0x%05x  %02x -> %02x%s" % (a, mem[a], omem[a], where))
        if len(diff) > 40:
            print("    ... and %d more" % (len(diff) - 40))
        if not diff:
            print("  IDENTICAL -- 0x022B changes nothing in this region, so "
                  "whatever is announced is the chip's default and our "
                  "bring-up adds nothing to it.")

    print("\n--- byte stability across sweeps ---")
    per = collections.defaultdict(set)
    for addr, data in chunks:
        for i, v in enumerate(data):
            per[addr + i].add(v)
    churn = sorted(a for a in per if len(per[a]) > 1)
    print("  %d of %d bytes changed during the run" % (len(churn), len(per)))
    if churn:
        print("  changing: " + ", ".join("0x%05x" % a for a in churn[:24])
              + (" ..." if len(churn) > 24 else ""))
        print("  (the slots rotate across the multiframe; the identity "
              "region should be constant)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
