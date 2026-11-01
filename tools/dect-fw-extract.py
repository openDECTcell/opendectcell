#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Get the DECT chip firmware out of an official FRITZ!OS image.

opendectcell may not ship AVM's DECT firmware, so every user extracts it once,
from the image for their own box, on their own PC:

    python3 dect-fw-extract.py FRITZ.Box_7520-07.56.image

It finds the dectfw_*.hex files, checks them against the builds opendectcell
knows (SHA-256), writes them to ./opendectcell-firmware/ and prints how to put
them on the box. Python 3 standard library only, so it runs unchanged on
Linux, macOS and Windows. See docs/firmware-sourcing.md.

Accepted input: a FRITZ!OS .image file (a tar archive), a bare SquashFS
image, the NAND filesystem.image of older boxes (7362 SL, 7412), or a folder
that already holds the extracted files.
"""
import argparse
import hashlib
import os
import sys
import tarfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sqfs_be import Sqfs  # noqa: E402  (the reader next to this file)
from avm_nand_sqfs import AvmNandFs, plausible  # noqa: E402

# SHA-256 of each .hex file openDECTcell knows: (model, FRITZ!OS build, stage,
# the name to save it under). docs/firmware-sourcing.md section 4.
KNOWN = {
    '0d855b5929f8b2ad0b8d8fb38065b32e27f9357cd44a2b4e60573b1b5b6da895':
        ('7520/7530', '07.56 and 08.25', 'first stage',
         'dectfw_firstlevel_441_slow.hex'),
    '9bdb332678f981d83ac31cf1b68c489ab9c979e2ac601d18b81bb802f16d0d20':
        ('7520/7530', '07.56', 'second stage',
         'dectfw_secondlevel_441-07.56.hex'),
    '7cac33c72b2222468f34033a15c35487aa35062265ab0013bb480068ebab7b31':
        ('7520/7530', '08.25', 'second stage',
         'dectfw_secondlevel_441.hex'),
    # the 7362 SL's first stage is the same file; it keeps the 7412's name
    '72f8e27a0d01c301a05a8b373a71887af882a6cebcb1a30bd4a435580e1045f2':
        ('7412 and 7362 SL', '137.06.88 and 07.18', 'first stage',
         'dectfw_firstlevel_441-06.88-7412.hex'),
    '4fabee65d07a3214f9083de81ae54089f01bbfccb4cdbb672f37487a09cdfaa6':
        ('7412', '137.06.88', 'second stage',
         'dectfw_secondlevel_441-06.88-7412.hex'),
    '8aae3b5e3450fd2c9999e003068e786096c11d8ef53f7d47c25cebd057a797d3':
        ('7362 SL', '07.18', 'second stage',
         'dectfw_secondlevel_441-07.18-7362sl.hex'),
}

SQFS_MAGICS = (b'hsqs', b'sqsh')     # little- and big-endian SquashFS 4
SCAN_LIMIT = 4 * 1024 * 1024         # look this far into a file for one


def is_firmware(name):
    base = os.path.basename(name)
    return base.startswith('dectfw_') and base.endswith('.hex')


def squashfs_offsets(data):
    """Offsets in data that start a plausible SquashFS 4 superblock."""
    limit = min(len(data), SCAN_LIMIT)
    for magic in SQFS_MAGICS:
        pos = data.find(magic, 0, limit)
        while pos >= 0:
            # version 4.0 at offset 28, block size a power of two
            major = data[pos + 28:pos + 30]
            if major in (b'\x04\x00', b'\x00\x04'):
                yield pos
            pos = data.find(magic, pos + 4, limit)


def from_squashfs(data):
    """(path, bytes) of every dectfw_*.hex in any SquashFS inside data."""
    found = []
    for off in squashfs_offsets(data):
        try:
            fs = Sqfs(data[off:])
            for path, ino in fs.walk():
                if ino.get('type') in (2, 9) and is_firmware(path):
                    found.append((path, fs.read(ino)))
        except Exception:      # not a filesystem after all, or one we
            continue           # cannot read: try the next candidate
        if found:
            break
    return found


def from_avm_nand(data):
    """(path, bytes) of every dectfw_*.hex in an AVM NAND filesystem.image
    (7362 SL, 7412): a SquashFS cut up by sector index headers, which
    from_squashfs cannot read. Slow, so only tried where that found none."""
    try:
        fs = AvmNandFs(data)
    except Exception:          # not this format either
        return []
    found = []
    for path, ino in fs.walk():
        if ino.get('type') in (2, 9) and is_firmware(path):
            try:
                found.append((path, fs.read(path, check=plausible)))
            except Exception as e:
                print('Could not read %s from the image: %s' % (path, e))
    return found


def from_tar(path):
    found = []
    with tarfile.open(path) as tar:
        for m in tar.getmembers():
            if not m.isfile():
                continue
            data = tar.extractfile(m).read()
            if is_firmware(m.name):
                found.append((m.name, data))
                continue
            files = from_squashfs(data)
            if not files and os.path.basename(m.name) == 'filesystem.image':
                files = from_avm_nand(data)
            found.extend(files)
    return found


def from_folder(path):
    found = []
    for root, _dirs, files in os.walk(path):
        for name in files:
            if is_firmware(name):
                with open(os.path.join(root, name), 'rb') as f:
                    found.append((os.path.join(root, name), f.read()))
    return found


def find(path):
    if os.path.isdir(path):
        return from_folder(path)
    if tarfile.is_tarfile(path):
        return from_tar(path)
    with open(path, 'rb') as f:
        data = f.read()
    return from_squashfs(data) or from_avm_nand(data)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description='Extract the DECT chip firmware from a FRITZ!OS image.')
    ap.add_argument('image', help='FRITZ!OS .image file, SquashFS image, '
                    'or a folder holding the extracted files')
    ap.add_argument('-o', '--out', default='opendectcell-firmware',
                    help='folder to write the files to (default: %(default)s)')
    ap.add_argument('--box', default='root@192.168.1.1',
                    help='how to reach the box with scp (for the hint)')
    args = ap.parse_args(argv)

    try:
        found = find(args.image)
    except OSError as e:
        print('Cannot read %s: %s' % (args.image, e.strerror or e))
        return 1
    if not found:
        print('No DECT firmware (dectfw_*.hex) found in %s.\n'
              'Is this the official FRITZ!OS image for a box with DECT?'
              % args.image)
        return 1

    os.makedirs(args.out, exist_ok=True)
    known = 0
    second = None
    for path, data in found:
        digest = hashlib.sha256(data).hexdigest()
        info = KNOWN.get(digest)
        name = (info[3] if info and info[3] else os.path.basename(path))
        with open(os.path.join(args.out, name), 'wb') as f:
            f.write(data)
        if info:
            known += 1
            print('OK       %-34s FRITZ!Box %s, FRITZ!OS %s, %s'
                  % (name, info[0], info[1], info[2]))
            if info[2] == 'second stage' and info[3]:
                second = name
        else:
            print('UNKNOWN  %-34s not a build opendectcell knows (SHA-256 %s...)'
                  % (name, digest[:16]))

    print()
    if not known:
        print('None of these files is a build opendectcell was tested with. '
              'They were\nsaved, but the box will refuse them unless you '
              'allow unverified\nfirmware. Use the FRITZ!OS version listed '
              'in docs/firmware-sourcing.md.')
        return 2
    print('Saved to %s/. Put them on the box:\n' % args.out)
    print('  scp -O %s/*.hex %s:/etc/opendectcell/firmware/'
          % (args.out, args.box))
    if second:
        print("  ssh %s \"uci set opendectcell.main.second='%s'; uci commit "
              "opendectcell\"" % (args.box, second))
    print('\nThen enable the service: option enabled \'1\' in '
          '/etc/config/opendectcell,\nand /etc/init.d/opendectcell restart.')
    return 0 if known == len(found) else 2


if __name__ == '__main__':
    sys.exit(main())
