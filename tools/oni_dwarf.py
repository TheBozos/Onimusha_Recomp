"""oni_dwarf.py <SLUS_200.18> <out.h> [--grep TEXT]: the game's DWARF 1 (.debug, CodeWarrior) as C declarations.

SLUS_200.18 keeps its compiler's debugging information: every structure, union, enumeration and typedef, every
function with its parameters and locals (register or stack slot), every global and static with its address. This writes
them as a C-like header (one compile unit after the other) so a port reads the game's own types instead of guessing:

  struct SPRT { /* 0x28 */ ... };
  /* 0x13f1c0-0x13f4c8 */ void title_init(struct TASK *tp /* $a0 */) { int i /* sp+0x10 */; }
  /* 0x281e08 */ struct SPRT spr_work[...];

The output is game-derived (names, layouts): keep it in build/, never commit it. `--grep TEXT` prints only the
compile units, types and functions whose text contains TEXT.

DWARF 1: a sequence of entries (4-byte length, 2-byte tag, attributes: 2-byte name whose low 4 bits are the form).
"""
import struct
import sys

TAGS = {0: 'padding', 1: 'array_type', 2: 'class_type', 3: 'entry_point', 4: 'enumeration_type', 5: 'formal_parameter',
        6: 'global_subroutine', 7: 'global_variable', 0xa: 'label', 0xb: 'lexical_block', 0xc: 'local_variable',
        0xd: 'member', 0xf: 'pointer_type', 0x10: 'reference_type', 0x11: 'compile_unit', 0x12: 'string_type',
        0x13: 'structure_type', 0x14: 'subroutine', 0x15: 'subroutine_type', 0x16: 'typedef', 0x17: 'union_type',
        0x18: 'unspecified_parameters', 0x19: 'variant', 0x1d: 'inlined_subroutine', 0x21: 'subrange_type'}
AT = {0x01: 'sibling', 0x02: 'location', 0x03: 'name', 0x05: 'fund_type', 0x06: 'mod_fund_type', 0x07: 'user_def_type',
      0x08: 'mod_u_d_type', 0x09: 'ordering', 0x0a: 'subscr_data', 0x0b: 'byte_size', 0x0c: 'bit_offset',
      0x0d: 'bit_size', 0x0f: 'element_list', 0x10: 'stmt_list', 0x11: 'low_pc', 0x12: 'high_pc', 0x13: 'language',
      0x1c: 'const_value', 0x2f: 'upper_bound', 0x22: 'lower_bound'}
FUND = {1: 'char', 2: 'signed char', 3: 'unsigned char', 4: 'short', 5: 'short', 6: 'unsigned short', 7: 'int',
        8: 'int', 9: 'unsigned int', 0xa: 'long', 0xb: 'long', 0xc: 'unsigned long', 0xd: 'void *', 0xe: 'float',
        0xf: 'double', 0x10: 'long double', 0x14: 'void', 0x15: 'bool', 0x8008: 'long long', 0x8208: 'unsigned long long',
        0x8108: 'long long', 0x8408: 'u_long128', 0x8508: 'long128'}
REGS = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7', 's0', 's1', 's2',
        's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']


class Entry:
    __slots__ = ('off', 'tag', 'attrs', 'children', 'parent')

    def __init__(self, off, tag, attrs):
        self.off, self.tag, self.attrs, self.children, self.parent = off, tag, attrs, [], None


def read_entries(data, base):
    entries = {}
    order = []
    pos = 0
    while pos + 6 <= len(data):
        length, = struct.unpack_from('<I', data, pos)
        if length < 8:
            pos += max(length, 4)
            continue
        tag, = struct.unpack_from('<H', data, pos + 4)
        attrs = {}
        p, end = pos + 6, pos + length
        while p + 2 <= end:
            name, = struct.unpack_from('<H', data, p)
            p += 2
            form = name & 0xf
            if form == 1 or form == 2 or form == 6:
                value, = struct.unpack_from('<I', data, p); p += 4
            elif form == 3:
                n, = struct.unpack_from('<H', data, p); p += 2; value = data[p:p + n]; p += n
            elif form == 4:
                n, = struct.unpack_from('<I', data, p); p += 4; value = data[p:p + n]; p += n
            elif form == 5:
                value, = struct.unpack_from('<H', data, p); p += 2
            elif form == 7:
                value, = struct.unpack_from('<Q', data, p); p += 8
            elif form == 8:
                z = data.index(b'\0', p); value = data[p:z].decode('latin-1'); p = z + 1
            else:
                break
            attrs[name >> 4] = value
        e = Entry(base + pos, tag, attrs)
        entries[e.off] = e
        order.append(e)
        pos += length
    return entries, order


def build_tree(entries, order):
    """DWARF 1 nests by sibling links: an entry's children follow it until its sibling."""
    roots = []
    stack = []  # (entry, sibling offset)
    for e in order:
        while stack and stack[-1][1] is not None and e.off >= stack[-1][1]:
            stack.pop()
        if stack:
            e.parent = stack[-1][0]
            stack[-1][0].children.append(e)
        else:
            roots.append(e)
        sib = e.attrs.get(0x01)
        if e.tag != 0 and sib is not None and sib > e.off:
            # It may have children: they follow it up to its sibling.
            stack.append((e, sib))
    return roots


def location(block):
    """A location expression: member offset, register, frame slot or address."""
    ops = []
    p = 0
    while p < len(block):
        op = block[p]; p += 1
        if op in (1, 2, 3, 4):
            v, = struct.unpack_from('<I', block, p); p += 4
            ops.append((op, v))
        else:
            ops.append((op, None))
    if len(ops) == 2 and ops[0][0] == 4 and ops[1][0] == 7:
        return ('offset', ops[0][1])
    if len(ops) == 1 and ops[0][0] == 1:
        return ('reg', ops[0][1])
    if len(ops) == 1 and ops[0][0] == 3:
        return ('addr', ops[0][1])
    if len(ops) >= 3 and ops[0][0] == 2 and ops[1][0] == 4 and ops[2][0] == 7:
        return ('base', (ops[0][1], ops[1][1]))
    if len(ops) == 1 and ops[0][0] == 4:
        return ('offset', ops[0][1])
    return ('expr', ops)


class Namer:
    def __init__(self, entries):
        self.entries = entries

    def type_of(self, e, name=''):
        a = e.attrs
        if 0x05 in a:
            return self.decl(FUND.get(a[0x05], f'fund_{a[0x05]:#x}'), name)
        if 0x07 in a:
            return self.decl(self.ref_name(a[0x07]), name)
        if 0x06 in a:
            return self.modified(a[0x06], True, name)
        if 0x08 in a:
            return self.modified(a[0x08], False, name)
        return self.decl('int', name)

    @staticmethod
    def decl(base, name):
        return f'{base} {name}'.rstrip()

    def modified(self, block, fund, name):
        mods = []
        p = 0
        n = len(block) - (2 if fund else 4)
        while p < n:
            mods.append(block[p]); p += 1
        if fund:
            base = FUND.get(struct.unpack_from('<H', block, n)[0], 'fund?')
        else:
            base = self.ref_name(struct.unpack_from('<I', block, n)[0])
        out = name
        for m in mods:
            if m == 1:
                out = '*' + out
            elif m == 2:
                out = '&' + out
            elif m == 3:
                out = 'const ' + out
            elif m == 4:
                out = 'volatile ' + out
        return f'{base} {out}'.rstrip()

    def ref_name(self, off):
        e = self.entries.get(off)
        if e is None:
            return f'type_{off:x}'
        nm = e.attrs.get(0x03)
        kind = {0x13: 'struct ', 0x17: 'union ', 0x04: 'enum ', 0x02: 'class '}.get(e.tag, '')
        if nm:
            return kind + nm
        if e.tag == 0x01:
            return self.array(e)
        if e.tag == 0x15:
            return 'void (*)()'
        return f'{kind}anon_{off:x}'

    def array(self, e):
        block = e.attrs.get(0x0a, b'')
        dims = []
        p = 0
        element = 'char'
        while p < len(block):
            fmt = block[p]; p += 1
            if fmt in (0, 1):  # FMT_FT_C_C / FMT_FT_C_X: index type, then constant bounds
                p += 2
                lo, hi = struct.unpack_from('<ii', block, p); p += 8
                dims.append(hi - lo + 1)
            elif fmt == 8:  # FMT_ET: the element type, an attribute
                at, = struct.unpack_from('<H', block, p); p += 2
                form = at & 0xf
                fake = Entry(0, 0, {})
                if form == 5:
                    fake.attrs[at >> 4], = struct.unpack_from('<H', block, p); p += 2
                elif form in (1, 2, 6):
                    fake.attrs[at >> 4], = struct.unpack_from('<I', block, p); p += 4
                elif form == 3:
                    n, = struct.unpack_from('<H', block, p); p += 2
                    fake.attrs[at >> 4] = block[p:p + n]; p += n
                element = self.type_of(fake)
                break
            else:
                break
        return element + ''.join(f'[{d}]' for d in dims)


def describe(namer, e, indent=''):
    a = e.attrs
    name = a.get(0x03, '')
    out = []
    if e.tag in (0x13, 0x17, 0x02):
        kind = {0x13: 'struct', 0x17: 'union', 0x02: 'class'}[e.tag]
        size = a.get(0x0b)
        out.append(f'{indent}{kind} {name or "anon_%x" % e.off} {{ /* {size:#x} */' if size is not None else
                   f'{indent}{kind} {name or "anon_%x" % e.off} {{')
        for c in e.children:
            if c.tag == 0x0d:
                loc = location(c.attrs[0x02]) if 0x02 in c.attrs else ('?', 0)
                off = f'{loc[1]:#x}' if loc[0] == 'offset' else '?'
                bits = f' : {c.attrs[0x0d]} (bit {c.attrs.get(0x0c, 0)})' if 0x0d in c.attrs else ''
                out.append(f'{indent}    /* {off} */ {namer.type_of(c, c.attrs.get(0x03, ""))}{bits};')
        out.append(f'{indent}}};')
    elif e.tag == 0x04:
        out.append(f'{indent}enum {name or "anon_%x" % e.off} {{')
        block = a.get(0x0f, b'')
        p = 0
        while p + 4 <= len(block):
            v, = struct.unpack_from('<i', block, p); p += 4
            z = block.index(b'\0', p); n = block[p:z].decode('latin-1'); p = z + 1
            out.append(f'{indent}    {n} = {v},')
        out.append(f'{indent}}};')
    elif e.tag == 0x16:
        out.append(f'{indent}typedef {namer.type_of(e, name)};')
    elif e.tag in (0x06, 0x14):
        lo, hi = a.get(0x11), a.get(0x12)
        params = [c for c in e.children if c.tag == 0x05]
        ps = []
        for c in params:
            loc = location(c.attrs[0x02]) if 0x02 in c.attrs else None
            where = ''
            if loc and loc[0] == 'reg':
                where = f' /* ${REGS[loc[1]] if loc[1] < 32 else "f%d" % (loc[1] - 32)} */'
            elif loc and loc[0] == 'base':
                where = f' /* {REGS[loc[1][0]] if loc[1][0] < 32 else loc[1][0]}+{loc[1][1]:#x} */'
            ps.append(namer.type_of(c, c.attrs.get(0x03, '')) + where)
        span = f'/* {lo:#x}-{hi:#x} */ ' if lo is not None and hi is not None else ''
        static = 'static ' if e.tag == 0x14 else ''
        out.append(f'{indent}{span}{static}{namer.type_of(e, name)}({", ".join(ps)})')
        locals_ = []
        for c in e.children:
            if c.tag == 0x0c:
                loc = location(c.attrs[0x02]) if 0x02 in c.attrs else None
                where = ''
                if loc and loc[0] == 'reg':
                    where = f' /* ${REGS[loc[1]] if loc[1] < 32 else "f%d" % (loc[1] - 32)} */'
                elif loc and loc[0] == 'base':
                    where = f' /* {REGS[loc[1][0]] if loc[1][0] < 32 else loc[1][0]}+{loc[1][1]:#x} */'
                elif loc and loc[0] == 'addr':
                    where = f' /* {loc[1]:#x} */'
                locals_.append(f'{indent}    {namer.type_of(c, c.attrs.get(0x03, ""))};{where}')
        out.append(f'{indent}{{' if locals_ else f'{indent};')
        if locals_:
            out += locals_ + [f'{indent}}}']
    elif e.tag == 0x07 or (e.tag == 0x0c and e.parent is not None and e.parent.tag == 0x11):
        loc = location(a[0x02]) if 0x02 in a else None
        where = f'/* {loc[1]:#x} */ ' if loc and loc[0] == 'addr' else ''
        static = '' if e.tag == 0x07 else 'static '
        out.append(f'{indent}{where}{static}{namer.type_of(e, name)};')
    return out


def main():
    elf = open(sys.argv[1], 'rb').read()
    out_path = sys.argv[2]
    grep = sys.argv[sys.argv.index('--grep') + 1] if '--grep' in sys.argv else None
    shoff, = struct.unpack_from('<I', elf, 0x20)
    shnum, = struct.unpack_from('<H', elf, 0x30)
    shstr, = struct.unpack_from('<H', elf, 0x32)
    sh = [struct.unpack_from('<10I', elf, shoff + i * 40) for i in range(shnum)]
    names = [elf[sh[shstr][4] + s[0]:elf.index(b'\0', sh[shstr][4] + s[0])].decode() for s in sh]
    debug = sh[names.index('.debug')]
    data = elf[debug[4]:debug[4] + debug[5]]
    entries, order = read_entries(data, 0)
    roots = build_tree(entries, order)
    namer = Namer(entries)
    lines = []
    for unit in roots:
        if unit.tag != 0x11:
            continue
        block = [f'/* ==== {unit.attrs.get(0x03, "?")} ==== */']
        for e in unit.children:
            block += describe(namer, e)
        text = '\n'.join(block)
        if grep is None:
            lines.append(text)
        else:
            for chunk in text.split('\n\n'):
                if grep in chunk:
                    lines.append(chunk)
    with open(out_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n\n'.join(lines) + '\n')
    print(f'{len(order)} entries, {sum(1 for r in roots if r.tag == 0x11)} compile units -> {out_path}')


if __name__ == '__main__':
    main()
