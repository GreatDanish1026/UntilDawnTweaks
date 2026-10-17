"""Minimal reader for unencrypted UE pak files (version 11): list and extract.

  python pak.py <file.pak> list "<glob>"
  python pak.py <file.pak> extract "<glob>" <out dir>

Oodle-compressed entries need oo2core_9_win64.dll, which isn't included (many games ship
it): point the OODLE_DLL environment variable at it. Until Dawn's config files are in
Bates-Windows.pak; the cooked assets are in the IoStore (.utoc/.ucas), which this doesn't
read.
"""
import ctypes
import fnmatch
import os
import struct
import sys

OODLE = os.environ.get('OODLE_DLL', 'oo2core_9_win64.dll')
PAK_MAGIC = 0x5A6F12E1


class R:
    def __init__(s, b, o=0): s.b, s.o = b, o
    def u32(s): v = struct.unpack_from('<I', s.b, s.o)[0]; s.o += 4; return v
    def i32(s): v = struct.unpack_from('<i', s.b, s.o)[0]; s.o += 4; return v
    def u64(s): v = struct.unpack_from('<Q', s.b, s.o)[0]; s.o += 8; return v
    def raw(s, n): v = s.b[s.o:s.o + n]; s.o += n; return v

    def fstr(s):  # FString: length with terminator; negative = UTF-16
        n = s.i32()
        if n == 0: return ''
        if n < 0: return s.raw(-n * 2).decode('utf-16le')[:-1]
        return s.raw(n)[:-1].decode('latin-1')


class Pak:
    def __init__(s, path):
        s.f = open(path, 'rb')
        s.f.seek(os.path.getsize(path) - 512)
        t = s.f.read()
        i = t.rfind(struct.pack('<I', PAK_MAGIC))
        if i < 1: raise RuntimeError('not a pak file')
        if t[i - 1]: raise RuntimeError('encrypted index')
        _, s.version, ioff, isz = struct.unpack_from('<IiQQ', t, i)
        if s.version != 11: print(f'warning: pak version {s.version}, written for 11', file=sys.stderr)
        j = i + 4 + 4 + 8 + 8 + 20
        s.methods = [t[j + k * 32:j + k * 32 + 32].rstrip(b'\0').decode() for k in range(5)]
        # Primary index: mount point, entry count, path hash seed, the two secondary
        # indexes' locations, then the encoded entries.
        s.f.seek(ioff)
        r = R(s.f.read(isz))
        s.mount = r.fstr()
        r.i32(); r.u64()
        if r.u32(): r.u64(); r.u64(); r.raw(20)  # path hash index
        if not r.u32(): raise RuntimeError('no full directory index')
        fdo = r.u64(); fds = r.u64(); r.raw(20)
        s.enc = r.raw(r.i32())
        # Full directory index: directory -> file name -> offset into the encoded entries.
        s.f.seek(fdo)
        d = R(s.f.read(fds))
        s.files = {}
        for _ in range(d.i32()):
            dname = d.fstr()
            for _ in range(d.i32()):
                fname = d.fstr()
                off = d.i32()
                if off >= 0:  # negative: an unencoded entry, which this doesn't support
                    s.files[(dname + fname).lstrip('/')] = off

    def entry(s, off):
        # FPakEntry bit-packed encoding (UE 4.25+).
        r = R(s.enc, off)
        v = r.u32()
        bs = v & 0x3f
        bs = r.u32() if bs == 0x3f else bs << 11
        nblocks = (v >> 6) & 0xffff
        enc = (v >> 22) & 1
        meth = (v >> 23) & 0x3f  # 1-based index into s.methods, 0 = stored
        offset = r.u32() if v & (1 << 31) else r.u64()
        usize = r.u32() if v & (1 << 30) else r.u64()
        size = (r.u32() if v & (1 << 29) else r.u64()) if meth else usize
        blocks = []
        if meth:
            # The serialized FPakEntry header in front of the data, then the blocks.
            pos = offset + 8 + 8 + 8 + 4 + 20 + 4 + 16 * nblocks + 1 + 4
            sizes = [size] if nblocks == 1 and not enc else [r.u32() for _ in range(nblocks)]
            for z in sizes:
                blocks.append((pos, z))
                pos += (z + 15) & ~15 if enc else z
        if bs == 0: bs = usize  # a single block holds the whole file
        return dict(offset=offset, usize=usize, size=size, meth=meth, bs=bs, blocks=blocks, enc=enc)

    def read(s, name):
        e = s.entry(s.files[name])
        if e['enc']: raise RuntimeError('encrypted entry')
        if not e['meth']:
            s.f.seek(e['offset'] + 8 + 8 + 8 + 4 + 20 + 1 + 4)
            return s.f.read(e['usize'])
        if s.methods[e['meth'] - 1] != 'Oodle': raise RuntimeError(f"unsupported compression {s.methods[e['meth'] - 1]}")
        fn = ctypes.WinDLL(OODLE).OodleLZ_Decompress
        fn.restype = ctypes.c_int64
        out = bytearray()
        left = e['usize']
        for pos, z in e['blocks']:
            s.f.seek(pos)
            src = s.f.read(z)
            want = min(e['bs'], left)
            dst = ctypes.create_string_buffer(want)
            got = fn(src, z, dst, want, 1, 0, 0, None, 0, None, None, None, 0, 3)
            if got != want: raise RuntimeError(f'oodle returned {got}, expected {want} ({name})')
            out += dst.raw
            left -= want
        return bytes(out)


if __name__ == '__main__':
    if len(sys.argv) < 4 or sys.argv[2] not in ('list', 'extract') or (sys.argv[2] == 'extract' and len(sys.argv) < 5):
        sys.exit(__doc__)
    p = Pak(sys.argv[1])
    pattern = sys.argv[3].lower()
    for n in p.files:
        if not fnmatch.fnmatch(n.lower(), pattern): continue
        if sys.argv[2] == 'list':
            print(n)
        else:
            dst = os.path.join(sys.argv[4], n)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, 'wb').write(p.read(n))
            print('extracted', n)
