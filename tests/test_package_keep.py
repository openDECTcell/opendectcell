#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""What a sysupgrade keeps: the OpenWrt packages install keep lists, and what the
programs write to flash is in them. A cell that loses /etc/opendectcell/cell/ in a firmware
upgrade needs a new enrolment, which for a box in a makerspace is a visit."""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
FILES = os.path.join(ROOT, 'openwrt', 'net', 'opendectcell', 'files')
MAKEFILE = os.path.join(ROOT, 'openwrt', 'net', 'opendectcell', 'Makefile')
failed = 0


def check(cond, what):
    global failed
    if not cond:
        failed += 1
        print('FAIL:', what)


def read(path):
    with open(path) as f:
        return f.read()


def keep(name):
    return [l.strip() for l in read(os.path.join(FILES, name)).splitlines() if l.strip()]


mk = read(MAKEFILE)
cell_src = read(os.path.join(ROOT, 'src', 'cell', 'dect-cell.c'))
m = re.search(r'#define\s+CELL_DIR\s+"([^"]+)"', cell_src)
check(m is not None, 'dect-cell.c defines CELL_DIR')
cell_dir = m.group(1).rstrip('/') + '/' if m else ''

# opendectcell-cell installs its own keep list, and it holds the credentials directory
inst = re.search(r'define Package/opendectcell-cell/install\n(.*?)\nendef', mk, re.S)
check(inst is not None and 'keep.d/opendectcell-cell' in inst.group(1) and 'opendectcell-cell.keep' in inst.group(1),
      'opendectcell-cell installs /lib/upgrade/keep.d/opendectcell-cell')
check(cell_dir in keep('opendectcell-cell.keep'), 'opendectcell-cell.keep lists ' + cell_dir)

# openDECTcell keeps the firmware, the profiles and the handset stores
base = keep('opendectcell.keep')
for p in ('/etc/opendectcell/firmware/', '/etc/opendectcell/profiles/', '/etc/opendectcell-sub.*'):
    check(p in base, 'opendectcell.keep lists ' + p)
check('keep.d/opendectcell\n' in mk or 'keep.d/opendectcell$' in mk or '/lib/upgrade/keep.d/opendectcell\n' in mk
      or re.search(r'opendectcell\.keep \$\(1\)/lib/upgrade/keep\.d/opendectcell\b', mk) is not None,
      'opendectcell installs its keep list')

if failed:
    print(failed, 'failed')
    sys.exit(1)
print('tests/test_package_keep.py: ok')
