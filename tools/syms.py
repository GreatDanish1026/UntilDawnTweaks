"""Symbol lookups in Until Dawn's shipped PDB (Bates-Win64-Shipping.pdb).

  llvm-pdbutil dump -publics Bates-Win64-Shipping.pdb > publics.txt
  python syms.py "<regex>"                symbols matching, with their RVAs
  python syms.py "<regex>" <n> [<slots>]  also list the vtable slots of the first n matches

UDRT_EXE (the exe) and UDRT_PUBLICS (the dump; default publics.txt next to this script)
locate the inputs; a pickle cache is written next to the dump. Section addresses are read
from the exe. The linker folds identical functions, so a slot can show an unrelated name:
disassemble to be sure (llvm-objdump -d --start-address=0x140000000+rva ...).
"""
import os
import pickle
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.environ.get('UDRT_EXE', r'E:\SteamLibrary\steamapps\common\Until Dawn\Windows\Bates\Binaries\Win64\Bates-Win64-Shipping.exe')
PUBLICS = os.environ.get('UDRT_PUBLICS', os.path.join(HERE, 'publics.txt'))

data = open(EXE, 'rb').read()
_pe = struct.unpack_from('<I', data, 0x3c)[0]
IMAGE_BASE = struct.unpack_from('<Q', data, _pe + 24 + 24)[0]
_nsec = struct.unpack_from('<H', data, _pe + 6)[0]
_opt = struct.unpack_from('<H', data, _pe + 20)[0]
SECTIONS = []  # (rva, raw offset, size), in PDB section order (1-based)
for _i in range(_nsec):
    _o = _pe + 24 + _opt + _i * 40
    vsize, rva, _, raw = struct.unpack_from('<IIII', data, _o + 8)
    SECTIONS.append((rva, raw, vsize))


def rva2off(r):
    for rva, raw, size in SECTIONS:
        if rva <= r < rva + size: return r - rva + raw
    raise ValueError(hex(r))


def load():
    cache = PUBLICS + '.pkl'
    if os.path.exists(cache): return pickle.load(open(cache, 'rb'))
    name = None
    by_name, by_rva = {}, {}
    for line in open(PUBLICS, encoding='utf-8', errors='replace'):
        m = re.search(r'S_PUB32 \[size = \d+\] `(.*)`', line)
        if m:
            name = m.group(1)
            continue
        m = re.search(r'addr = (\d{4}):(\d+)', line)
        if m and name:
            sec = int(m.group(1))
            if 1 <= sec <= len(SECTIONS):
                rva = SECTIONS[sec - 1][0] + int(m.group(2))
                by_name[name] = rva
                by_rva.setdefault(rva, name)
            name = None
    pickle.dump((by_name, by_rva), open(cache, 'wb'))
    return by_name, by_rva


by_name, by_rva = load()


def vtable(sym, n=40):
    r = by_name[sym]
    for i in range(n):
        p = struct.unpack_from('<Q', data, rva2off(r + i * 8))[0] - IMAGE_BASE
        print(f'  slot {i:2d} +0x{i * 8:03x}  {by_rva.get(p, hex(p))}')


if __name__ == '__main__':
    if len(sys.argv) < 2: sys.exit(__doc__)
    hits = [k for k in by_name if re.search(sys.argv[1], k)]
    for h in hits[:20]: print(hex(by_name[h]), h)
    if len(sys.argv) > 2:
        for h in hits[:int(sys.argv[2])]:
            print(h)
            vtable(h, int(sys.argv[3]) if len(sys.argv) > 3 else 40)
