#!/usr/bin/env python3
"""Virtual method tables of the IOAcceleratorFamily2 classes, from the symbol tables of this Mac's kernel and system kernel collection.

Reads data and symbol names only (nm, otool -l, the vtable words in the collection file); no code is disassembled. The collection stores pointers as chained
fixups (DYLD_CHAINED_PTR_X86_64_KERNEL_CACHE): bits 0-29 the target as an offset from the base of the collection named by bits 30-31 (0: the kernel, 1: the system
collection itself). The kernel's offset base is found by comparing a slot every vtable inherits from OSObject with the kernel's own symbol address.

  accel_vtable.py CLASS...               every slot of the class: index, implementing function (demangled)
  accel_vtable.py --pure BASE DERIVED    the pure virtual slots of BASE, named by what the shipped class DERIVED puts there
  accel_vtable.py --markdown FILE        writes the document docs/M6_ACCEL_ABI.md
  accel_vtable.py --summary              slot and pure-virtual counts for the classes the M6 study lists
  accel_vtable.py --header-chain HEADER STUBS   writes IOAccelClasses.h and the stub subclasses (see CHAIN, write_chain)
  accel_vtable.py --header-class FILE CLASS PARENT BASE_SLOTS PARENT_SIZE SIZE DERIVED   (see write_class_header)
  accel_vtable.py --header-event FILE    writes the declaration of IOAccelEventMachine2 and IOAccelEventMachineFast2 (see write_event_header)
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
    'newVidMemory': 'void *', 'newResource': 'void *', 'newMemoryMap': 'void *', 'populateAccelConfig': 'void', 'configureDevice': 'bool', 'teardownDevice': 'bool'}
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
    # IOService virtuals the family overrides (slots 184 and 185, docs/M6_ACCEL_ABI.md): declared so that a subclass can call the family's own start() and stop()
    h += ['    bool start(IOService *) APPLE_KEXT_OVERRIDE;', '    void stop(IOService *) APPLE_KEXT_OVERRIDE;', '};', '']
    open(os.path.join(outdir, 'IOGraphicsAccelerator2.h'), 'w').write('\n'.join(h))
    inc = ['// Generated by tools/accel_vtable.py: the pure virtual functions of IOGraphicsAccelerator2 (RN_PURE(return type, name, (arguments), const-qualifier)).']
    for k, name, args, const, pure in rows:
        if pure: inc.append('RN_PURE(%s, %s, (%s), %s)' % (RET_GUESS[name], name, args, 'const' if const else ''))
    open(os.path.join(outdir, 'IOGraphicsAccelerator2_pure.inc'), 'w').write('\n'.join(inc) + '\n')
    print('%d virtuals, %d pure, forward declared: %s' % (len(rows), sum(1 for r in rows if r[4]), ' '.join(fwd)))

def write_event_header(path):
    """IOAccelEventMachine2 (240 bytes, parent OSObject, own virtuals from slot 35) and IOAccelEventMachineFast2 (3376 bytes, 95 slots, its 4 pure virtuals at 84-87)
    declared so that a third-party subclass gets the family's vtable: every slot that Fast2 implements is re-declared in Fast2 (the subclass's vtable then points at the
    family's exported function instead of __cxa_pure_virtual). Return types are guesses (void); the 4 pure ones are declared long, see RedNaviAccel2."""
    import re
    OS_SLOTS, M2_SLOTS, F2_SLOTS, M2_SIZE, F2_SIZE, OBJ = 35, 84, 95, 240, 3376, 16
    m2, f2, ig = slots('IOAccelEventMachine2'), slots('IOAccelEventMachineFast2'), slots('IGAccelEventMachine')
    def decl(i, ret, pure):
        name, args, const = parse(dem(ig[i] if pure else f2[i]))
        return name, args, const, '    virtual %s %s(%s)%s%s;   // slot %d' % (ret, name, args, ' const' if const else '', ' = 0' if pure else '', i)
    types, base, derived = set(), [], []
    for i in range(OS_SLOTS, F2_SLOTS):
        f2pure = 'pure_virtual' in f2[i]
        name, args, const, line = decl(i, 'long' if f2pure else ('bool' if i == 35 else 'void'), f2pure)
        for t in re.findall(r'\b(IO[A-Za-z0-9_]+|vendevt[A-Za-z]+)\b', args): types.add(t)
        if i < M2_SLOTS:
            base.append(decl(i, 'bool' if i == 35 else 'void', 'pure_virtual' in m2[i])[3])
            if dem(f2[i]).startswith('IOAccelEventMachineFast2::') and not f2pure: derived.append(line)
        else:
            derived.append(line)
    h = ['// Generated by tools/accel_vtable.py --header-event from the symbol tables of this Mac\'s IOAcceleratorFamily2 (docs/M6_ACCEL_ABI.md). Do not edit.',
         '// IOAccelEventMachine2 and IOAccelEventMachineFast2 as a third-party subclass has to see them; data members are padding. Return types are guesses.',
         '#pragma once', '#include <libkern/c++/OSObject.h>', '#include <IOKit/IOTypes.h>', '']
    for t in sorted(types - {'IOGraphicsAccelerator2'}): h.append('class %s;' % t)
    h += ['class IOGraphicsAccelerator2;', '',
          'class IOAccelEventMachine2 : public OSObject {', '    OSDeclareAbstractStructors(IOAccelEventMachine2)', 'public:', '    uint8_t rn_em2_opaque[%d];' % (M2_SIZE - OBJ)] + base + ['};', '',
          'class IOAccelEventMachineFast2 : public IOAccelEventMachine2 {', '    OSDeclareAbstractStructors(IOAccelEventMachineFast2)', 'public:', '    uint8_t rn_emf2_opaque[%d];' % (F2_SIZE - M2_SIZE)] + derived + ['};', '']
    open(path, 'w').write('\n'.join(h))
    print('event machine header: %d own slots in Machine2, %d re-declared or new in Fast2' % (len(base), len(derived)))

def write_class_header(path, cls, parent, base_slots, parent_size, size, derived):
    """A class that derives from an SDK class (base_slots virtuals: OSObject 35, IOService 266) declared with its own virtuals in vtable order, the object padded to its
    registry size. Pure virtuals are named by the shipped subclass `derived` and declared long. kxld patches the entries a subclass does not override from the family's
    real vtable, so only the class's own slots need declaring."""
    import re
    sl, ds = slots(cls), slots(derived)
    types, lines = set(), []
    for i in range(base_slots, len(sl)):
        pure = 'pure_virtual' in sl[i]
        name, args, const = parse(dem(ds[i] if pure else sl[i]))
        for t in re.findall(r'\b(IO[A-Za-z0-9_]+|vendevt[A-Za-z0-9]+)\b', args): types.add(t)
        lines.append('    virtual %s %s(%s)%s%s;   // slot %d' % ('long' if pure else 'void', name, args, ' const' if const else '', ' = 0' if pure else '', i))
    known = {'IOService', 'IOPCIDevice', 'IOFramebuffer', 'IOMemoryDescriptor', 'IOUserClient'}
    h = ['// Generated by tools/accel_vtable.py --header-class from the symbol tables of this Mac\'s IOAcceleratorFamily2 (docs/M6_ACCEL_ABI.md). Do not edit.',
         '// %s as a third-party subclass has to see it: parent %s, %d bytes, own virtuals in vtable order; data members are padding, return types are guesses.' % (cls, parent, size),
         '#pragma once', '#include <IOKit/IOService.h>', '#include <IOKit/IOUserClient.h>', '#include <IOKit/pci/IOPCIDevice.h>', '#include <IOKit/graphics/IOFramebuffer.h>', '']
    for t in sorted(types - known): h.append('class %s;' % t)
    h += ['', 'class %s : public %s {' % (cls, parent), '    OSDeclareAbstractStructors(%s)' % cls, 'public:', '    uint8_t rn_pad_%s[%d];' % (cls, size - parent_size)] + lines + ['};', '']
    open(path, 'w').write('\n'.join(h))
    print('%s: %d own virtuals, %d pure' % (cls, len(lines), sum(1 for l in lines if '= 0' in l)))

# (class, parent, parent's slot count, parent's size, own size, shipped subclasses that name the pure slots, stub?) in declaration order. Slot counts of the SDK parents:
# OSObject 35, IOUserClient 300 (counted with -fdump-vtable-layouts on a subclass). Sizes are the registry's (docs/M6_ACCEL_ABI.md).
CHAIN = [
    ('IOAccelMemory', 'OSObject', 35, 16, 176, ['IOAccelSysMemory'], False),
    ('IOAccelVidMemory', 'IOAccelMemory', None, 176, 272, ['AMDRadeonX6000_AMDAccelVidMemory', 'IOAccelSysMemory'], True),
    ('IOAccelResource2', 'OSObject', 35, 16, 384, ['IGAccelResource'], True),
    ('IOAccelMemoryMap', 'OSObject', 35, 16, 280, ['IGAccelMemoryMap'], True),
    ('IOAccelSubmitter2', 'IOUserClient', 300, 216, 1384, ['IGAccelGLContext'], False),
    ('IOAccelContext2', 'IOAccelSubmitter2', None, 1384, 4264, ['IGAccelGLContext'], False),
    ('IOAccelGLContext2', 'IOAccelContext2', None, 4264, 4664, ['IGAccelGLContext'], True),
    ('IOAccel2DContext2', 'IOAccelContext2', None, 4264, 4352, ['IGAccel2DContext'], True),
    ('IOAccelSurface', 'IOUserClient', 300, 216, 4920, ['IGAccelSurface'], True),
]

def write_chain(hpath, spath):
    """Declarations of the classes behind the family's memory, resource, context and surface factories, and a stub subclass of each one that has pure virtuals of its
    own (all return 0 and log their first call: they are only here so that the family can allocate the objects)."""
    import re
    decl = {}      # class -> {slot: (name, args, const, ret, pure)} as declared
    nslots = {}
    types, hl, sl = set(), [], []
    known = {'IOService', 'IOPCIDevice', 'IOFramebuffer', 'IOMemoryDescriptor', 'IOUserClient', 'IOExternalMethodArguments', 'IOExternalMethodDispatch', 'IOAccelSurfaceInformation', 'IOAccelSurfaceScaling'}
    for cls, parent, pslots, psize, size, ders, stub in CHAIN:
        t = slots(cls)
        dts = [slots(d) for d in ders]
        pdecl = decl.get(parent, {})
        if pslots is None: pslots = nslots[parent]
        nslots[cls] = len(t)
        mine = {i: v for i, v in pdecl.items()}
        lines, over = [], []
        for i in range(pslots, len(t)):
            pure = 'pure_virtual' in t[i]
            src = t[i]
            if pure:
                src = next((d[i] for d in dts if d and i < len(d) and 'pure_virtual' not in d[i]), None)
                if src is None: raise SystemExit('no name for pure slot %d of %s' % (i, cls))
            name, args, const = parse(dem(src))
            ret = 'long' if pure else 'void'
            mine[i] = (name, args, const, ret, pure)
            lines.append('    virtual %s %s(%s)%s%s;   // slot %d' % (ret, name, args, ' const' if const else '', ' = 0' if pure else '', i))
            for tt in re.findall(r'\b(IO[A-Za-z0-9_]+|vendevt[A-Za-z0-9]+|e[A-Z][A-Za-z0-9]+)\b', args): types.add(tt)
        for i, (name, args, const, ret, pure) in pdecl.items():      # inherited pure virtuals that this class implements, and ones it still leaves pure
            if not pure: continue
            if 'pure_virtual' in t[i]: continue
            n2, a2, c2 = parse(dem(t[i]))
            mine[i] = (n2, a2, c2, ret, False)
            over.append('    virtual %s %s(%s)%s;   // slot %d, implemented by the family' % (ret, n2, a2, ' const' if c2 else '', i))
        decl[cls] = mine
        hl += ['class %s : public %s {' % (cls, parent), '    OSDeclareAbstractStructors(%s)' % cls, 'public:', '    uint8_t rn_pad_%s[%d];' % (cls, size - psize)] + over + lines + ['};', '']
        if stub:
            sname = 'RedNavi' + cls[len('IOAccel'):]
            pures = sorted(i for i, v in mine.items() if v[4])
            sl += ['class %s : public %s {' % (sname, cls), '    OSDeclareDefaultStructors(%s)' % sname, 'public:']
            for i in pures:
                name, args, const, ret, _ = mine[i]
                sl.append('    %s %s(%s)%s override;' % (ret, name, args, ' const' if const else ''))
            sl += ['    virtual void rnPad0(); virtual void rnPad1(); virtual void rnPad2(); virtual void rnPad3();', '};',
                   'static_assert(sizeof(%s) == %d, "%s is %d bytes in the registry");' % (sname, size, cls, size),
                   'OSDefineMetaClassAndStructors(%s, %s)' % (sname, cls),
                   'void %s::rnPad0() {} void %s::rnPad1() {} void %s::rnPad2() {} void %s::rnPad3() {}' % ((sname,) * 4)]
            for i in pures:
                name, args, const, ret, _ = mine[i]
                sl.append('%s %s::%s(%s)%s { static bool seen; if (!seen) { seen = true; rnNote("%s::%s called (stub, returns 0)"); } return 0; }' % (ret, sname, name, args, ' const' if const else '', sname, name))
            sl.append('')
    h = ['// Generated by tools/accel_vtable.py --header-chain from the symbol tables of this Mac\'s IOAcceleratorFamily2 (docs/M6_ACCEL_ABI.md). Do not edit.',
         '// The memory, resource, context and surface classes of the family as a third-party subclass has to see them: parents, registry sizes, own virtuals in vtable',
         '// order, pure ones declared long. Data members are padding, return types are guesses.',
         '#pragma once', '#include <IOKit/IOService.h>', '#include <IOKit/IOUserClient.h>', '#include <IOKit/IOMemoryDescriptor.h>', '#include <IOKit/graphics/IOAccelTypes.h>', '']
    for tt in sorted(types - known - {c[0] for c in CHAIN}): h.append(('enum %s : int {};' if re.match(r'e[A-Z]', tt) else 'class %s;') % tt)
    open(hpath, 'w').write('\n'.join(h + [''] + hl))
    open(spath, 'w').write('// Generated by tools/accel_vtable.py --header-chain: stub subclasses for the abstract classes of IOAccelClasses.h. Included by kern_accel2.cpp after rnNote.\n' + '\n'.join(sl))
    print('chain: %d classes declared, %d stubs' % (len(CHAIN), sum(1 for c in CHAIN if c[6])))

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
    elif argv[0] == '--header-chain':
        write_chain(argv[1], argv[2])
    elif argv[0] == '--header-class':
        write_class_header(argv[1], argv[2], argv[3], int(argv[4]), int(argv[5]), int(argv[6]), argv[7])
    elif argv[0] == '--header-event':
        write_event_header(argv[1])
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
