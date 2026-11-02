#!/usr/bin/env python3
"""Virtual method tables of the IOAcceleratorFamily2 classes, from the symbol tables of this Mac's kernel and system kernel collection.

Reads data and symbol names only (nm, otool -l, the vtable words in the collection file); no code is disassembled. The collection stores pointers as chained
fixups (DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE): bits 0-29 the target as an offset from the base of the collection named by bits 30-31 (0: the kernel, 1: the system
collection itself). The kernel's offset base is found by comparing a slot every vtable inherits from OSObject with the kernel's own symbol address.

  accel_vtable.py CLASS...               every slot of the class: index, implementing function (demangled)
  accel_vtable.py --pure BASE DERIVED    the pure virtual slots of BASE, named by what the shipped class DERIVED puts there
  accel_vtable.py --summary              slot and pure-virtual counts for the classes the M6 study lists
Needs macOS 15 (the symbol layout is that of 24.6 kernels); run on a Mac that has the collection, no root needed.
"""
import bisect, struct, subprocess, sys

KC = '/System/Library/KernelCollections/SystemKernelExtensions.kc'
KERNEL = '/System/Library/Kernels/kernel'

def run(*a):
    return subprocess.run(a, capture_output=True, text=True).stdout

def load():
    segs, cur = [], None
    for l in run('otool', '-l', KC).splitlines():
        l = l.strip()
        if l.startswith('segname'): cur = {}
        elif cur is not None and l.startswith('vmaddr'): cur['vm'] = int(l.split()[1], 16)
        elif cur is not None and l.startswith('fileoff'): cur['fo'] = int(l.split()[1])
        elif cur is not None and l.startswith('filesize'):
            cur['fs'] = int(l.split()[1]); segs.append(cur); cur = None
    def syms(path):
        out = []
        for l in run('nm', '-n', '-arch', 'x86_64', path).splitlines():
            p = l.split()
            if len(p) == 3 and p[1] != 'U':
                try: out.append((int(p[0], 16), p[2]))
                except ValueError: pass
        out.sort(); return out
    return segs, syms(KC), syms(KERNEL)

SEGS, KCS, KERN = load()
KCA = [a for a, _ in KCS]
KERNEL_BY_ADDR = {a: n for a, n in KERN}

def fileoff(va):
    for s in SEGS:
        if s['fs'] and s['vm'] <= va < s['vm'] + s['fs']: return s['fo'] + va - s['vm']

def vtable_words(cls):
    sym = '__ZTV%d%s' % (len(cls), cls)
    for a, n in KCS:
        if n == sym:
            i = bisect.bisect_right(KCA, a)
            end = KCA[i] if i < len(KCA) else a + 0x400
            with open(KC, 'rb') as f:
                f.seek(fileoff(a)); return a, struct.unpack('<%dQ' % ((end - a) // 8), f.read(end - a))
    return None

def kernel_base():
    # slot 4 of every vtable is OSObject's; IOService's vtable lives in the kernel file itself
    iosvc = [a for a, n in KERN if n == '__ZTV9IOService'][0]
    # read IOService slot 4 from the kernel file
    cur, segs = None, []
    for l in run('otool', '-l', KERNEL).splitlines():
        l = l.strip()
        if l.startswith('segname'): cur = {}
        elif cur is not None and l.startswith('vmaddr'): cur['vm'] = int(l.split()[1], 16)
        elif cur is not None and l.startswith('fileoff'): cur['fo'] = int(l.split()[1])
        elif cur is not None and l.startswith('filesize'):
            cur['fs'] = int(l.split()[1]); segs.append(cur); cur = None
    for s in segs:
        if s['fs'] and s['vm'] <= iosvc < s['vm'] + s['fs']:
            with open(KERNEL, 'rb') as f:
                f.seek(s['fo'] + iosvc - s['vm'] + 8 * 4); absolute = struct.unpack('<Q', f.read(8))[0]
    other = vtable_words('IOGraphicsAccelerator2')[1][4] & 0x3fffffff
    return absolute - other

BASE = kernel_base()

def name_of(word):
    if word == 0: return None
    lvl, tgt = (word >> 30) & 3, word & 0x3fffffff
    n = KERNEL_BY_ADDR.get(BASE + tgt) if lvl == 0 else None
    if n is None:
        i = bisect.bisect_left(KCA, tgt)
        if i < len(KCA) and KCA[i] == tgt: n = KCS[i][1]
    return n

def dem(n):
    return run('c++filt', n).strip() if n and n.lstrip('_').startswith('Z') else (n or '?')

def slots(cls):
    r = vtable_words(cls)
    return None if r is None else [name_of(w) for w in r[1][2:]]

def main(argv):
    if not argv or argv[0] in ('-h', '--help'): print(__doc__); return
    if argv[0] == '--pure':
        base, derived = argv[1], argv[2]
        b, d = slots(base), slots(derived)
        print('%s: %d virtual slots, derived example %s' % (base, len(b), derived))
        for i, n in enumerate(b):
            if n and 'pure_virtual' in n: print('  slot %3d  %s' % (i, dem(d[i]) if d and i < len(d) else '?'))
    elif argv[0] == '--summary':
        for c in ('IOGraphicsAccelerator2', 'IOAccelDevice2', 'IOAccelSharedUserClient2', 'IOAccelCommandQueue', 'IOAccelSurface', 'IOAccel2DContext2', 'IOAccelDisplayPipe',
                  'IOAccelContext2', 'IOAccelResource2', 'IOAccelShared2', 'IOAccelChannel2', 'IOAccelMemory', 'IOAccelEventMachine2', 'IOAccelSubmitter2'):
            s = slots(c)
            print('%-28s %s' % (c, 'no vtable' if s is None else '%d slots, %d pure virtual' % (len(s), sum(1 for n in s if n and 'pure_virtual' in n))))
    else:
        for c in argv:
            s = slots(c)
            print('==', c, 'no vtable' if s is None else '%d slots' % len(s))
            for i, n in enumerate(s or []): print('%4d  %s' % (i, dem(n)))

if __name__ == '__main__':
    main(sys.argv[1:])
