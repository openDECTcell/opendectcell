#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare chip memory ACROSS runs, ignoring anything that moves within a run.

`dect_memdump.py --diff` compares two sweeps of one capture. To ask "what did
this mail change?" you need two *runs* that differ only in the mail, and you
have to discount every address that is simply a counter -- run A alone has 469
of those out of 49 152, so a naive diff drowns in them.

  ./dect_memcmp.py A.log B.log            # what differs, counters excluded
  ./dect_memcmp.py A.log B.log --addr 0x2b4 --len 16
"""
import argparse, sys
sys.path.insert(0, __file__.rsplit("/", 1)[0])
from dect_memdump import parse


def stable(sweeps):
    """{addr: value} for addresses holding one value across every sweep."""
    vals = {}
    for _n, _state, d in sweeps:
        for a, v in d.items():
            vals.setdefault(a, set()).add(v)
    return {a: s.pop() for a, s in vals.items() if len(s) == 1}


def main():
    p = argparse.ArgumentParser(description=__doc__,
            formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("a"); p.add_argument("b")
    p.add_argument("--addr", type=lambda x: int(x, 0))
    p.add_argument("--len", type=lambda x: int(x, 0), default=32)
    args = p.parse_args()

    A, B = parse(args.a), parse(args.b)
    sa, sb = stable(A), stable(B)
    common = sorted(set(sa) & set(sb))
    if args.addr is not None:
        common = [a for a in common if args.addr <= a < args.addr + args.len]

    moved_a = len({a for _n, _s, d in A for a in d}) - len(sa)
    moved_b = len({a for _n, _s, d in B for a in d}) - len(sb)
    diff = [a for a in common if sa[a] != sb[a]]
    print(f"{args.a}: {len(A)} sweeps, {len(sa)} stable addresses "
          f"({moved_a} excluded as changing within the run)")
    print(f"{args.b}: {len(B)} sweeps, {len(sb)} stable addresses "
          f"({moved_b} excluded)")
    print(f"compared {len(common)} stable addresses present in both, "
          f"{len(diff)} DIFFER\n")
    if not diff:
        print("  nothing differs. The two runs put the chip in the same state,")
        print("  as far as these addresses can show.")
        return 0
    runs, i = [], 0
    while i < len(diff):
        j = i
        while j + 1 < len(diff) and diff[j + 1] == diff[j] + 1:
            j += 1
        runs.append((diff[i], diff[j])); i = j + 1
    for lo, hi in runs:
        print(f"  {lo:#08x}..{hi:#08x} ({hi - lo + 1}B)")
        print("    A: " + " ".join(f"{sa[x]:02x}" for x in range(lo, hi + 1)))
        print("    B: " + " ".join(f"{sb[x]:02x}" for x in range(lo, hi + 1)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
