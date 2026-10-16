#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Read-only access to AVM's Lantiq-era NAND filesystem.image (7362 SL, 7412, ...).

Format notes (see Fritzbox7362SL/DECT-analysis.md):
  * big-endian SquashFS 4 with little-endian metadata block headers, xz
  * interleaved 1 KiB sector-index headers (256 consecutive LE u32 values)
  * data and metadata regions do not share one base offset, so file data is
    located by matching each file's block-size chain against xz streams.

Usage:
  avm_nand_sqfs.py IMAGE ls
  avm_nand_sqfs.py IMAGE cat PATH > out
  avm_nand_sqfs.py IMAGE extract OUTDIR PATH [PATH...]
IMAGE may be the AVM .image tar or a bare filesystem.image.
"""
import lzma
import os
import re
import struct
import sys
import tarfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sqfs_be import Sqfs  # noqa: E402


def load_fs_image(path):
    if tarfile.is_tarfile(path):
        with tarfile.open(path) as t:
            return t.extractfile('./var/tmp/filesystem.image').read()
    with open(path, 'rb') as f:
        return f.read()


def is_index_header(d, p):
    """1 KiB block of strictly increasing LE u32 sector numbers (small steps; jumps =
    remapped blocks), optionally zero-padded when it covers fewer than 256 sectors"""
    v = struct.unpack_from('<256I', d, p)
    if not 0 < v[0] < len(d) // 1024 + 0x1000:
        return False
    n = 256
    while n and v[n - 1] == 0:
        n -= 1
    if n < 8:
        return False
    steps = [b - a for a, b in zip(v[:n], v[1:n])]
    return all(0 < s <= 0x400 for s in steps) and sum(s == 1 for s in steps) >= (n - 1) * 3 // 4


def strip_index_headers(d):
    out = bytearray()
    prev = 0
    p = 0
    while p <= len(d) - 1024:
        if is_index_header(d, p):
            out += d[prev:p]
            prev = p + 0x400
            p += 0x400
        else:
            p += 0x100  # headers are 0x100-aligned
    out += d[prev:]
    return bytes(out)


class AvmNandFs:
    def __init__(self, raw):
        self.L = strip_index_headers(raw)
        L = self.L
        sb = None
        for m in re.finditer(b'sqsh', L):
            v = struct.unpack('>IIIIHHHHHHQQQQQQQQ', L[m.start() + 4:m.start() + 96])
            # sane superblock: power-of-two block size, version 4.0, tables in order
            if v[2] in (1 << 16, 1 << 17, 1 << 18, 1 << 20) and v[8] == 4 and v[14] < v[15] < v[11]:
                sb = m.start()
                break
        if sb is None:
            raise ValueError('no plausible SquashFS superblock found')
        it, dt = v[14], v[15]
        self.xs = [m.start() for m in re.finditer(b'\xfd7zXZ\x00', L)]
        xset = set(self.xs)
        def mhdr_ok(p):
            if p < 0 or p + 8 > len(L):
                return False
            h = struct.unpack_from('<H', L, p)[0]
            n = h & 0x7fff
            return 0 < n <= 0x2000 and bool(h & 0x8000 or (p + 2) in xset)

        # the metadata region's base may even be negative (region placed before the superblock)
        cands = sorted({p - 2 - t for p in self.xs for t in (it, dt)})
        self.fs = None
        for base in (c for c in cands if mhdr_ok(c + it) and mhdr_ok(c + dt)):
            img = bytearray(b'\0' * max(0, -base) + L[max(0, base):])
            img[0:96] = L[sb:sb + 96]
            fs = Sqfs(bytes(img))
            fs.he = '<'
            try:
                if sum(1 for _ in fs.walk()) > 100:
                    self.fs = fs
                    break
            except Exception:  # noqa: BLE001 - wrong candidate, try the next one
                continue
        if self.fs is None:
            raise ValueError('metadata base not found')
        self.slen = {}
        for p in self.xs:
            dec = lzma.LZMADecompressor(format=lzma.FORMAT_XZ)
            try:
                dec.decompress(L[p:p + 0x12000])
                if dec.eof:
                    self.slen[p] = 0x12000 - len(dec.unused_data)
            except lzma.LZMAError:
                pass
        self.bysize = {}
        for p, n in self.slen.items():
            self.bysize.setdefault(n, []).append(p)

    def walk(self):
        return self.fs.walk()

    EMPTY = (b'\0' * 0x400, b'\xff' * 0x400)

    def _repair_block(self, q, size):
        """Block at q does not decode because a non-data gap (empty 0x00/0xff sectors
        and other filler) interrupts it. Try split points at 0x100-aligned empty sectors
        inside the block and 0x100-aligned resume points up to 256 KiB later; accept only
        if the xz stream decodes (it carries a CRC). Returns (data, end_position) or None."""
        L = self.L
        first = q + ((-q) % 0x100)
        for c in range(first, q + size, 0x100):
            if L[c:c + 0x400] not in self.EMPTY:
                continue
            head = L[q:c]
            rest = size - len(head)
            # the image is circular: data near the end continues near the start
            resumes = list(range(c + 0x400, min(len(L), c + 0x40000), 0x100))
            if c + 0x40000 > len(L):
                resumes += list(range(0, 0x40000, 0x100))
            for r in resumes:
                try:
                    data = lzma.decompress(head + L[r:r + rest])
                except lzma.LZMAError:
                    continue
                if data:  # an all-zero window "decodes" as xz stream padding; reject that
                    return data, r + rest
            break
        return None

    def _filler_at(self, q):
        """q is at (or just before) a 0x100-aligned run of empty sectors"""
        c = q + ((-q) % 0x100)
        return c - q < 0x100 and self.L[c:c + 0x400] in self.EMPTY

    def _next_stream(self, q, size):
        """first decodable xz stream of the wanted size within 256 KiB after q"""
        for p in self.bysize.get(size, []):
            if q < p < q + 0x40000 or (q + 0x40000 > len(self.L) and p < 0x40000):
                return p
        return None

    def _read_chain(self, q, blocks, sizes):
        out = b''
        for b, s in zip(blocks, sizes):
            if b & (1 << 24):  # stored uncompressed
                out += self.L[q:q + s]
                q += s
            elif self.slen.get(q) == s:
                out += lzma.decompress(self.L[q:q + s])
                q += s
            elif self._filler_at(q) and self._next_stream(q, s) is not None:
                # non-data gap exactly at the block boundary
                q = self._next_stream(q, s)
                out += lzma.decompress(self.L[q:q + s])
                q += s
            else:
                fixed = self._repair_block(q, s)
                if fixed is None:
                    return None
                out += fixed[0]
                q = fixed[1]
        return out

    def read(self, path, check=None):
        ino = self.fs.lookup(path)
        if ino['type'] in (3, 10):
            raise IsADirectoryError('symlink -> ' + ino['target'])
        L = self.L
        out = b''
        blocks = ino['blocks']
        sizes = [b & 0xffffff for b in blocks]
        if blocks:
            bad = [p for p in self.xs if p not in self.slen]
            for p in self.bysize.get(sizes[0], []) + bad:
                data = self._read_chain(p, blocks, sizes)
                if data is not None:
                    out = data
                    break
            else:
                raise LookupError('block chain not found: ' + path)
        if ino['frag'] != 0xffffffff:
            st, sz, _ = self.fs.frag(ino['frag'])
            n = sz & 0xffffff
            want = ino['size'] - len(out)
            cands = []
            blocks = [lzma.decompress(L[p:p + n]) for p in self.bysize.get(n, [])]
            if not blocks:  # fragment block interrupted by a gap: repair it
                for p in (p for p in self.xs if p not in self.slen):
                    fixed = self._repair_block(p, n)
                    if fixed:
                        blocks.append(fixed[0])
            for fb in blocks:
                c = fb[ino['foff']:ino['foff'] + want]
                if len(c) == want and (check is None or check(out + c)):
                    cands.append(c)
            if not cands:
                raise LookupError('fragment not found: ' + path)
            if len(set(cands)) > 1:
                raise LookupError('fragment ambiguous (%d candidates): %s' % (len(set(cands)), path))
            out += cands[0]
        return out[:ino['size']]


def plausible(data):
    """content check used to disambiguate fragments"""
    if data[:4] == b'\x7fELF' or data[:2] == b'#!' or data[:1] == b':':
        return True
    return not data[:1] or 0x09 <= data[0] < 0x7f


def main():
    img, cmd = sys.argv[1], sys.argv[2]
    fs = AvmNandFs(load_fs_image(img))
    if cmd == 'ls':
        for p, i in fs.walk():
            kind = {1: 'd', 8: 'd', 2: '-', 9: '-', 3: 'l', 10: 'l'}.get(i['type'], '?')
            extra = (' -> ' + i['target']) if 'target' in i else ''
            print(kind, i.get('size', '') if kind == '-' else '', p + extra)
    elif cmd == 'cat':
        sys.stdout.buffer.write(fs.read(sys.argv[3], check=plausible))
    elif cmd == 'extract':
        outdir = sys.argv[3]
        os.makedirs(outdir, exist_ok=True)
        for path in sys.argv[4:]:
            try:
                data = fs.read(path, check=plausible)
                with open(os.path.join(outdir, os.path.basename(path)), 'wb') as f:
                    f.write(data)
                print('ok', len(data), path)
            except Exception as e:  # noqa: BLE001 - report and continue
                print('FAIL', path, e)


if __name__ == '__main__':
    main()
