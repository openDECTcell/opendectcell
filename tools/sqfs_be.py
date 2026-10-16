# SPDX-License-Identifier: GPL-2.0-or-later
"""Minimal read-only SquashFS 4 reader (big- or little-endian, xz/gzip/lzma).
Usage: python3 -I sqfs_be.py IMAGE OFFSET ls|cat [PATH]"""
import lzma, zlib, struct, sys


class Sqfs:
    def __init__(self, data):
        self.d = data
        e = '>' if data[:4] == b'sqsh' else '<'
        self.e = e
        (self.inodes, _, self.bs, self.nfrag, self.comp, _, self.flags, _, _, _,
         self.root, self.used, self.idt, _, self.it, self.dt, self.ft, _) = struct.unpack(
            e + 'IIIIHHHHHHQQQQQQQQ', data[4:96])
        self._frag = None

    def dec(self, b):
        if self.comp == 4:
            return lzma.decompress(b)
        if self.comp == 1:
            return zlib.decompress(b)
        if self.comp == 2:
            return lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(b)
        raise ValueError('compression %d' % self.comp)

    def meta(self, start, off, length):
        """read `length` bytes from metadata starting at block `start`, offset `off`"""
        out = b''
        pos = start
        while len(out) < off + length:
            hdr = struct.unpack(getattr(self, 'he', self.e) + 'H', self.d[pos:pos + 2])[0]
            n = hdr & 0x7fff
            blk = self.d[pos + 2:pos + 2 + n]
            out += blk if hdr & 0x8000 else self.dec(blk)
            pos += 2 + n
        return out[off:off + length]

    def meta_stream(self, table, ref):
        blk, off = ref >> 16, ref & 0xffff
        return table + blk, off

    def inode(self, ref):
        s, o = self.meta_stream(self.it, ref)
        raw = self.meta(s, o, 64)
        e = self.e
        t, mode, uid, gid, mtime, ino = struct.unpack(e + 'HHHHII', raw[:16])
        r = {'type': t, 'mode': mode}
        if t == 1:
            sb, nl, fs, off, par = struct.unpack(e + 'IIHHI', raw[16:32])
            r.update(start=sb, size=fs, off=off)
        elif t == 8:
            nl, fs, sb, par, ic, off, x = struct.unpack(e + 'IIIIHHI', raw[16:40])
            r.update(start=sb, size=fs, off=off)
        elif t in (2, 9):
            if t == 2:
                sb, frag, foff, fs = struct.unpack(e + 'IIII', raw[16:32])
                hl = 32
            else:
                sb, fs, sp, nl, frag, foff, x = struct.unpack(e + 'QQQIIII', raw[16:56])
                hl = 56
            nblk = fs // self.bs if frag != 0xffffffff else (fs + self.bs - 1) // self.bs
            raw = self.meta(s, o, hl + 4 * nblk)
            bl = struct.unpack(e + '%dI' % nblk, raw[hl:hl + 4 * nblk])
            r.update(start=sb, size=fs, frag=frag, foff=foff, blocks=bl)
        elif t in (3, 10):
            nl, sz = struct.unpack(e + 'II', raw[16:24])
            raw = self.meta(s, o, 24 + sz)
            r.update(target=raw[24:24 + sz].decode(errors='replace'))
        return r

    def listdir(self, ino):
        if ino['size'] <= 3:
            return []
        data = self.meta(self.dt + ino['start'], ino['off'], ino['size'] - 3)
        e = self.e
        p = 0
        out = []
        while p < len(data):
            cnt, sb, inum = struct.unpack(e + 'IIi', data[p:p + 12])
            p += 12
            for _ in range(cnt + 1):
                off, idelta, t, ns = struct.unpack(e + 'HhHH', data[p:p + 8])
                name = data[p + 8:p + 9 + ns].decode(errors='replace')
                p += 9 + ns
                out.append((name, (sb << 16) | off, t))
        return out

    def frag(self, idx):
        if self._frag is None:
            n = self.nfrag
            nidx = (n + 511) // 512
            ptrs = struct.unpack(self.e + '%dQ' % nidx, self.d[self.ft:self.ft + 8 * nidx])
            ents = b''
            for ptr in ptrs:
                ents += self.meta(ptr, 0, min(8192, 16 * (n - len(ents) // 16)))
            self._frag = [struct.unpack(self.e + 'QII', ents[i * 16:i * 16 + 16]) for i in range(n)]
        return self._frag[idx]

    def read(self, ino):
        out = b''
        pos = ino['start']
        for b in ino['blocks']:
            n = b & 0xffffff
            if n == 0:
                out += b'\0' * self.bs
                continue
            blk = self.d[pos:pos + n]
            out += blk if b & (1 << 24) else self.dec(blk)
            pos += n
        if ino['frag'] != 0xffffffff:
            st, sz, _ = self.frag(ino['frag'])
            n = sz & 0xffffff
            blk = self.d[st:st + n]
            fb = blk if sz & (1 << 24) else self.dec(blk)
            out += fb[ino['foff']:ino['foff'] + ino['size'] - len(out)]
        return out[:ino['size']]

    def walk(self, ref=None, path=''):
        ino = self.inode(self.root if ref is None else ref)
        for name, r, t in self.listdir(ino):
            sub = self.inode(r)
            p = path + '/' + name
            yield p, sub
            if sub['type'] in (1, 8):
                yield from self.walk(r, p)

    def lookup(self, path):
        ref = self.root
        for part in [x for x in path.split('/') if x]:
            ino = self.inode(ref)
            for name, r, t in self.listdir(ino):
                if name == part:
                    ref = r
                    break
            else:
                raise FileNotFoundError(path)
        return self.inode(ref)


if __name__ == '__main__':
    img, off, cmd = sys.argv[1], int(sys.argv[2], 0), sys.argv[3]
    with open(img, 'rb') as f:
        f.seek(off)
        data = f.read()
    fs = Sqfs(data)
    if cmd == 'ls':
        for p, i in fs.walk():
            kind = {1: 'd', 8: 'd', 2: '-', 9: '-', 3: 'l', 10: 'l'}.get(i['type'], '?')
            extra = (' -> ' + i['target']) if 'target' in i else ''
            print(kind, i.get('size', 0) if kind == '-' else '', p + extra)
    elif cmd == 'cat':
        sys.stdout.buffer.write(fs.read(fs.lookup(sys.argv[4])))
