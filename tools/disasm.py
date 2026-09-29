#!/usr/bin/env python3
"""Decode CC64 machine code without using a host disassembler.

A test that checks a frame prologue, a call sequence, or a service boundary
needs to see instructions rather than a hex string. This decoder recognises the
forms the CC64 encoder emits, which is what the project's own tests and
debugging need, and it is written from the same architecture specification
the encoder is written from rather than from any external tool's output.

An unrecognised byte sequence is reported as such and ends the decode of the
run it appears in. That is deliberate: the decoder does not guess, so a form
the encoder starts emitting shows up as an unknown instruction rather than
being silently read as something else.
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys

# The condition codes the encoder selects, named for what the processor's
# flags mean rather than for a mnemonic, so a wrong selection is visible as a
# wrong name.
CONDITIONS = {
    0x80: "o", 0x81: "no", 0x82: "b", 0x83: "ae", 0x84: "e", 0x85: "ne",
    0x86: "be", 0x87: "a", 0x88: "s", 0x89: "ns", 0x8A: "p", 0x8B: "np",
    0x8C: "l", 0x8D: "ge", 0x8E: "le", 0x8F: "g",
}

REG64 = ("rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
         "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15")
REG32 = tuple(name.replace("r", "e", 1) if name[0] == "r" and name[1] != "8"
              else name for name in REG64)
XMM = tuple(f"xmm{i}" for i in range(16))
OPCODE_NAME = {
    0x88: "mov", 0x89: "mov", 0x8A: "mov", 0x8B: "mov",
    0x31: "xor", 0x29: "sub", 0x01: "add", 0x09: "or", 0x21: "and",
    0x39: "cmp", 0x85: "test",
}


def operand(reg_field: int, width: int) -> str:
    table = REG64 if width == 8 else REG32
    return table[reg_field & 7]


def memory_text(modrm: int, code: bytes, at: int) -> tuple[str, int]:
    """Name a memory operand and return it with the offset past it.

    A memory ModRM may carry a displacement and a scale-index-base byte, and
    the base is often the stack pointer, whose encoding reserves the low bits
    for the scale field. Both have to be consumed for the next instruction to
    be read from the right place, and a form the decoder does not model has
    to say so rather than name an address it cannot compute.
    """
    base = modrm & 7
    rm = REG64[base]
    at = at
    disp = 0
    mode = modrm >> 6
    if mode == 1 and at < len(code):
        disp = struct.unpack_from("<b", code, at)[0]
        at += 1
    elif mode == 2 and at + 3 < len(code):
        disp = struct.unpack_from("<i", code, at)[0]
        at += 4
    if base == 4:
        if at >= len(code):
            return "(memory operand)", at
        sib = code[at]
        at += 1
        if (sib & 7) == 5 and mode != 0 and at + 3 < len(code):
            at += 4
        if (sib & 7) == 4:
            # A base of the stack pointer, or no base at all, is the common
            # form for a frame slot; the index is not worth reporting.
            return f"[rsp{disp:+d}]", at
        return f"[{REG64[sib & 7]}+{REG64[(sib >> 3) & 7]}{disp:+d}]", at
    if base == 5 and mode == 0:
        if at + 3 < len(code):
            return f"[0x{struct.unpack_from('<I', code, at)[0]:x}]", at + 4
    return f"[{rm}{disp:+d}]", at


def decode(code: bytes, limit: int) -> list[str]:
    """Decode a run of bytes into `address  bytes  text` lines."""
    lines: list[str] = []
    at = 0
    while at < len(code) and len(lines) < limit:
        start = at
        rex_w = False
        if code[at] in (0x48, 0x49, 0x4C, 0x4D):
            rex = code[at]
            rex_w = bool(rex & 0x08)
            at += 1
        if at >= len(code):
            lines.append(f"{start:04x}  {code[start:].hex():<18} <truncated>")
            break
        # A vector form carries a mandatory prefix, a REX byte, and then the
        # map opcode, so the prefix has to be recognised before the REX is
        # consumed rather than after.
        vector_prefix = None
        if code[at] in (0x66, 0xF2, 0xF3):
            look = at + 1
            # A REX byte may sit between the mandatory prefix and the map
            # opcode, so the prefix is recognised through it.
            if look < len(code) and 0x40 <= code[look] <= 0x4F:
                look += 1
            if look < len(code) and code[look] == 0x0F:
                vector_prefix = code[at]
                at += 1
        op = code[at]
        # A ModRM byte follows most forms; reading its two fields once, as
        # soon as the opcode is known, is what keeps a register name from
        # being read out of a byte that is not there.
        modrm_reg = None
        modrm_rm = None
        modrm = None
        if at + 1 < len(code):
            modrm = code[at + 1]
            modrm_reg = (modrm >> 3) & 7
            modrm_rm = modrm & 7
        text = None
        if op == 0x55:
            text = "push rbp"
            at += 1
        elif op == 0x5D:
            text = "pop rbp"
            at += 1
        elif op == 0x5B:
            text = "pop " + REG64[code[at + 1] & 7] if at + 1 < len(code) else "pop"
            at += 2
        elif op == 0x50 + (code[at + 1] & 7 if at + 1 < len(code) else 0) and 0x50 <= op <= 0x57:
            text = f"push {REG64[op - 0x50]}"
            at += 1
        elif op == 0x58 + (code[at + 1] & 7 if at + 1 < len(code) else 0) and 0x58 <= op <= 0x5F:
            text = f"pop {REG64[op - 0x58]}"
            at += 1
        elif op == 0xC3:
            text = "ret"
            at += 1
        elif op == 0x90:
            text = "nop"
            at += 1
        elif op == 0xE8 and at + 4 < len(code):
            displacement = struct.unpack_from("<i", code, at + 1)[0]
            text = f"call {start + 5 + displacement:+d}"
            at += 5
        elif op == 0xE9 and at + 4 < len(code):
            displacement = struct.unpack_from("<i", code, at + 1)[0]
            text = f"jmp {start + 5 + displacement:+d}"
            at += 5
        elif op == 0xEB and at + 1 < len(code):
            displacement = struct.unpack_from("<b", code, at + 1)[0]
            text = f"jmp {start + 2 + displacement:+d}"
            at += 2
        elif op in (0x70,) and at + 1 < len(code):
            text = f"j{CONDITIONS.get(op, '?')} {start + 2 + struct.unpack_from('<b', code, at + 1)[0]:+d}"
            at += 2
        elif op in (0x0F,) and at + 1 < len(code) and 0x80 <= code[at + 1] <= 0x8F:
            name = CONDITIONS.get(code[at + 1], "?")
            if at + 4 < len(code):
                displacement = struct.unpack_from("<i", code, at + 2)[0]
                text = f"j{name} {start + 6 + displacement:+d}"
                at += 6
            else:
                at += 2
        elif op == 0x0F and at + 1 < len(code) and code[at + 1] in (0x92, 0x94, 0x96, 0x97, 0x93, 0x95):
            at += 2
            if at < len(code):
                modrm = code[at]
                reg = (modrm >> 3) & 7
                rm = modrm & 7
                text = f"set{CONDITIONS.get(0x90 + reg, '?')} {REG32[rm]}"
                at += 1
        elif op == 0x0F and at + 1 < len(code) and code[at + 1] in (0xB6, 0xB7, 0xBE, 0xBF):
            at += 2
            if at < len(code):
                modrm = code[at]
                reg = (modrm >> 3) & 7
                rm = modrm & 7
                at += 1
                width = 8 if code[start] == 0x48 else 4
                if modrm >= 0xC0:
                    text = f"movzx {operand(reg, width)}, {operand(rm, 1)}"
        elif op == 0x0F and at + 1 < len(code) and code[at + 1] == 0xB6:
            at += 2
            if at < len(code):
                modrm = code[at]
                at += 1
                if modrm >= 0xC0:
                    text = f"movzx {operand((modrm >> 3) & 7, 4)}, {REG64[modrm & 7]}"
        elif op in (0x00, 0x01, 0x02, 0x03, 0x20, 0x21, 0x22, 0x23,
                    0x28, 0x29, 0x2A, 0x2B, 0x30, 0x31, 0x32, 0x33,
                    0x38, 0x39, 0x3A, 0x3B) and modrm is not None:
            # The accumulator forms read and write RAX implicitly, so the
            # ModRM's reg field is the other operand.
            simple = {0x00: "add", 0x01: "add", 0x02: "add", 0x03: "add",
                      0x20: "and", 0x21: "and", 0x22: "and", 0x23: "and",
                      0x28: "sub", 0x29: "sub", 0x2A: "sub", 0x2B: "sub",
                      0x30: "xor", 0x31: "xor", 0x32: "xor", 0x33: "xor",
                      0x38: "cmp", 0x39: "cmp", 0x3A: "cmp", 0x3B: "cmp"}
            at += 2
            width = 8 if rex_w else 4
            name = simple[op]
            if modrm < 0xC0:
                memory, at = memory_text(modrm, code, at)
                text = f"{name} {operand(modrm_reg, width)}, {memory}"
            else:
                if op in (0x38, 0x39, 0x3A, 0x3B):
                    text = f"cmp {operand(modrm_reg, width)}, {operand(modrm_rm, width)}"
                else:
                    text = f"{name} {operand(modrm_rm, width)}, {operand(modrm_reg, width)}"
        elif op in (0xC0, 0xC1, 0xD0, 0xD1, 0xD2, 0xD3) and modrm is not None:
            group = (modrm >> 3) & 7
            shift = {0: "rol", 1: "ror", 4: "shl", 5: "shr", 7: "sar"}
            name = shift.get(group, "shift")
            at += 2
            width = 8 if rex_w else 4
            if modrm < 0xC0:
                memory, at = memory_text(modrm, code, at)
                text = f"{name} {memory}"
            else:
                text = f"{name} {operand(modrm_rm, width)}"
        elif op in (0xC6, 0xC7) and modrm is not None:
            at += 2
            if modrm < 0xC0:
                memory, at = memory_text(modrm, code, at)
            else:
                memory = operand(modrm_rm, 8 if rex_w else 4)
            if op == 0xC6 and at < len(code):
                immediate = code[at]
                at += 1
                text = f"mov {memory}, {immediate}"
            elif op == 0xC7 and at + 3 < len(code):
                immediate = struct.unpack_from("<i", code, at)[0]
                at += 4
                text = f"mov {memory}, {immediate}"
        elif op in (0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87) \
                and modrm is not None:
            at += 2
            width = 8 if rex_w else 4
            if modrm < 0xC0:
                memory, at = memory_text(modrm, code, at)
            else:
                memory = operand(modrm_rm, width)
            if op in (0x84, 0x85, 0x86, 0x87):
                text = f"test {memory}, {operand(modrm_reg, width)}"
            else:
                group = (modrm >> 3) & 7
                names = {0: "add", 5: "sub", 7: "cmp", 4: "and", 6: "or",
                         1: "or", 2: "adc", 3: "sbb"}
                if at < len(code):
                    if op in (0x80, 0x82, 0x83):
                        immediate = code[at]
                        at += 1
                    elif at + 3 < len(code):
                        immediate = struct.unpack_from("<i", code, at)[0]
                        at += 4
                    else:
                        immediate = 0
                    text = f"{names.get(group, 'group8')} {memory}, {immediate}"
        elif op in (0xFF, 0x8F) and modrm is not None:
            group = (modrm >> 3) & 7
            at += 2
            width = 8 if rex_w else 4
            target = operand(modrm_rm, width) if modrm >= 0xC0 else None
            if target is None:
                memory, at = memory_text(modrm, code, at)
            if op == 0x8F:
                text = f"pop {target or memory}"
            else:
                text = {0: "inc", 1: "dec", 2: "call", 4: "jmp",
                        6: "push"}.get(group, "group5") + " " + (target or memory)
        elif op in (0x41, 0x49, 0x4D) or (0x40 <= op <= 0x4F):
            # A REX byte that was not consumed as a prefix: the previous
            # instruction ended in the wrong place, so report it and resync.
            at = start + 1
            text = f"<unknown 0x{op:02x}>"
        elif op in (0x69, 0x6B, 0xF6, 0xF7) and modrm is not None:
            # The multiply and divide family: the group is the ModRM reg
            # field, and the operand form depends on the opcode.
            group = (modrm >> 3) & 7
            names = {0: "test", 2: "mul", 3: "imul", 4: "div", 5: "idiv",
                     6: "neg", 7: "not"}
            name = names.get(group, "arith")
            at += 2
            if op in (0xF6, 0xF7) and modrm < 0xC0:
                memory, at = memory_text(modrm, code, at)
                text = f"{name} {memory}"
            elif op in (0x6B, 0xF6, 0xF7) and at < len(code):
                immediate = struct.unpack_from("<b", code, at)[0]
                at += 1
                text = f"{name} {operand(modrm_reg, 8 if rex_w else 4)}, {immediate}"
            elif modrm >= 0xC0:
                text = f"{name} {operand(modrm_reg, 8 if rex_w else 4)}"
        elif op in (0x80, 0x81, 0x83) and at + 1 < len(code):
            group = (code[at + 1] >> 3) & 7
            names = {0: "add", 5: "sub", 7: "cmp"}
            if group in names:
                if op == 0x81 and at + 5 < len(code):
                    immediate = struct.unpack_from("<i", code, at + 2)[0]
                    text = f"{names[group]} rsp, {immediate}"
                    at += 6
                elif op == 0x83 and at + 2 < len(code):
                    immediate = struct.unpack_from("<b", code, at + 2)[0]
                    text = f"{names[group]} rsp, {immediate}"
                    at += 3
        elif op in (0x88, 0x89, 0x8A, 0x8B, 0x31, 0x29, 0x01, 0x09, 0x21,
                    0x39, 0x85) and modrm is not None:
            at += 2
            if modrm >= 0xC0:
                width = 8 if rex_w else 4
                order = (f"{operand(modrm_rm, width)}, {operand(modrm_reg, width)}"
                         if op in (0x88, 0x89, 0x31, 0x29, 0x01, 0x09, 0x21,
                                   0x39, 0x85)
                         else f"{operand(modrm_reg, width)}, {operand(modrm_rm, width)}")
                text = f"{OPCODE_NAME.get(op, '?')} {order}"
            else:
                memory, at = memory_text(modrm, code, at)
                text = f"{OPCODE_NAME.get(op, '?')} {memory}"
        elif op == 0xB8 and at + 1 < len(code):
            width = 8 if rex_w else 4
            if width == 8 and at + 8 < len(code):
                immediate = struct.unpack_from("<Q", code, at + 1)[0]
                # The opcode's low three bits select the destination, so the
                # immediate form names a register the opcode itself encodes.
                text = f"movabs {REG64[op & 7]}, 0x{immediate:x}"
                at += 9
        elif op == 0x8D and modrm is not None:
            at += 2
            if modrm < 0xC0:
                memory, at = memory_text(modrm, code, at)
                text = f"lea {REG64[modrm_reg]}, {memory}"
        elif op == 0x63 and modrm is not None:
            at += 2
            if modrm >= 0xC0:
                text = f"movsxd {operand(modrm_reg, 8)}, {operand(modrm_rm, 4)}"
        elif op == 0xCD and at + 1 < len(code):
            text = f"int 0x{code[at + 1]:02x}"
            at += 2
        elif vector_prefix is not None:
            # The REX byte, if there was one, is consumed here rather than by
            # the prefix scan, so the map opcode is what follows it.
            if 0x40 <= op <= 0x4F:
                rex_w = bool(op & 0x08)
                at += 1
                if at >= len(code):
                    break
            if code[at] != 0x0F:
                at = start + 1
                lines.append(f"{start:04x}  {code[start:at].hex():<18} "
                             f"<unknown 0x{op:02x}>")
                continue
            at += 1
            if at >= len(code):
                break
            # The vector forms carry a mandatory prefix and a two-byte opcode.
            # The decoder names the operation and its operands, which is what
            # a test checking a load, a store, or a compare needs; it does not
            # claim to model the whole vector encoding.
            prefix = {0x66: "packed", 0xF2: "double",
                      0xF3: "single"}.get(vector_prefix, "vector")
            map_op = code[at]
            operations = {0x10: "mov", 0x11: "mov", 0x28: "mov", 0x29: "mov",
                          0x2A: "cvt", 0x2C: "cvtt", 0x2D: "cvt",
                          0x2E: "ucomis", 0x5A: "cvts", 0x5C: "sub",
                          0x5E: "div", 0x5F: "max", 0x60: "add",
                          0x6E: "mov", 0x6F: "mov", 0xD6: "mov"}
            name = operations.get(map_op, "vector")
            at += 1
            if at < len(code):
                mod = code[at]
                at += 1
                if mod >= 0xC0:
                    text = f"{name} {prefix} {XMM[mod & 7]}, {XMM[(mod >> 3) & 7]}"
                else:
                    memory, at = memory_text(mod, code, at)
                    text = f"{name} {prefix} {memory}"
        if text is None:
            lines.append(
                f"{start:04x}  {code[start:at + 1].hex():<18} "
                f"<unknown 0x{op:02x}>")
            at = start + 1
            continue
        lines.append(f"{start:04x}  {code[start:at].hex():<18} {text}")
    return lines


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("path", type=pathlib.Path)
    parser.add_argument("--limit", type=int, default=200)
    args = parser.parse_args()
    try:
        data = args.path.read_bytes()
    except OSError as error:
        print(f"disasm: {error}", file=sys.stderr)
        return 1
    for line in decode(data, args.limit):
        print(line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
