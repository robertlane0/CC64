#!/usr/bin/env python3
"""Validate and display a project-owned CC64O object."""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys

MAGIC = b"CC64OBJ\0"
HEADER_SIZE = 96
SECTION_SIZE = 40
SYMBOL_SIZE = 32
RELOC_SIZE = 32


def crc32(data: bytes) -> int:
    value = 0xFFFFFFFF
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0xEDB88320 if value & 1 else 0)
    return value ^ 0xFFFFFFFF


def fail(message: str) -> None:
    raise ValueError(message)


def read_object(path: pathlib.Path) -> tuple[dict, list[dict], list[dict], bytes]:
    data = path.read_bytes()
    if len(data) < HEADER_SIZE or data[:8] != MAGIC:
        fail("bad magic or truncated header")
    fields = struct.unpack_from("<8sHHHHIIIIIIII", data, 0)
    if fields[1] != 1 or fields[2] != 0x3433 or fields[3] != 1 or fields[4] != 62:
        fail("unsupported object version or target")
    if fields[5] != HEADER_SIZE:
        fail("unexpected header size")
    if crc32(data[:64]) != struct.unpack_from("<I", data, 64)[0]:
        fail("header CRC mismatch")
    if crc32(data[:68]) != struct.unpack_from("<I", data, 68)[0]:
        fail("file CRC mismatch")
    section_offset = struct.unpack_from("<I", data, 28)[0]
    section_count = struct.unpack_from("<I", data, 32)[0]
    symbol_offset = struct.unpack_from("<I", data, 36)[0]
    symbol_count = struct.unpack_from("<I", data, 40)[0]
    string_offset = struct.unpack_from("<I", data, 44)[0]
    string_size = struct.unpack_from("<I", data, 48)[0]
    if section_offset + section_count * SECTION_SIZE > len(data):
        fail("section table is truncated")
    if symbol_offset + symbol_count * SYMBOL_SIZE > len(data):
        fail("symbol table is truncated")
    if string_offset + string_size > len(data):
        fail("string table is truncated")
    strings = data[string_offset:string_offset + string_size]
    sections: list[dict] = []
    for index in range(section_count):
        record = data[section_offset + index * SECTION_SIZE:
                       section_offset + (index + 1) * SECTION_SIZE]
        name_offset, payload_offset, payload_size, reloc_offset, reloc_count, align = struct.unpack_from("<IIIIII", record, 0)
        kind = record[24]
        if name_offset >= len(strings) or align > 15 or kind > 3:
            fail("invalid section record")
        if kind != 3 and payload_offset + payload_size > len(data):
            fail("section payload is truncated")
        if reloc_count and reloc_offset + reloc_count * RELOC_SIZE > len(data):
            fail("relocation table is truncated")
        name_end = strings.find(b"\0", name_offset)
        if name_end < 0:
            fail("invalid section name")
        sections.append({
            "name": strings[name_offset:name_end].decode("ascii"),
            "payload_offset": payload_offset,
            "size": payload_size,
            "reloc_offset": reloc_offset,
            "reloc_count": reloc_count,
            "align": align,
            "kind": kind,
        })
    symbols: list[dict] = []
    for index in range(symbol_count):
        record = data[symbol_offset + index * SYMBOL_SIZE:
                       symbol_offset + (index + 1) * SYMBOL_SIZE]
        name_offset, value, section_index = struct.unpack_from("<III", record, 0)
        binding, kind = record[12], record[13]
        size = struct.unpack_from("<Q", record, 16)[0]
        if name_offset >= len(strings):
            fail("invalid symbol name")
        name_end = strings.find(b"\0", name_offset)
        if name_end < 0:
            fail("unterminated symbol name")
        if section_index != 0xFFFFFFFF and section_index >= section_count:
            fail("symbol section is out of range")
        symbols.append({
            "name": strings[name_offset:name_end].decode("ascii"),
            "value": value,
            "section": section_index,
            "binding": binding,
            "kind": kind,
            "size": size,
        })
    relocations: list[dict] = []
    for section_index, section in enumerate(sections):
        for index in range(section["reloc_count"]):
            offset = section["reloc_offset"] + index * RELOC_SIZE
            entry = data[offset:offset + RELOC_SIZE]
            where, symbol_index, kind = struct.unpack_from("<QII", entry, 0)
            addend = struct.unpack_from("<q", entry, 16)[0]
            width = struct.unpack_from("<I", entry, 24)[0]
            if where + width > section["size"] or symbol_index >= symbol_count:
                fail("relocation range or symbol is invalid")
            if width not in (4, 8) or kind not in (1, 2, 3, 4, 5):
                fail("unsupported relocation")
            relocations.append({"section": section_index, "offset": where,
                                "symbol": symbol_index, "type": kind,
                                "addend": addend, "width": width})
    header = {
        "size": len(data), "sections": section_count, "symbols": symbol_count,
        "section_offset": section_offset, "symbol_offset": symbol_offset,
    }
    return header, sections, symbols, data


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("path", type=pathlib.Path)
    args = parser.parse_args()
    try:
        header, sections, symbols, data = read_object(args.path)
    except (OSError, ValueError, struct.error) as error:
        print(f"inspect: {error}", file=sys.stderr)
        return 1
    print(f"CC64O bytes={header['size']} sections={header['sections']} symbols={header['symbols']}")
    for index, section in enumerate(sections):
        print(f"section {index} {section['name']} kind={section['kind']} size={section['size']} align={1 << section['align']}")
        if section["kind"] != 3 and section["size"] != 0:
            start = section["payload_offset"]
            print(f"  bytes {data[start:start + section['size']].hex()}")
    for index, symbol in enumerate(symbols):
        print(f"symbol {index} {symbol['name']} section={symbol['section']} value={symbol['value']} size={symbol['size']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
