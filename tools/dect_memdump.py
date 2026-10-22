#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reassemble and diff chip-memory snapshots taken with `dect-stubd -a memdump`.

The chip answers the read-address debug mail with `0xFFF1` sub `0x08`, whose
payload is the echoed address, the (capped) byte count and the bytes.  The
daemon logs every mail, so a run that sweeps the configured ranges repeatedly
leaves a series of snapshots in its log: the early ones taken while the link is
idle, the later ones while a call is up.

Diffing our own idle snapshot against our own active one is the measurement
that matters.  It needs no reference values from anyone else, and it controls
for differences in memory layout between chip firmware builds, which absolute
byte comparisons across boxes do not.

  ./dect_memdump.py run.log --list
  ./dect_memdump.py run.log --diff 1 5        # sweep 1 (idle) vs sweep 5 (live)
  ./dect_memdump.py run.log --dump 5 --addr 0x177e0 --len 32
"""
import argparse
import re
import sys

LINE = re.compile(
    r"\[\s*\d+\]\s+rx\s+src\s+\w+\s+dst\s+\w+\s+prim\s+fff1\s+len\s+\d+\s+(.*)",
    re.I)
SWEEP = re.compile(r"mem: sweep (\d+) complete .*?\((idle|call bound)\)")


def parse(path):
    """Return [(sweep_index, state, {addr: byte})] in file order."""
    sweeps, cur = [], {}
    state = "idle"
    for line in open(path, errors="replace"):
        m = SWEEP.search(line)
        if m:
            sweeps.append((int(m.group(1)), m.group(2), cur))
            cur = {}
            continue
        m = LINE.match(line.strip())
        if not m:
            continue
        try:
            b = [int(x, 16) for x in m.group(1).split() if x != "..."]
        except ValueError:
            continue
        # sub 0x08 reply: 08, addr LE32, count LE16, data
        if len(b) < 7 or b[0] != 0x08:
            continue
        addr = b[1] | (b[2] << 8) | (b[3] << 16) | (b[4] << 24)
        count = b[5] | (b[6] << 8)
        data = b[7:7 + count]
        for i, v in enumerate(data):
            cur[addr + i] = v
    if cur:
        sweeps.append((len(sweeps) + 1, state, cur))
    return sweeps


def fmt(addr, a, b=None):
    if b is None:
        return f"  {addr:#08x}: {a:02x}"
    return f"  {addr:#08x}: {a:02x} -> {b:02x}"



def report_changed(sweeps, args):
    """Every address that ever takes more than one value.

    Diffing two sweeps answers "did these two differ". The question that
    matters is "did anything move at all, ever", because a run in which
    nothing moves is a run that measured nothing -- which is what every
    sweep taken before 2026-10-22 did, and it was not obvious from a diff
    of two arbitrary sweeps.
    """
    seen = {}
    for i, (_n, state, d) in enumerate(sweeps, 1):
        for a, v in d.items():
            seen.setdefault(a, {}).setdefault(v, []).append((i, state))

    if args.addr is not None:
        lo, hi = args.addr, args.addr + args.len
        seen = {a: v for a, v in seen.items() if lo <= a < hi}

    changed = sorted(a for a, vals in seen.items() if len(vals) > 1)
    total = len(seen)
    print(f"{len(sweeps)} sweeps, {total} addresses covered, "
          f"{len(changed)} ever changed")
    if not changed:
        print("  NOTHING MOVED. Either the chip is not touching these "
              "addresses, or\n  the capture contains no live media. Check "
              "that a call was up and that\n  audio was actually flowing "
              "before reading anything into this.")
        return 0

    runs, i = [], 0
    while i < len(changed):
        j = i
        while j + 1 < len(changed) and changed[j + 1] == changed[j] + 1:
            j += 1
        runs.append((changed[i], changed[j]))
        i = j + 1

    for lo, hi in runs:
        print(f"  {lo:#08x}..{hi:#08x} ({hi - lo + 1}B)")
        for a in range(lo, min(hi + 1, lo + 8)):
            vals = seen[a]
            bits = []
            for v, where in sorted(vals.items()):
                states = {st for _i, st in where}
                bits.append(f"{v:02x}x{len(where)}"
                            f"[{'/'.join(sorted(states))}]")
            print(f"    {a:#08x}: " + " ".join(bits))
        if hi - lo + 1 > 8:
            print(f"    ... {hi - lo + 1 - 8} more")
    return 0


def main():
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("log", help="dect-stubd log containing the sweeps")
    p.add_argument("--list", action="store_true",
                   help="list the sweeps found and their coverage")
    p.add_argument("--diff", nargs=2, type=int, metavar=("A", "B"),
                   help="diff sweep A against sweep B (1-based)")
    p.add_argument("--dump", type=int, metavar="N", help="print sweep N")
    p.add_argument("--changed", action="store_true",
                   help="report every address that ever takes more than one "
                        "value, across all sweeps, split idle vs call-bound")
    p.add_argument("--addr", type=lambda x: int(x, 0), help="restrict to this address")
    p.add_argument("--len", type=lambda x: int(x, 0), default=32,
                   help="length for --addr (default 32)")
    args = p.parse_args()

    sweeps = parse(args.log)
    if not sweeps:
        print("no chip-memory replies found in that log", file=sys.stderr)
        return 1

    if args.changed:
        return report_changed(sweeps, args)

    if args.list or not (args.diff or args.dump):
        print(f"{len(sweeps)} sweeps")
        for i, (n, state, d) in enumerate(sweeps, 1):
            if d:
                print(f"  sweep {i:2d} [{state:10s}] {len(d):5d} bytes, "
                      f"{min(d):#08x}..{max(d):#08x}")
            else:
                print(f"  sweep {i:2d} [{state:10s}] empty")
        return 0

    def pick(n):
        if not 1 <= n <= len(sweeps):
            sys.exit(f"sweep {n} out of range (1..{len(sweeps)})")
        return sweeps[n - 1]

    if args.dump:
        n, state, d = pick(args.dump)
        lo = args.addr if args.addr is not None else min(d)
        hi = lo + args.len if args.addr is not None else max(d) + 1
        print(f"sweep {args.dump} [{state}]")
        for a in range(lo, hi):
            if a in d:
                print(fmt(a, d[a]))
        return 0

    na, sa, da = pick(args.diff[0])
    nb, sb, db = pick(args.diff[1])
    common = sorted(set(da) & set(db))
    if args.addr is not None:
        common = [a for a in common if args.addr <= a < args.addr + args.len]
    changed = [a for a in common if da[a] != db[a]]
    print(f"sweep {args.diff[0]} [{sa}] vs sweep {args.diff[1]} [{sb}]: "
          f"{len(common)} bytes compared, {len(changed)} differ")
    runs, i = [], 0
    while i < len(changed):
        j = i
        while j + 1 < len(changed) and changed[j + 1] == changed[j] + 1:
            j += 1
        runs.append((changed[i], changed[j]))
        i = j + 1
    for lo, hi in runs:
        old = " ".join(f"{da[a]:02x}" for a in range(lo, hi + 1))
        new = " ".join(f"{db[a]:02x}" for a in range(lo, hi + 1))
        print(f"  {lo:#08x}..{hi:#08x} ({hi - lo + 1}B)")
        print(f"    was: {old}")
        print(f"    now: {new}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
