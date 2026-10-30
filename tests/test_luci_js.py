#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The LuCI views against the minifier OpenWrt's LuCI build runs on them.

luci.mk pipes every .js through jsmin and keeps the original when jsmin
fails, but ships jsmin's half-written output beside it as <file>.js.o. This
jsmin takes a / right after `return` for a division, so `return /re/.test()`
sends it into a string at the first quote in the class and it gives up
("Unterminated set in Regular Expression literal"; settings.js, 2026-10-30).
Write `return (/re/).test()`. With node installed, each view must also parse.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
VIEWS = os.path.join(HERE, '..', 'openwrt', 'applications', 'luci-app-opendectcell',
                     'htdocs', 'luci-static', 'resources', 'view', 'opendectcell')
# a regex literal straight after return (not a comment: // or /*)
TRAP = re.compile(r'\breturn\s+!?/(?![/*])')


def main():
    failed = 0
    node = shutil.which('node')
    files = sorted(f for f in os.listdir(VIEWS) if f.endswith('.js'))
    for name in files:
        path = os.path.join(VIEWS, name)
        with open(path, encoding='utf-8') as f:
            text = f.read()
        for n, line in enumerate(text.split('\n'), 1):
            if TRAP.search(line):
                print('%s:%d: a regex literal after return breaks jsmin; '
                      'write return (/.../)' % (name, n))
                failed += 1
        if node:
            # a view is a function body (it ends in a top-level return)
            with tempfile.NamedTemporaryFile('w', suffix='.js',
                                             delete=False) as t:
                t.write('(function(){\n' + text + '\n})\n')
            r = subprocess.run([node, '--check', t.name],
                               capture_output=True, text=True)
            os.unlink(t.name)
            if r.returncode:
                print('%s: does not parse:\n%s' % (name, r.stderr.strip()))
                failed += 1
    print('tests/test_luci_js.py: %d views, %s%s' % (
        len(files), 'ok' if not failed else '%d failed' % failed,
        '' if node else ' (no node: syntax not checked)'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
