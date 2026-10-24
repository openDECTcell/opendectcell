#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""tools/dect-fw-extract.py against synthetic images: no AVM bytes, just
files with the right names inside the containers AVM uses (a tar holding a
SquashFS behind a header). Needs mksquashfs; skips without it."""
import contextlib
import hashlib
import importlib.util
import io
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, '..', 'tools', 'dect-fw-extract.py')
spec = importlib.util.spec_from_file_location('fwx', TOOL)
fwx = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fwx)

FIRST = b':10000000FIRSTSTAGEFAKE0000000000000000\n'
SECOND = b':10000000SECONDSTAGEFAKE000000000000000\n'
failed = 0


def check(cond, what):
    global failed
    if not cond:
        failed += 1
        print('FAIL:', what)


def run(*argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = fwx.main(list(argv))
    return rc, out.getvalue()


def main():
    if not shutil.which('mksquashfs'):
        print('test_fw_extract: mksquashfs not found, skipped')
        return 0
    t = tempfile.mkdtemp()
    try:
        root = os.path.join(t, 'root', 'lib', 'firmware')
        os.makedirs(root)
        with open(os.path.join(root, 'dectfw_firstlevel_441_slow.hex'), 'wb') as f:
            f.write(FIRST)
        with open(os.path.join(root, 'dectfw_secondlevel_441.hex'), 'wb') as f:
            f.write(SECOND)
        with open(os.path.join(root, 'other.bin'), 'wb') as f:
            f.write(b'x' * 5000)
        sq = os.path.join(t, 'fs.sqfs')
        subprocess.run(['mksquashfs', os.path.join(t, 'root'), sq, '-comp',
                        'xz', '-noappend', '-quiet'], check=True,
                       stdout=subprocess.DEVNULL)
        # AVM style: the SquashFS behind a header, inside a tar
        fsimg = os.path.join(t, 'filesystem.image')
        with open(fsimg, 'wb') as f:
            f.write(b'\xa5' * 256)
            with open(sq, 'rb') as s:
                f.write(s.read())
        image = os.path.join(t, 'FRITZ.Box_test.image')
        with tarfile.open(image, 'w') as tar:
            tar.add(fsimg, arcname='./var/tmp/filesystem.image')

        # unknown builds: saved, said so, exit 2
        out_dir = os.path.join(t, 'out1')
        rc, text = run(image, '-o', out_dir)
        check(rc == 2, 'unknown builds exit 2, got %d' % rc)
        check('UNKNOWN' in text, 'unknown build reported')
        got = sorted(os.listdir(out_dir)) if os.path.isdir(out_dir) else []
        check(got == ['dectfw_firstlevel_441_slow.hex',
                      'dectfw_secondlevel_441.hex'],
              'both files extracted from the tar: %s' % got)
        if got:
            with open(os.path.join(out_dir, got[1]), 'rb') as f:
                check(f.read() == SECOND, 'content intact')

        # known builds: renamed as the service expects, exit 0, upload hint
        fwx.KNOWN[hashlib.sha256(FIRST).hexdigest()] = (
            'test', 't1', 'first stage', 'dectfw_firstlevel_441_slow.hex')
        fwx.KNOWN[hashlib.sha256(SECOND).hexdigest()] = (
            'test', 't2', 'second stage', 'dectfw_secondlevel_441-test.hex')
        out_dir = os.path.join(t, 'out2')
        rc, text = run(image, '-o', out_dir)
        check(rc == 0, 'known builds exit 0, got %d' % rc)
        check(os.path.exists(os.path.join(out_dir,
                                          'dectfw_secondlevel_441-test.hex')),
              'second stage saved under its build name')
        check("opendectcell.main.second='dectfw_secondlevel_441-test.hex'" in text,
              'uci hint names the second stage')

        # a bare SquashFS and an extracted folder work too
        rc, _ = run(sq, '-o', os.path.join(t, 'out3'))
        check(rc == 0, 'bare squashfs, got %d' % rc)
        rc, _ = run(os.path.join(t, 'root'), '-o', os.path.join(t, 'out4'))
        check(rc == 0, 'folder, got %d' % rc)

        # nothing to find
        empty = os.path.join(t, 'empty.bin')
        with open(empty, 'wb') as f:
            f.write(b'\0' * 1000)
        rc, text = run(empty, '-o', os.path.join(t, 'out5'))
        check(rc == 1 and 'No DECT firmware' in text, 'empty input exits 1')
    finally:
        shutil.rmtree(t)
    print('tests/test_fw_extract.py: %s' % ('ok' if not failed else
                                            '%d failed' % failed))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
