"""oni_vu1_gen.py <oni.elf> <out.cpp>: Onimusha's VU1 microprograms (the models' Vu1mc_* images) as native C++.

The game draws its models with VU1: each part's VIF1 chain (Md_tag_trans) sends one of the images below with a VIF MPG
at micro address 0 (Md_obj_vu1_type_set puts a REF to it in the part's chain), starts it at 0 (MSCAL, the program's
set-up: constants and matrices from VU memory) and then runs it on each batch of vertices it unpacks (MSCNT: the main
loop after the set-up's E bit), the program building GIF packets in VU memory and sending them with XGKICK.

Natively each image becomes a function oni_vu1_<name>(ctx, io, pc) run on a VU1 state (an R5900Context holding VU1's
registers: BozoEngine's VU operations take one) from pc to the pair after the next E bit; it returns where an MSCNT
continues. XGKICK calls io.kick with the packet's address (runtime/oni_vu1.cpp turns the packet into draws).

The pair model is tools/oni_vu0_gen.py's (from TTA's VU0 translator): in a pair the lower instruction reads its
operands before the upper writes, Q lands 7 cycles after DIV (13 after RSQRT), FMAND sees the MAC flags 4 pairs back, a
branch's next pair is its delay slot. VU1's lower instructions are added here: LQ / LQI / SQ / ILW / ILWR / ISW on its
16 KB data memory, XTOP, XGKICK, FCEQ. Anything else is refused, so a different image cannot be translated silently.
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oni_vu0_gen as vu0  # noqa: E402

# (ELF address of the image: a VIF NOP, an MPG of the program to micro address 0, then MSCAL 0), name.
IMAGES = [
    (0x1E9E00, "std_cl"), (0x1EA2D0, "std_nc"), (0x1EA770, "100_cl"), (0x1EABC0, "100_nc"),
    (0x1EAFD0, "1mat_cl"), (0x1EB5B0, "1mat_nc"), (0x1EBAF0, "bc_cl"), (0x1EC030, "wld_cl"),
    (0x1EC400, "wld_nc"), (0x1EC7B0, "spe"), (0x1ECD10, "texmap"), (0x1ED2A0, "100_clz"),
    (0x1ED700, "100_ncz"), (0x1EDB20, "wld_bcl"),
    (0x20E878, "shadow"),  # the shadows' program (SHADOW_VIF1DMA_TAG's chain): two MPGs, 256 + 201 pairs
]


def elf_reader(path):
    d = open(path, "rb").read()
    phoff, phnum = struct.unpack_from("<I", d, 0x1C)[0], struct.unpack_from("<H", d, 0x2C)[0]
    segs = []
    for i in range(phnum):
        t, off, va, _, fs, _ = struct.unpack_from("<6I", d, phoff + 32 * i)
        if t == 1:
            segs.append((va, off, fs))

    def at(address):
        for va, off, fs in segs:
            if va <= address < va + fs:
                return off + address - va
        raise ValueError(f"0x{address:x} is not in the ELF")
    return d, at


def load_image(d, offset):
    """The image's pairs by index (its MPGs, consecutive from micro address 0) and its pair count."""
    p, mem = offset, {}
    while True:
        code = struct.unpack_from("<I", d, p)[0]
        cmd, num, imm = (code >> 24) & 0x7F, (code >> 16) & 0xFF, code & 0xFFFF
        if cmd == 0:
            p += 4
            continue
        if cmd != 0x4A:
            break
        if imm != len(mem):
            raise ValueError(f"image at file 0x{offset:x}: MPG to 0x{imm * 8:x} after {len(mem)} pairs")
        n = num or 256
        p += 4
        for k in range(n):
            mem[imm + k] = struct.unpack_from("<II", d, p + 8 * k)
        p += 8 * n
    if not mem:
        raise ValueError(f"image at file 0x{offset:x}: no MPG")
    return mem, len(mem)


def program_hash(mem, n):
    """FNV-1a 64 over the program's words (lower, upper per pair): runtime/oni_vu1.cpp knows an MPG's program by it."""
    h = 0xCBF29CE484222325
    for k in range(n):
        for word in mem[k]:
            for b in struct.pack("<I", word):
                h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def lower_vu1(p):
    """VU1's lower instructions besides the VU0 translator's; False when it is not one of them."""
    w = p.lo
    if p.up >> 31 & 1:
        return False
    op = (w >> 25) & 0x7F
    it, is_, imm11 = (w >> 16) & 31, (w >> 11) & 31, w & 0x7FF
    simm11 = imm11 - 0x800 if imm11 & 0x400 else imm11
    dest = vu0.dest_of(w)
    if op == 0x00:  # LQ.dest ft, imm(is)
        if it:
            p.lbody.append(f"Vec l = vf[{it}]; vu1Lq(io, l, vi[{is_}] + {simm11}, {dest});")
            p.lwrites.add(it)
            p.commit.insert(0, f"vf[{it}] = l;")
        return True
    if op == 0x01:  # SQ.dest fs, imm(it)
        p.lbody.append(f"vu1Sq(io, vf[{is_}], vi[{it}] + {simm11}, {dest});")
        return True
    if op == 0x04:  # ILW.dest it, imm(is): the field's low 16 bits
        if it:
            p.lbody.append(f"const uint32_t iNext = vu1Ilw(io, vi[{is_}] + {simm11}, {dest});")
            p.commit.insert(0, f"vi[{it}] = iNext;")
        return True
    if op == 0x05:  # ISW.dest it, imm(is)
        p.lbody.append(f"vu1Isw(io, vi[{it}], vi[{is_}] + {simm11}, {dest});")
        return True
    if op == 0x10:  # FCEQ: vi1 = 1 when the clip flags equal the 24-bit value
        p.lbody.append(f"const uint32_t iNext = ((ctx->vu0_clip_flags & 0xFFFFFFu) == 0x{w & 0xFFFFFF:06x}u) ? 1u : 0u;")
        p.commit.insert(0, "vi[1] = iNext;")
        return True
    if op == 0x13:  # FCOR: vi1 = 1 when the clip flags or'ed with the value are all set
        p.lbody.append(f"const uint32_t iNext = (((ctx->vu0_clip_flags | 0x{w & 0xFFFFFF:06x}u) & 0xFFFFFFu) == 0xFFFFFFu) ? 1u : 0u;")
        p.commit.insert(0, "vi[1] = iNext;")
        return True
    if op == 0x11:  # FCSET: the clip flags set to the value
        p.commit.append(f"ctx->vu0_clip_flags = 0x{w & 0xFFFFFF:06x}u;")
        return True
    if op == 0x1C:  # FCGET it: the last two clip judgements (12 bits)
        if it:
            p.lbody.append("const uint32_t iNext = ctx->vu0_clip_flags & 0xFFFu;")
            p.commit.insert(0, f"vi[{it}] = iNext;")
        return True
    if op in (0x2C, 0x2D, 0x2E, 0x2F):  # IBLTZ, IBGTZ, IBLEZ, IBGEZ: vi[is] as a signed 16-bit value
        rel = {0x2C: "<", 0x2D: ">", 0x2E: "<=", 0x2F: ">="}[op]
        p.branch = (f"static_cast<int16_t>(vi[{is_}]) {rel} 0", p.pc + 8 + simm11 * 8)
        return True
    if op in (0x24, 0x25):  # JR is / JALR it, is: to vi[is] (in pairs); JALR's return address past the delay slot
        if op == 0x25 and it:
            p.lbody.append(f"const uint32_t iNext = 0x{(p.pc + 16) // 8:x}u;")
            p.commit.insert(0, f"vi[{it}] = iNext;")
        p.lbody.append(f"const uint32_t jumpTo = (vi[{is_}] * 8u) & 0x3FFFu;")
        p.branch = ("true", "jumpTo")
        p.returns = op == 0x25
        return True
    if op == 0x21:  # BAL it, target: the return address (in pairs) past the delay slot
        if it:
            p.lbody.append(f"const uint32_t iNext = 0x{(p.pc + 16) // 8:x}u;")
            p.commit.insert(0, f"vi[{it}] = iNext;")
        p.branch = ("true", p.pc + 8 + simm11 * 8)
        p.returns = True
        return True
    if op != 0x40 or (w & 0x3F) < 0x3C:
        return False
    sp = ((w >> 4) & 0x7C) | (w & 3)
    if sp == 0x34:  # LQI.dest ft, (is++)
        if it:
            p.lbody.append(f"Vec l = vf[{it}]; vu1Lq(io, l, vi[{is_}], {dest});")
            p.lwrites.add(it)
            p.commit.insert(0, f"vf[{it}] = l;")
        if is_:
            p.lbody.append(f"const uint32_t incNext = (vi[{is_}] + 1u) & 0xFFFFu;")
            p.commit.insert(0, f"vi[{is_}] = incNext;")
        return True
    if sp == 0x35:  # SQI.dest fs, (it++)
        p.lbody.append(f"vu1Sq(io, vf[{is_}], vi[{it}], {dest});")
        if it:
            p.lbody.append(f"const uint32_t incNext = (vi[{it}] + 1u) & 0xFFFFu;")
            p.commit.insert(0, f"vi[{it}] = incNext;")
        return True
    if sp == 0x36:  # LQD.dest ft, (--is)
        p.lbody.append(f"const uint32_t decNext = (vi[{is_}] - 1u) & 0xFFFFu;")
        if it:
            p.lbody.append(f"Vec l = vf[{it}]; vu1Lq(io, l, decNext, {dest});")
            p.lwrites.add(it)
            p.commit.insert(0, f"vf[{it}] = l;")
        if is_:
            p.commit.insert(0, f"vi[{is_}] = decNext;")
        return True
    if sp == 0x37:  # SQD.dest fs, (--it)
        p.lbody.append(f"const uint32_t decNext = (vi[{it}] - 1u) & 0xFFFFu;")
        p.lbody.append(f"vu1Sq(io, vf[{is_}], decNext, {dest});")
        if it:
            p.commit.insert(0, f"vi[{it}] = decNext;")
        return True
    if sp == 0x3E:  # ILWR.dest it, (is)
        if it:
            p.lbody.append(f"const uint32_t iNext = vu1Ilw(io, vi[{is_}], {dest});")
            p.commit.insert(0, f"vi[{it}] = iNext;")
        return True
    if sp == 0x3F:  # ISWR.dest it, (is)
        p.lbody.append(f"vu1Isw(io, vi[{it}], vi[{is_}], {dest});")
        return True
    if sp == 0x68:  # XTOP it
        if it:
            p.lbody.append("const uint32_t iNext = io.top & 0x3FFu;")
            p.commit.insert(0, f"vi[{it}] = iNext;")
        return True
    if sp == 0x69:  # XITOP it
        if it:
            p.lbody.append("const uint32_t iNext = io.itop & 0x3FFu;")
            p.commit.insert(0, f"vi[{it}] = iNext;")
        return True
    if sp == 0x6C:  # XGKICK is: the GIF packet at that address (sent now: the program has written it)
        p.lbody.append(f"io.kick(io, vi[{is_}] & 0x3FFu);")
        return True
    return False


def translate(mem, entries):
    """Every pair reachable from the entries (byte addresses)."""
    pairs, todo = {}, list(entries)
    finals = {idx * 8 + 8 for idx, (lo, up) in mem.items() if up >> 30 & 1}
    while todo:
        pc = todo.pop()
        stop_at = None  # an unconditional branch's delay slot: the last pair of this run
        while pc not in pairs:
            idx = pc // 8
            if idx not in mem:
                raise ValueError(f"pc 0x{pc:x} outside the image")
            lo, up = mem[idx]
            p = vu0.Pair(pc, lo, up)
            vu0.upper_insn(p)
            if not lower_vu1(p):
                vu0.lower_insn(p)
            if p.uwrites & p.lwrites:
                raise ValueError(f"pair 0x{pc:x}: upper and lower write the same register")
            pairs[pc] = p
            if p.branch:
                if isinstance(p.branch[1], int):
                    todo.append(p.branch[1])
                if getattr(p, "returns", False):
                    todo.append(pc + 16)  # where the subroutine returns (JR)
                if p.branch[0] == "true":
                    stop_at = pc + 8
                else:
                    todo.append(pc + 16)
            if p.end:
                todo.append(pc + 8)
                break
            if pc in finals or pc == stop_at:
                break
            pc += 8
    return pairs


def emit_program(name, mem, lines):
    """One program as straight-line C++: a label per pair, jumps only at branches (after their delay slot), Q's latency
    checked where Q is read. Programs that never read the MAC flags use the flag-free arithmetic (oni_vu1.h, fast_*)."""
    entries = [0] + sorted(idx * 8 + 16 for idx, (lo, up) in mem.items() if up >> 30 & 1 and idx * 8 + 16 < len(mem) * 8)
    pairs = translate(mem, entries)
    uses_mac = any(p.reads_mac for p in pairs.values())
    dynamic = any(p.branch and isinstance(p.branch[1], str) for p in pairs.values())
    finals = {pc + 8 for pc, p in pairs.items() if p.end}
    fast = re.compile(r"\b(add|sub|mul|madd|msub)\(ctx, ")
    lines += [f"    uint32_t oni_vu1_{name}(R5900Context *ctx, Vu1Io &io, uint32_t pc)", "    {",
              "        Vec *vf = ctx->vu0_vf;", "        Vec &acc = ctx->vu0_acc;", "        uint32_t *vi = ctx->vi;",
              "        uint32_t q = ctx->vu0_q_bits, qPending = 0, qReadyAt = 0;", "        bool qWaiting = false;",
              "        uint32_t cycle = 0, branchTo = 0, next = 0;", "        bool branchSet = false;"]
    if uses_mac:
        lines += ["        uint32_t macPipe[4] = {ctx->vu0_mac, ctx->vu0_mac, ctx->vu0_mac, ctx->vu0_mac};"]
    lines += ["        switch (pc)", "        {"]
    for e in entries:
        if e in pairs:
            lines.append(f"        case 0x{e:x}u: goto L_{e:x};")
    lines += ["        default: io.fault = pc; return pc;", "        }"]
    if dynamic:
        lines += ["    dispatch:", "        switch (branchTo)", "        {"]
        for pc in sorted(pairs):
            lines.append(f"        case 0x{pc:x}u: goto L_{pc:x};")
        lines += ["        default: io.fault = branchTo; next = branchTo; goto finish;", "        }"]
    commit_in_upper = lambda c: c.startswith("acc") or "= u;" in c or c.startswith("ctx->vu0_clip_flags")
    order = sorted(pairs)
    for k, pc in enumerate(order):
        p = pairs[pc]
        lines.append(f"    L_{pc:x}:")
        lines.append("    {")
        if p.waitq:
            lines.append("        if (qWaiting) { q = qPending; qWaiting = false; }")
        elif p.reads_q or p.q_issue:
            lines.append("        if (qWaiting && cycle >= qReadyAt) { q = qPending; qWaiting = false; }")
        if p.reads_mac:
            lines.append("        const uint32_t macSeen = macPipe[cycle & 3] & 0xFFFFu;")
        for b in p.lbody:
            lines.append("        " + b)
        if p.ubody:
            lines.append("        {")
            for b in p.ubody:
                lines.append("            " + (b if uses_mac else fast.sub(lambda m: f"fast_{m.group(1)}(", b)))
            for c in p.commit:
                if commit_in_upper(c):
                    lines.append("            " + c)
            lines.append("        }")
        for c in p.commit:
            if not commit_in_upper(c):
                lines.append("        " + c)
        if p.q_issue:
            lines.append(f"        qPending = qNext; qWaiting = true; qReadyAt = cycle + {p.q_issue};")
        if uses_mac:
            lines.append("        macPipe[cycle & 3] = ctx->vu0_mac;")
        lines.append("        ++cycle;")
        # This pair is the delay slot of the one before: its branch goes now.
        prev = pairs.get(pc - 8)
        if prev is not None and prev.branch:
            target = prev.branch[1]
            go = "goto dispatch;" if isinstance(target, str) else f"goto L_{target:x};"
            lines.append(f"        if (branchSet) {{ branchSet = false; {go} }}")
        if p.branch:
            cond, target = p.branch
            to = target if isinstance(target, str) else f"0x{target:x}u"
            lines.append(f"        branchSet = {cond}; branchTo = {to};" if cond != "true" else f"        branchSet = true; branchTo = {to};")
        if pc in finals:
            lines.append(f"        next = 0x{pc + 8:x}u; goto finish;")
        elif k + 1 >= len(order) or order[k + 1] != pc + 8:
            if pc + 8 in pairs:
                lines.append(f"        goto L_{pc + 8:x};")
            else:
                lines.append(f"        io.fault = 0x{pc + 8:x}u; next = 0x{pc + 8:x}u; goto finish;")
        lines.append("    }")
    lines += ["    finish:",
              "        if (qWaiting) q = qPending;",
              "        ctx->vu0_q_bits = q;",
              "        io.cycles += cycle;",
              "        return next & 0x3FFFu;",
              "    }", ""]


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    d, at = elf_reader(sys.argv[1])
    lines = [
        "// Generated by tools/oni_vu1_gen.py from SLUS_200.18 (the player's disc): do not edit, never commit.",
        '#include "oni_vu1.h"',
        '#include "bozo/ops_vu0.h"',
        "",
        "namespace oni::vu1",
        "{",
        "    using namespace bozo::vu0;",
        "",
    ]
    table = []
    for address, name in IMAGES:
        mem, n = load_image(d, at(address))
        emit_program(name, mem, lines)
        table.append(f"        {{0x{address:x}u, {n}u, 0x{program_hash(mem, n):016x}ull, \"{name}\", oni_vu1_{name}}},")
    lines += ["    const Program kPrograms[] = {"] + table + ["    };",
              "    const size_t kProgramCount = sizeof(kPrograms) / sizeof(kPrograms[0]);", "}", ""]
    open(sys.argv[2], "w", newline="\n").write("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
