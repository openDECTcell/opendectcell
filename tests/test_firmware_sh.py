#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""openwrt/net/opendectcell/files/firmware.sh, the box's side of LuCI's firmware
upload (docs/firmware-sourcing.md), in a scratch folder with a fake board
profile and a fake uci. Synthetic bytes only: no AVM firmware."""
import hashlib
import importlib.util
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
SCRIPT = os.path.join(ROOT, 'openwrt', 'net', 'opendectcell', 'files', 'firmware.sh')
INIT = os.path.join(ROOT, 'openwrt', 'net', 'opendectcell', 'files', 'opendectcell.init')
TOOL = os.path.join(ROOT, 'tools', 'dect-fw-extract.py')

FIRST = b':10000000FIRSTSTAGEFAKE0000000000000000\n'
SECOND = b':10000000SECONDSTAGEFAKE000000000000000\n'
OTHER = b':10000000SOMETHINGELSEFAKE00000000000000\n'
failed = 0


def check(cond, what):
    global failed
    if not cond:
        print('FAIL: ' + what)
        failed += 1


def sha(b):
    return hashlib.sha256(b).hexdigest()


def table(text):
    """hash -> name (the init script's table has hashes only: name None)"""
    m = re.search(r'KNOWN_FW="(.*?)"', text, re.S)
    out = {}
    for line in m.group(1).split('\n'):
        w = line.split()
        if w:
            out[w[0]] = w[1] if len(w) > 1 else None
    return out


class Box:
    """A scratch box: board profile, firmware folder, upload folder, uci."""

    def __init__(self, tmp, pins=True, allow=False, script=SCRIPT):
        self.tmp = tmp
        self.script = script
        self.boards = os.path.join(tmp, 'boards')
        self.fw = os.path.join(tmp, 'fw')
        self.up = os.path.join(tmp, 'up')
        for d in (self.boards, self.up, os.path.join(tmp, 'bin')):
            os.makedirs(d)
        with open(os.path.join(tmp, 'board_name'), 'w') as f:
            f.write('test,box\n')
        with open(os.path.join(self.boards, 'test,box'), 'w') as f:
            f.write('fw_firstlevel=first.hex\nfw_secondlevel=second.hex\n')
            if pins:
                f.write('fw_firstlevel_sha256=%s\n' % sha(FIRST))
                f.write('fw_secondlevel_sha256=%s\n' % sha(SECOND))
        with open(os.path.join(tmp, 'bin', 'uci'), 'w') as f:
            f.write('#!/bin/sh\ncase "$3" in\n'
                    '*fw_allow_unverified) echo %d ;;\n'
                    '*firmware_dir) echo "%s" ;;\n'
                    '*) exit 1 ;;\nesac\n' % (1 if allow else 0, self.fw))
        os.chmod(os.path.join(tmp, 'bin', 'uci'), 0o755)

    def run(self, *args):
        env = dict(os.environ, PATH=os.path.join(self.tmp, 'bin') + ':' +
                   os.environ['PATH'],
                   OPENDECTCELL_BOARDS=self.boards, OPENDECTCELL_UPLOAD_DIR=self.up,
                   OPENDECTCELL_BOARD_NAME=os.path.join(self.tmp, 'board_name'),
                   OPENDECTCELL_FW_MAX='1000')
        r = subprocess.run([os.environ.get('TEST_SH', 'sh'), self.script] + list(args), capture_output=True,
                           text=True, env=env)
        try:
            j = json.loads(r.stdout)
        except ValueError:
            j = None
            check(False, 'not JSON: %r %r' % (r.stdout, r.stderr))
        return r.returncode, j

    def upload(self, data, name='opendectcell-fw-upload.first'):
        p = os.path.join(self.up, name)
        with open(p, 'wb') as f:
            f.write(data)
        return p

    def state(self, role):
        rc, j = self.run('status')
        return [f for f in j['files'] if f['role'] == role][0]


def main():
    # the copies of the table agree
    with open(SCRIPT) as f:
        text = f.read()
    ours = table(text)
    with open(INIT) as f:
        check(set(table(f.read())) == set(ours),
              'KNOWN_FW differs from the init script')
    spec = importlib.util.spec_from_file_location('fwx', TOOL)
    fwx = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fwx)
    check(set(ours) == set(fwx.KNOWN),
          'KNOWN_FW and dect-fw-extract.py know different builds')
    for h, n in ours.items():
        check(n == fwx.KNOWN[h][3], 'name of %s: %s, the extractor says %s'
              % (h[:8], n, fwx.KNOWN[h][3]))

    tmp = tempfile.mkdtemp()
    try:
        b = Box(os.path.join(tmp, 'a'))
        rc, j = b.run('status')
        check(rc == 0 and j['board'] == 'test,box', 'status answers')
        check(b.state('first')['state'] == 'missing', 'nothing there: missing')
        check(b.state('first')['expected'] == sha(FIRST), 'pin shown')

        # the right file goes in, 0600, and the upload is consumed
        p = b.upload(FIRST)
        rc, j = b.run('install', 'first', p)
        check(rc == 0 and j.get('ok'), 'good file installed: %r' % j)
        dest = os.path.join(b.fw, 'first.hex')
        check(os.path.exists(dest) and open(dest, 'rb').read() == FIRST,
              'file in place')
        check(stat.S_IMODE(os.stat(dest).st_mode) == 0o600, 'mode 0600')
        check(not os.path.exists(p), 'upload removed')
        check(b.state('first')['state'] == 'ok', 'state ok')
        check(not [n for n in os.listdir(b.fw) if n.startswith('.')],
              'no temp file left')

        # the wrong build is refused and the old file stays
        p = b.upload(OTHER)
        rc, j = b.run('install', 'first', p)
        check(rc == 1 and 'expected SHA-256 ' + sha(FIRST) in j['error'],
              'wrong build refused: %r' % j)
        check(open(dest, 'rb').read() == FIRST, 'old file kept')
        check(not os.path.exists(p), 'refused upload removed')

        # a file for the other role under this role's pin
        p = b.upload(SECOND)
        rc, j = b.run('install', 'first', p)
        check(rc == 1, 'second stage in the first slot refused')
        rc, j = b.run('install', 'second', b.upload(SECOND))
        check(rc == 0, 'second stage installed')

        # a file that changed under us shows as a mismatch
        with open(dest, 'wb') as f:
            f.write(OTHER)
        check(b.state('first')['state'] == 'mismatch', 'changed file: mismatch')

        # delete
        rc, j = b.run('delete', 'first')
        check(rc == 0 and not os.path.exists(dest), 'deleted')
        rc, j = b.run('delete', 'first')
        check(rc == 1, 'delete of nothing fails')

        # nothing the caller sends picks a path
        for src in ('/etc/passwd', b.up + '/../x', b.up + '/opendectcell-fw-upload.a/b',
                    b.up + '/other'):
            rc, j = b.run('install', 'first', src)
            check(rc == 1 and 'not an upload file' in j['error'],
                  'source %s refused' % src)
        for role in ('../first', 'third', ''):
            rc, j = b.run('install', role, b.upload(FIRST))
            check(rc == 1 and 'role' in j['error'], 'role %r refused' % role)
            rc, j = b.run('delete', role)
            check(rc == 1, 'delete role %r refused' % role)
        link = os.path.join(b.up, 'opendectcell-fw-upload.first')
        if os.path.exists(link):
            os.unlink(link)
        os.symlink('/etc/passwd', link)
        rc, j = b.run('install', 'first', link)
        check(rc == 1 and 'no uploaded file' in j['error'], 'symlink refused')
        os.unlink(link)

        # size
        p = b.upload(b'x' * 1001)
        rc, j = b.run('install', 'first', p)
        check(rc == 1 and 'larger' in j['error'] and not os.path.exists(p),
              'too big refused')
        p = b.upload(b'')
        rc, j = b.run('install', 'first', p)
        check(rc == 1, 'empty refused')
        check(not os.path.exists(os.path.join(b.fw, 'first.hex')),
              'nothing refused got in')

        # no pin in the profile (7520): a known build goes in, others do not
        b2 = Box(os.path.join(tmp, 'b'), pins=False)
        rc, j = b2.run('install', 'first', b2.upload(OTHER))
        check(rc == 1 and 'not a firmware build opendectcell knows' in j['error'],
              'unknown build refused: %r' % j)
        check(b2.state('first')['state'] == 'missing', 'still missing')

        # no pin, a known build: ok under its own name, not under another's
        first_known = [h for h, n in ours.items()
                       if n == 'dectfw_firstlevel_441_slow.hex'][0]
        for name, want in (('first.hex', 'ok'), ('other.hex', 'wrongname')):
            sc = os.path.join(tmp, 'fw-%s.sh' % want)
            with open(sc, 'w') as f:
                f.write(text.replace('%s dectfw_firstlevel_441_slow.hex'
                                     % first_known, '%s %s' % (sha(FIRST), name)))
            b4 = Box(os.path.join(tmp, 'd' + want), pins=False, script=sc)
            rc, j = b4.run('install', 'first', b4.upload(FIRST))
            if want == 'ok':
                check(rc == 0 and j['file']['state'] == 'ok',
                      'known build under its own name: %r' % j)
            else:
                check(rc == 1 and 'set the First-stage or Second-stage' in j['error']
                      and name in j['error'],
                      'known build under another name refused: %r' % j)
                check(not os.path.exists(os.path.join(b4.fw, 'first.hex')),
                      'wrong name: nothing installed')

        # fw_allow_unverified lets the other build in, as the init does
        b3 = Box(os.path.join(tmp, 'c'), allow=True)
        rc, j = b3.run('install', 'first', b3.upload(OTHER))
        check(rc == 0 and j['file']['state'] == 'mismatch',
              'allowed: installed, still shown as a mismatch: %r' % j)
        rc, j = b3.run('status')
        check(j['unverified'] is True, 'status says unverified is on')
    finally:
        shutil.rmtree(tmp)
    print('tests/test_firmware_sh.py: %s' % ('ok' if not failed else '%d failed' % failed))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
