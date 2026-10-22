#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Print the broadcast capability block from a `dect-stubd -a memdump` log.

The chip holds the four Qt groups it broadcasts as one contiguous 20-octet
block (`docs/mac-bringup.md`):

    38 49 94 ee 08 | 40 11 10 20 04 | c8 01 85 11 34 | 73 07 00 00 00
    QH=3             QH=4             QH=C             QH=7 escape

This locates that block by its QH nibbles rather than by a fixed address, so
it keeps working if the chip firmware moves it, and prints each group with the
QH it declares. Give it several logs and it diffs them against the first.

  ./dect_qh7.py qbase.log qa0006.log
"""
import sys, importlib.util, os

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "dect_memdump", os.path.join(_here, "dect_memdump.py"))
_md = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_md)


def merged(path):
    """All sweeps in one address -> byte map (later sweeps win)."""
    out = {}
    for _n, _state, d in _md.parse(path):
        out.update(d)
    return out


def find_blocks(mem):
    """Every address where four consecutive 5-octet groups declare QH 3,4,C,7."""
    want = [0x3, 0x4, 0xC, 0x7]
    hits = []
    for a in sorted(mem):
        if not all((a + i) in mem for i in range(20)):
            continue
        if [(mem[a + 5 * g] >> 4) & 0xF for g in range(4)] == want:
            hits.append(a)
    return hits


def fmt(mem, a):
    return [" ".join(f"{mem[a + 5 * g + i]:02x}" for i in range(5))
            for g in range(4)]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    names = ["QH=3 fixed part caps", "QH=4 ext FP caps",
             "QH=C ext FP caps p2", "QH=7 escape"]
    ref = None
    for path in sys.argv[1:]:
        label = os.path.basename(path)
        if not os.path.exists(path):
            print(f"{label}: MISSING")
            continue
        mem = merged(path)
        blocks = find_blocks(mem)
        if not blocks:
            print(f"{label}: block not found "
                  f"({len(mem)} bytes covered) -- did the memdump cover it?")
            continue
        # The live copy is the higher address; report every copy found.
        print(f"\n{label}: {len(blocks)} copy/copies at "
              + ", ".join(f"{a:#06x}" for a in blocks))
        groups = fmt(mem, blocks[-1])
        for n, g in zip(names, groups):
            mark = ""
            if ref is not None:
                i = names.index(n)
                if g != ref[i]:
                    mark = f"   <<< CHANGED (was {ref[i]})"
            print(f"    {n:<22} {g}{mark}")
        if ref is None:
            ref = groups
            print("    (baseline; later logs are diffed against this)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
