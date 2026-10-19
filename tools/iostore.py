"""Minimal reader for unencrypted UE 5 IoStore containers (.utoc/.ucas, TOC version 5).

  python iostore.py <file.utoc> list "<glob>"            files with their chunk sizes
  python iostore.py <file.utoc> extract "<glob>" <dir>   write the files' chunks out
  python iostore.py <file.utoc> textures "<glob>"        Texture2D .uasset files: size, format

Oodle-compressed blocks need oo2core_9_win64.dll (OODLE_DLL, as for pak.py). Until Dawn's
cooked assets are in Bates-Windows.utoc; its directory index is not encrypted.

`textures` reads each .uasset chunk (the package's exports) and finds the cooked
FTexturePlatformData by its pixel format string: SizeX, SizeY and PackedData sit right
before it (UE 5 SerializeCooked). The .ubulk size is the streamed mips' bytes.
"""
import ctypes
import fnmatch
import os
import re
import struct
import sys

OODLE = os.environ.get('OODLE_DLL', 'oo2core_9_win64.dll')
MAGIC = b'-==--==--==--==-'


class Toc:
    def __init__(s, path):
        s.path = path
        d = open(path, 'rb').read()
        if d[:16] != MAGIC: raise ValueError('not a .utoc')
        s.version = d[16]
        (hsize, s.count, nblocks, bsize, nmeth, methlen, s.block_size, dirsize, partitions) = struct.unpack_from(
            '<IIIIIIIII', d, 20)
        flags = d[80]
        if flags & 2: raise ValueError('encrypted container')
        nseeds, = struct.unpack_from('<I', d, 84)
        noperfect, = struct.unpack_from('<I', d, 96) if s.version >= 5 else (0,)
        o = hsize
        s.ids = [d[o + 12 * i:o + 12 * i + 12] for i in range(s.count)]
        o += 12 * s.count
        s.offlen = []
        for i in range(s.count):
            b = d[o + 10 * i:o + 10 * i + 10]
            s.offlen.append((int.from_bytes(b[:5], 'big'), int.from_bytes(b[5:], 'big')))
        o += 10 * s.count
        o += 4 * nseeds + 4 * noperfect
        s.blocks = []
        for i in range(nblocks):
            b = d[o + bsize * i:o + bsize * i + bsize]
            v = int.from_bytes(b[:8], 'little')
            s.blocks.append((v & 0xFFFFFFFFFF, v >> 40, int.from_bytes(b[8:11], 'little'), b[11]))
        o += bsize * nblocks
        s.methods = [d[o + methlen * i:o + methlen * (i + 1)].split(b'\0')[0].decode() for i in range(nmeth)]
        o += methlen * nmeth
        if flags & 4: raise ValueError('signed containers not supported')
        s.files = {}  # path -> toc entry index
        if flags & 8: s._read_index(d[o:o + dirsize])
        s.ucas = open(path[:-5] + '.ucas', 'rb')
        s._oodle = None

    def _read_index(s, b):
        pos = [0]

        def u32():
            v, = struct.unpack_from('<I', b, pos[0]); pos[0] += 4; return v

        def fstr():
            n, = struct.unpack_from('<i', b, pos[0]); pos[0] += 4
            if n < 0:
                v = b[pos[0]:pos[0] - 2 * n].decode('utf-16-le').rstrip('\0'); pos[0] += -2 * n
            else:
                v = b[pos[0]:pos[0] + n].decode('utf-8', 'replace').rstrip('\0'); pos[0] += n
            return v

        mount = fstr().replace('../../../', '')
        dirs = [(u32(), u32(), u32(), u32()) for _ in range(u32())]
        files = [(u32(), u32(), u32()) for _ in range(u32())]
        strings = [fstr() for _ in range(u32())]
        none = 0xFFFFFFFF

        def walk(di, prefix):
            name, child, sibling, first = dirs[di]
            here = prefix + (strings[name] + '/' if name != none else '')
            f = first
            while f != none:
                fname, nxt, entry = files[f]
                s.files[here + strings[fname]] = entry
                f = nxt
            c = child
            while c != none:
                walk(c, here)
                c = dirs[c][2]

        walk(0, mount)

    def size(s, entry): return s.offlen[entry][1]

    def read(s, entry, limit=None):
        off, length = s.offlen[entry]
        if limit is not None: length = min(length, limit)
        first, last = off // s.block_size, (off + length - 1) // s.block_size
        out = bytearray()
        for bi in range(first, last + 1):
            boff, csize, usize, meth = s.blocks[bi]
            s.ucas.seek(boff)
            src = s.ucas.read((csize + 15) & ~15 if False else csize)
            if meth == 0:
                out += src[:usize]
                continue
            if s.methods[meth - 1] != 'Oodle': raise RuntimeError('unsupported compression ' + s.methods[meth - 1])
            if not s._oodle:
                s._oodle = ctypes.WinDLL(OODLE).OodleLZ_Decompress
                s._oodle.restype = ctypes.c_int64
            dst = ctypes.create_string_buffer(usize)
            got = s._oodle(src, csize, dst, usize, 1, 0, 0, None, 0, None, None, None, 0, 3)
            if got != usize: raise RuntimeError(f'oodle returned {got}, expected {usize}')
            out += dst.raw
        start = off - first * s.block_size
        return bytes(out[start:start + length])


PF = re.compile(rb'(?:\x07|\x08|\x09|\x0a|\x0b|\x0c|\x0d|\x0e|\x0f|\x10|\x11|\x12|\x13|\x14)\x00\x00\x00(PF_[A-Za-z0-9_]+)\x00')


def texture_info(data):
    """(SizeX, SizeY, PixelFormat) of the first cooked platform data in a package, or None."""
    for m in PF.finditer(data):
        fmt = m.group(1).decode()
        p = m.start()
        if p < 12: continue
        x, y, packed = struct.unpack_from('<iiI', data, p - 12)
        if 0 < x <= 16384 and 0 < y <= 16384: return x, y, fmt, packed & 0x3FFFFFFF
    return None


if __name__ == '__main__':
    if len(sys.argv) < 4 or sys.argv[2] not in ('list', 'extract', 'textures'):
        sys.exit(__doc__)
    t = Toc(sys.argv[1])
    pat = sys.argv[3].lower()
    names = [n for n in t.files if fnmatch.fnmatch(n.lower(), pat)]
    if sys.argv[2] == 'list':
        for n in names: print('%12d  %s' % (t.size(t.files[n]), n))
    elif sys.argv[2] == 'extract':
        for n in names:
            dst = os.path.join(sys.argv[4], n)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, 'wb').write(t.read(t.files[n]))
            print('extracted', n)
    else:
        for n in names:
            if not n.endswith('.uasset'): continue
            info = texture_info(t.read(t.files[n]))
            if not info: continue
            bulk = t.files.get(n[:-7] + '.ubulk')
            print('%5dx%-5d %-14s ubulk %9d  %s' % (info[0], info[1], info[2], t.size(bulk) if bulk is not None else 0, n))
