#!/usr/bin/env python3
"""Virtual method tables of the IOAcceleratorFamily2 classes, from the symbol tables of this Mac's kernel and system kernel collection.

Reads data and symbol names only (nm, otool -l, the vtable words in the collection file); no code is disassembled. The collection stores pointers as chained
fixups (DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE): bits 0-29 the target as an offset from the base of the collection named by bits 30-31 (0: the kernel, 1: the system
collection itself). The kernel's offset base is found by comparing a slot every vtable inherits from OSObject with the kernel's own symbol address.

  accel_vtable.py CLASS...               every slot of the class: index, implementing function (demangled)
  accel_vtable.py --pure BASE DERIVED    the pure virtual slots of BASE, named by what the shipped class DERIVED puts there
  accel_vtable.py --markdown FILE        writes the document docs/M6_ACCEL_ABI.md
  accel_vtable.py --summary              slot and pure-virtual counts for the classes the M6 study lists
  accel_vtable.py --header DIR           writes DIR/IOGraphicsAccelerator2.h and DIR/IOGraphicsAccelerator2_pure.inc: a declaration of the class with its 84 virtuals in
                                         vtable order and the object padded to its size (3544 bytes, docs/M6_ACCEL_ABI.md), and the list of the 17 pure virtuals
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
    if r is None: return None
    words = list(r[1][2:])
    while words and words[-1] == 0: words.pop()      # the table is padded with a zero word before the next symbol
    return [name_of(w) for w in words]

RET_GUESS = {  # return types are not in the mangled names; the ABI does not need them, the stubs of the pure virtuals do
    'getStampMemory': 'void *', 'newEventMachine': 'void *', 'createUserGPUTask': 'void *', 'createKernelGPUTask': 'void *', 'newDisplayMachine': 'void *',
    'newGLContext': 'void *', 'newCLContext': 'void *', 'newSurface': 'void *', 'new2DContext': 'void *', 'newVideoContext': 'void *', 'newSysMemory': 'void *',
    'newVidMemory': 'void *', 'newResource': 'void *', 'newMemoryMap': 'void *', 'populateAccelConfig': 'bool', 'configureDevice': 'bool', 'teardownDevice': 'bool'}
GA2_SIZE, ACCELERATOR_SIZE = 3544, 136

def parse(demangled):
    # "Class::name(args) const" -> (name, args, const)
    head, _, rest = demangled.partition('(')
    name = head.split('::')[-1]
    args, _, tail = rest.rpartition(')')
    return name, args, tail.strip() == 'const'

def write_header(outdir):
    import os, re
    s = slots('IOGraphicsAccelerator2'); intel = slots('IntelAccelerator')
    rows, types = [], set()
    for i in range(266, len(s)):
        pure = s[i] and 'pure_virtual' in s[i]
        name, args, const = parse(dem(intel[i] if pure else s[i]))
        rows.append((i - 266, name, args, const, pure))
        for t in re.findall(r'\b(IO[A-Za-z0-9_]+|OS[A-Za-z0-9_]+|task|semaphore)\b', args): types.add(t)
    known = {'IOReturn', 'IOOptionBits', 'IOService', 'IOPCIDevice', 'IOFramebuffer', 'IOMemoryDescriptor', 'IOByteCount', 'OSObject', 'OSDictionary', 'OSArray', 'OSString', 'OSData', 'OSSet', 'OSNumber', 'OSSymbol', 'OSBoolean'}
    fwd = sorted(t for t in types if t not in known)
    h = ['// Generated by tools/accel_vtable.py from the symbol tables of this Mac\'s IOAcceleratorFamily2 (docs/M6_ACCEL_ABI.md). Do not edit.',
         '// IOGraphicsAccelerator2 as a third-party subclass has to see it: its 84 own virtual functions in vtable order after IOService\'s 266, the object padded to the',
         '// 3544 bytes the registry reports. The names and argument types are those of the exported symbols; return types are not in them and are guesses (void), which',
         '// does not matter for the vtable. The data members are not known: they are the padding.',
         '#pragma once', '#include <IOKit/IOService.h>', '#include <IOKit/IOMemoryDescriptor.h>', '#include <IOKit/graphics/IOAccelerator.h>', '#include <IOKit/graphics/IOFramebuffer.h>', '#include <IOKit/pci/IOPCIDevice.h>', '']
    for t in fwd: h.append('class %s;' % t)
    h += ['', 'class IOGraphicsAccelerator2 : public IOAccelerator {', '    OSDeclareAbstractStructors(IOGraphicsAccelerator2)', 'public:']
    h.append('    // %d bytes of IOAccelerator, then the members of the family class' % ACCELERATOR_SIZE)
    h.append('    uint8_t rn_ga2_opaque[%d];' % (GA2_SIZE - ACCELERATOR_SIZE))
    for k, name, args, const, pure in rows:
        ret = RET_GUESS.get(name, 'void') if pure else 'void'
        h.append('    virtual %s %s(%s)%s%s;   // slot %d' % (ret, name, args, ' const' if const else '', '' if not pure else ' = 0', 266 + k))
    h += ['};', '']
    open(os.path.join(outdir, 'IOGraphicsAccelerator2.h'), 'w').write('\n'.join(h))
    inc = ['// Generated by tools/accel_vtable.py: the pure virtual functions of IOGraphicsAccelerator2 (RN_PURE(return type, name, (arguments), const-qualifier)).']
    for k, name, args, const, pure in rows:
        if pure: inc.append('RN_PURE(%s, %s, (%s), %s)' % (RET_GUESS[name], name, args, 'const' if const else ''))
    open(os.path.join(outdir, 'IOGraphicsAccelerator2_pure.inc'), 'w').write('\n'.join(inc) + '\n')
    print('%d virtuals, %d pure, forward declared: %s' % (len(rows), sum(1 for r in rows if r[4]), ' '.join(fwd)))

def write_markdown(path):
    import os
    def table(cls, derived, start):
        sl = slots(cls); d = slots(derived)
        rows = ['| slot | method | |', '|---|---|---|']
        for i in range(start, len(sl)):
            n = sl[i]; pure = n and 'pure_virtual' in n
            rows.append('| %d | `%s` | %s |' % (i - start, dem(d[i]) if pure and i < len(d) else dem(n), '**pure**' if pure else ''))
        return '\n'.join(rows)
    def pure_only(base, derived):
        sl, d = slots(base), slots(derived)
        rows = ['| slot | method (as the shipped subclass implements it) |', '|---|---|']
        for i, n in enumerate(sl):
            if n and 'pure_virtual' in n: rows.append('| %d | `%s` |' % (i, dem(d[i]) if i < len(d) else '?'))
        return '\n'.join(rows)
    summary = subprocess.run([sys.executable, __file__, '--summary'], capture_output=True, text=True).stdout
    ga2 = slots('IOGraphicsAccelerator2'); n_own = len(ga2) - 266
    doc = ['# The virtual interface of IOAcceleratorFamily2, from symbol tables', '',
           'Generated by `tools/accel_vtable.py --markdown` on macOS 15.7.3 (kernel 24.6.0, `IOAcceleratorFamily2` 485), from the data and symbol names of `/System/Library/KernelCollections/SystemKernelExtensions.kc` and the kernel. Nothing was disassembled (see the method note in the tool). This is the information a third-party subclass needs to line up with the family\'s C++ ABI; the object sizes are the other half (section at the end, from the run-time probe of `RedNaviAccel`). Parameter types are as the compiler mangled them; return types are not in mangled names; the structures behind `IOAccelConfig`, `IOAccelEvent`, ... are not known beyond their names.', '',
           'Slot numbers count virtual functions in vtable order, from 0 after the two leading zero words, without the zero word that pads the table. IOService has 266 slots; `IOAccelerator` (IOGraphicsFamily) adds none, so a derived class\'s own virtuals start at slot 266.', '',
           '## Sizes of the tables', '', '```', summary.rstrip(), '```', '',
           '## IOGraphicsAccelerator2: the %d virtuals it adds to IOService (slots 266 to %d of the full table; 17 are pure virtual)' % (n_own, len(ga2) - 1), '',
           'The pure ones are named by what `IntelAccelerator` puts there (AMD\'s X6000 accelerator fills the same slots). Intel\'s table has one more slot of its own after the family\'s last (`newMemoryManager`), AMD\'s more.', '',
           table('IOGraphicsAccelerator2', 'IntelAccelerator', 266), '']
    for base, der in (('IOAccelSurface', 'IGAccelSurface'), ('IOAccel2DContext2', 'IGAccel2DContext'), ('IOAccelResource2', 'IGAccelResource'), ('IOAccelEventMachine2', 'IGAccelEventMachine')):
        doc += ['## %s: pure virtual slots' % base, '', pure_only(base, der), '']
    log = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'docs', 'logs', 'accel_first_load.txt')
    if os.path.exists(log):
        rows = [l.split() for l in open(log) if l.startswith('size ')]
        doc += ['## Object sizes (run-time)', '', '`OSMetaClass::getClassSize()` of each class, asked by `RedNaviAccel` on the test desktop (macOS 15.8.2, `IOAcceleratorFamily2` loaded as its dependency); raw output in `docs/logs/accel_first_load.txt`. Four classes the study lists are not OSObject classes and have no metaclass: `IOAccelClientSharedMachine`, `IOAccelOrphanedMemoryPool`, `IOAccelOrphanedVidMemoryPool`, `IOAccelResidentMemorySet`.', '',
                '| class | size (bytes) | parent |', '|---|---|---|'] + ['| `%s` | %s | `%s` |' % (r[1], r[2], r[3]) for r in sorted(rows, key=lambda r: r[1])] + ['']
    open(path, 'w').write('\n'.join(doc))

def main(argv):
    if not argv or argv[0] in ('-h', '--help'): print(__doc__); return
    if argv[0] == '--markdown':
        write_markdown(argv[1])
    elif argv[0] == '--header':
        write_header(argv[1])
    elif argv[0] == '--pure':
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
