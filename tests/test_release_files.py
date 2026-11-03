#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Metadata a release depends on: every source of an OpenWrt package is pinned and has a hash,
every tracked source file names its licence, and the OpenWrt release is the same everywhere."""
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
failed = 0


def check(cond, what):
    global failed
    if not cond:
        failed += 1
        print('FAIL:', what)


def read(path):
    with open(os.path.join(ROOT, path), encoding='utf-8') as f:
        return f.read()


# 1. A package that builds from a git repository pins a commit and the hash of its source archive.
for mk in sorted(glob.glob(os.path.join(ROOT, 'openwrt', '**', 'Makefile'), recursive=True)):
    rel = os.path.relpath(mk, ROOT)
    t = read(rel)
    if 'PKG_SOURCE_PROTO:=git' not in t:
        continue
    check(re.search(r'^PKG_SOURCE_VERSION:=[0-9a-f]{40}$', t, re.M), rel + ': PKG_SOURCE_VERSION is not a full commit id')
    check(re.search(r'^PKG_MIRROR_HASH:=[0-9a-f]{64}$', t, re.M), rel + ': PKG_MIRROR_HASH is missing or "skip"')

# 2. Every tracked source file has an SPDX line near the top (files that cannot hold a comment, and
#    third-party files, are exempt).
exempt = re.compile(r'^(third_party/|docs/specs/|build/|work/|firmwares/)|\.(json|pem|hex|bin|png|svg|md|txt|po|pot)$|/LICENSE$|^LICENSE$')
want = re.compile(r'\.(c|h|py|sh|js|uc|yml|yaml)$|/Makefile$|^Makefile$|\.init$')
files = subprocess.check_output(['git', 'ls-files'], cwd=ROOT, text=True).splitlines()
for f in files:
    if exempt.search(f) or not want.search(f) or not os.path.isfile(os.path.join(ROOT, f)):
        continue
    with open(os.path.join(ROOT, f), encoding='utf-8', errors='replace') as fh:
        head = ''.join(fh.readline() for _ in range(6))
    check('SPDX-License-Identifier' in head, f + ': no SPDX-License-Identifier in the first lines')

# 3. One OpenWrt release for the release scripts, the workflow and the SDK default of the Makefile.
ver = read('tools/release/OPENWRT_VERSION').strip()
check(re.fullmatch(r'\d+\.\d+\.\d+', ver), 'tools/release/OPENWRT_VERSION is not a release number')
wf = read('.github/workflows/packages.yml')
for v in set(re.findall(r'openwrt: "([0-9.]+)"', wf)) | set(re.findall(r'^\s+OPENWRT: ([0-9.]+)', wf, re.M)):
    check(v == ver, 'packages.yml names OpenWrt %s, tools/release/OPENWRT_VERSION says %s' % (v, ver))
check('OPENWRT_VERSION' in read('tools/release/build.sh'), 'build.sh does not read OPENWRT_VERSION')
check('OPENWRT_VERSION' in read('tools/release/images.sh'), 'images.sh does not read OPENWRT_VERSION')

if failed:
    sys.exit(1)
print('release files ok')
