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
        # AVM's factory-test STUB, as in the 7362 SL image: skipped
        with open(os.path.join(root, 'dectfw_secondlevel_441p2test.hex'),
                  'wb') as f:
            f.write(b':10000000PRODUCTIONTESTSTUB0000000000\n')
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
        check('SKIPPED' in text and 'p2test' in text,
              'production-test STUB reported as skipped')
        check(not os.path.exists(os.path.join(
            out_dir, 'dectfw_secondlevel_441p2test.hex')),
              'production-test STUB not saved')
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

        # the NAND filesystem.image of the 7362 SL and 7412: no plain
        # SquashFS in it, so the AVM NAND reader is tried. A stand-in reader,
        # since a real image would need AVM bytes: this checks the wiring.
        class FakeNand:
            def __init__(self, data):
                if not data.startswith(b'FAKENAND'):
                    raise ValueError('not a NAND image')

            def walk(self):
                yield '/lib/modules', {'type': 1}
                yield '/lib/modules/dectfw_secondlevel_441.hex', {'type': 2}
                yield '/lib/modules/other.ko', {'type': 2}

            def read(self, path, check=None):
                return SECOND
        real = fwx.AvmNandFs
        fwx.AvmNandFs = FakeNand
        try:
            nand = os.path.join(t, 'nand', 'filesystem.image')
            os.makedirs(os.path.dirname(nand))
            with open(nand, 'wb') as f:
                f.write(b'FAKENAND' + b'\0' * 2000)
            nimage = os.path.join(t, 'FRITZ.Box_nand.image')
            with tarfile.open(nimage, 'w') as tar:
                tar.add(nand, arcname='./var/tmp/filesystem.image')
            rc, text = run(nimage, '-o', os.path.join(t, 'out6'))
            check(rc == 0 and 'dectfw_secondlevel_441-test.hex' in text,
                  'NAND image in a tar read through the NAND reader: %d' % rc)
            rc, _ = run(nand, '-o', os.path.join(t, 'out7'))
            check(rc == 0, 'bare NAND filesystem.image, got %d' % rc)
            # only a member named filesystem.image is worth the slow reader
            other = os.path.join(t, 'FRITZ.Box_other.image')
            with tarfile.open(other, 'w') as tar:
                tar.add(nand, arcname='./var/tmp/kernel.image')
            rc, _ = run(other, '-o', os.path.join(t, 'out8'))
            check(rc == 1, 'the NAND reader skips other members, got %d' % rc)
        finally:
            fwx.AvmNandFs = real

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
