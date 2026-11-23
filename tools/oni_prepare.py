"""Prepare Onimusha: Warlords' resident and overlay code images for BozoEngine's ELF-based recompiler.

SLUS_200.18 is a CodeWarrior link that kept its symbol table and its DWARF: every function of the main program and of
the code overlays is named, with its size. The overlays' sections (em000..em181, sb000/sb001, room000..room530) are
empty in the ELF: their code is in MWo3 modules inside the disc's LMR (enemy) and RDT (room) files, already linked at
their slot address:

  enemy slot 0  0x4ba780  emXX0.bin  (EMMR_*.LMR, LMR_*.LMR first module)
  enemy slot 1  0x4c9500  emXX1.bin  (LMR_*.LMR second module)
  room          0x4d8280  roomNNN.bin (ROOMNNN.RDT)
  (em090.bin is linked at 0x2bf200 on the disc, not at its ELF section's address)

Each distinct module (by content) becomes its own ELF (tools/wrap_elf.py) with the matching ELF section's FUNC
symbols, and its 64-byte header (load, sizes, file name) becomes the identity the runtime selects it by.
No instruction or relocation is executed. Everything written is local game-derived data (never committed).

  python -I tools/oni_prepare.py <game dir> <out dir>

Writes <out>/oni.elf, <out>/<module>.elf, <out>/<module>.identity, <out>/oni_modules.txt
(one line per module: name load end) and <out>/oni_modules.cmake (the list CMake generates code for).
"""
from pathlib import Path
import hashlib
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from wrap_elf import wrap  # noqa: E402

ELF_SHA1 = '21166f0c7ec1ee2e6dfb87e81af242efaf521dc7'  # SLUS_200.18, NTSC-U 1.00
MAIN_SECTION = 4
# The main program's EE code: crt0 at 0x100000 up to the VU microcode (.vutext at 0x1e9e00). Data follows.
MAIN_CODE = (0x100000, 0x1e9e00)
IDENTITY_BYTES = 64

SH = struct.Struct('<10I')


def headers(blob):
    off = struct.unpack_from('<I', blob, 0x20)[0]
    count = struct.unpack_from('<H', blob, 0x30)[0]
    return [list(SH.unpack_from(blob, off + i * 40)) for i in range(count)]


def section_names(blob, sections):
    si = struct.unpack_from('<H', blob, 0x32)[0]
    base = sections[si][4]
    return [blob[base + s[0]:blob.index(b'\0', base + s[0])].decode('latin-1') for s in sections]


def finish(blob, sections, names):
    out = bytearray(blob)
    si = struct.unpack_from('<H', blob, 0x32)[0]
    sections[si][4] = len(out)
    sections[si][5] = len(names)
    out += names
    out += bytes((-len(out)) % 4)
    struct.pack_into('<I', out, 0x20, len(out))
    struct.pack_into('<H', out, 0x30, len(sections))
    out += b''.join(SH.pack(*s) for s in sections)
    return out


def elf_symbols(elf, sections):
    symtab = next(s for s in sections if s[1] == 2)
    strtab = sections[symtab[6]]
    for off in range(symtab[4], symtab[4] + symtab[5], 16):
        name, value, size, info, _other, shndx = struct.unpack_from('<IIIBBH', elf, off)
        end = elf.index(b'\0', strtab[4] + name)
        yield elf[strtab[4] + name:end].decode('latin-1'), value, size, info & 15, shndx


def find_modules(game):
    """Every MWo3 module in the LMR and RDT files: {sha1: (name, raw bytes, [source files])}."""
    found = {}
    for path in sorted(list((game / 'IMAGE/LMR').glob('*.LMR')) + list((game / 'IMAGE/RDT').glob('*.RDT'))):
        blob = path.read_bytes()
        at = blob.find(b'MWo3')
        while at != -1:
            load, text, data, _bss = struct.unpack_from('<4I', blob, at + 8)
            name = blob[at + 0x20:at + 0x40].split(b'\0')[0].decode('latin-1')
            raw = blob[at:at + text + data]
            digest = hashlib.sha1(raw).hexdigest()
            entry = found.setdefault(digest, [name, raw, []])
            entry[2].append(path.name)
            at = blob.find(b'MWo3', at + 4)
    return found


def main(game, out):
    game = Path(game)
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    elf = (game / 'SLUS_200.18').read_bytes()
    sections = headers(elf)
    names = section_names(elf, sections)
    if hashlib.sha1(elf).hexdigest() != ELF_SHA1:
        raise ValueError('SLUS_200.18: not the NTSC-U Onimusha: Warlords executable this port is built against')
    flags = struct.unpack_from('<I', elf, 0x24)[0]

    # The resident program: the 'main' section covers code, VU microcode and data (and is WAX); describe only its EE
    # code as executable. The load image (program headers) is unchanged.
    resident = [list(s) for s in sections]
    resident[MAIN_SECTION][5] = MAIN_CODE[1] - MAIN_CODE[0]
    si = struct.unpack_from('<H', elf, 0x32)[0]
    shstr = bytearray(elf[sections[si][4]:sections[si][4] + sections[si][5]])
    image = finish(elf, resident, shstr)
    # The developers' 'test' section (0x100 bytes at 0x3800000) lies past the 32 MB of RAM: its program header is
    # dropped (PT_NULL); nothing in the game addresses it.
    phoff = struct.unpack_from('<I', image, 0x1c)[0]
    phnum = struct.unpack_from('<H', image, 0x2c)[0]
    for i in range(phnum):
        kind, _off, vaddr, _paddr, _filesz, memsz = struct.unpack_from('<6I', image, phoff + i * 32)
        if kind == 1 and vaddr + memsz > 0x2000000:
            struct.pack_into('<I', image, phoff + i * 32, 0)
    (out / 'oni.elf').write_bytes(image)

    # Overlay FUNC symbols by ELF section name (em000, room002, ...).
    by_section = {}
    for name, value, size, kind, shndx in elf_symbols(elf, sections):
        if kind == 2 and size and 0 < shndx < len(names) and shndx != MAIN_SECTION:
            # Relative to the section's link address: em090.bin is linked at 0x2bf200 on the disc, its ELF section
            # at the enemy slot.
            value -= sections[shndx][3]
            by_section.setdefault(names[shndx], {})
            if value not in by_section[names[shndx]] or size > by_section[names[shndx]][value][1]:
                by_section[names[shndx]][value] = (name, size)

    modules = find_modules(game)
    used = {}
    identities = {}
    listing = []
    for digest, (modname, raw, sources) in sorted(modules.items(), key=lambda kv: (kv[1][0], kv[0])):
        stem = modname.split('.')[0]
        image_name = stem if stem not in used else f'{stem}_{len(used[stem])}'
        used.setdefault(stem, []).append(digest)
        identity = raw[:IDENTITY_BYTES]
        if identity in identities:
            raise ValueError(f'{image_name}: same {IDENTITY_BYTES}-byte header as {identities[identity]} with different code')
        identities[identity] = image_name
        load, text, data, bss = struct.unpack_from('<4I', raw, 8)

        image = bytearray(wrap(raw, flags))
        sh = headers(image)
        isi = struct.unpack_from('<H', image, 0x32)[0]
        inames = bytearray(image[sh[isi][4]:sh[isi][4] + sh[isi][5]])
        strings = bytearray(b'\0')
        symbols = bytearray(16)
        symbols_from = by_section.get(stem, {})
        word = lambda a: struct.unpack_from('<I', raw, a - load)[0] if 0 <= a - load <= len(raw) - 4 else None
        matched = 0
        kept = 0
        for offset, (fname, size) in sorted(symbols_from.items()):
            addr = load + offset
            index = next((i for i, s in enumerate(sh) if s[2] & 4 and s[3] <= addr < s[3] + s[5]), None)
            if index is None:
                continue
            # The ELF is the developers' link: keep a symbol only where the disc's module agrees (its last
            # instructions are the return and its delay slot, or a jump).
            last = word(addr + size - 8)
            if last is not None and (last == 0x03E00008 or last >> 26 in (2, 3) or last & 0xFC00003F == 0x00000008):
                matched += 1
            else:
                continue
            kept += 1
            symname = len(strings)
            strings += fname.encode('latin-1') + b'\0'
            symbols += struct.pack('<IIIBBH', symname, addr, size, 0x12, 0, index)
        strname = len(inames)
        inames += b'.strtab\0'
        strindex = len(sh)
        sh.append([strname, 3, 0, 0, len(image), len(strings), 0, 0, 1, 0])
        image += strings
        image += bytes((-len(image)) % 4)
        symname = len(inames)
        inames += b'.symtab\0'
        sh.append([symname, 2, 0, 0, len(image), len(symbols), strindex, 1, 4, 16])
        image += symbols
        (out / f'{image_name}.elf').write_bytes(finish(image, sh, inames))
        (out / f'{image_name}.identity').write_bytes(identity)
        end = load + len(raw) + bss
        listing.append((image_name, load, end))
        print(f'{image_name:10s} load {load:#x} end {end:#x} symbols {kept}/{len(symbols_from)} '
              f'from {", ".join(sorted(set(sources))[:3])}{" ..." if len(set(sources)) > 3 else ""}')

    (out / 'oni_modules.txt').write_text(''.join(f'{n} {l:#x} {e:#x}\n' for n, l, e in listing))
    (out / 'oni_modules.cmake').write_text('set(oni_modules ' + ' '.join(n for n, _, _ in listing) + ')\n')
    print(f'oni.elf + {len(listing)} overlay modules -> {out}')


if __name__ == '__main__':
    main(*sys.argv[1:])
