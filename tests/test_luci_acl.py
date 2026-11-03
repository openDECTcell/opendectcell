#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The rpcd ACL of the LuCI app: what a LuCI session may run and write as root. Nothing may be open-ended, and
the files LuCI hands over (credentials, uploads) go through a root-only directory, not /tmp."""
import json
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
APP = os.path.join(ROOT, 'openwrt', 'applications', 'luci-app-opendectcell')
FILES = os.path.join(ROOT, 'openwrt', 'net', 'opendectcell', 'files')
failed = 0


def check(cond, what):
    global failed
    if not cond:
        failed += 1
        print('FAIL:', what)


def read(path):
    with open(path, encoding='utf-8') as f:
        return f.read()


acl = json.loads(read(os.path.join(APP, 'root/usr/share/rpcd/acl.d/luci-app-opendectcell.json')))['luci-app-opendectcell']
execs = []
writes = []
for part in ('read', 'write'):
    for path, perms in acl.get(part, {}).get('file', {}).items():
        if 'exec' in perms:
            execs.append(path)
        if 'write' in perms:
            writes.append(path)

# 1. no command takes arguments of any kind: every wildcard is behind a fixed verb and an exact slot or path
OPEN = re.compile(r'^\S+( \S+)? \*$')
for e in execs:
    check(not OPEN.match(e), 'open-ended exec: %s' % e)
    check(not re.search(r'\.\.|;|&|\|', e), 'odd characters in exec: %s' % e)

# 2. nothing is written in /tmp: hand-overs use the root-only directory
for w in writes:
    check(not w.startswith('/tmp/'), 'write to /tmp: %s' % w)
    check(w.startswith('/var/run/opendectcell-ui/') or w.startswith('/etc/opendectcell-sub.'), 'write outside the hand-over directory: %s' % w)

# 3. the paths the page uses are the ones the ACL, the init script, firmware.sh and carddav.uc agree on
js = read(os.path.join(APP, 'htdocs/luci-static/resources/view/opendectcell/settings.js'))
check("var UI_DIR = '/var/run/opendectcell-ui/';" in js, 'settings.js hands over somewhere else')
check('/tmp/opendectcell' not in js, 'settings.js still names a /tmp hand-over file')
for name in ('opendectcell-carddav-discover.json', 'opendectcell-enroll.', 'opendectcell-fw-upload.'):
    check(name in js, 'settings.js lacks %s' % name)
init = read(os.path.join(FILES, 'opendectcell.init'))
check('UI_DIR=/var/run/opendectcell-ui' in init and 'chmod 700 "$UI_DIR"' in init, 'the init script does not make the directory root-only')
check('@/var/run/opendectcell-ui/opendectcell-enroll.*)' in init, 'the init script removes another enrolment file')
check('"/etc/init.d/opendectcell ui_dir"' in json.dumps(acl).replace('\\"', '"') or '/etc/init.d/opendectcell ui_dir' in execs, 'ui_dir is not allowed')
check('UPLOAD=${OPENDECTCELL_UPLOAD_DIR:-/var/run/opendectcell-ui}' in read(os.path.join(FILES, 'firmware.sh')), 'firmware.sh reads uploads from another directory')
check("'/var/run/opendectcell-ui/opendectcell-carddav-discover.json'" in read(os.path.join(FILES, 'carddav.uc')), 'carddav.uc takes another file')
check('/var/run/opendectcell-ui/opendectcell-carddav-discover.json' in ' '.join(execs), 'the ACL does not name the discover file')

if failed:
    sys.exit(1)
print('test_luci_acl.py: ok')
