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
    if fields[5] != HEADER_SIZE or struct.unpack_from("<I", data, 60)[0] != 0:
        fail("unexpected header flags or size")
    if any(data[72:HEADER_SIZE]):
        fail("nonzero reserved header bytes")
    if crc32(data[:64]) != struct.unpack_from("<I", data, 64)[0]:
        fail("header CRC mismatch")
    crc_image = bytearray(data)
    crc_image[68:72] = b"\0\0\0\0"
    if crc32(crc_image) != struct.unpack_from("<I", data, 68)[0]:
        fail("file CRC mismatch")
    if len(data) > 512 * 1024 * 1024:
        fail("object file exceeds 512 MiB")
    target_offset, target_size = struct.unpack_from("<II", data, 20)
    if (target_size != len(b"x86_64-pc-dos64\0") or
            target_offset + target_size > len(data) or
            data[target_offset:target_offset + target_size] !=
            b"x86_64-pc-dos64\0"):
        fail("invalid target triple")
    section_offset = struct.unpack_from("<I", data, 28)[0]
    section_count = struct.unpack_from("<I", data, 32)[0]
    symbol_offset = struct.unpack_from("<I", data, 36)[0]
    symbol_count = struct.unpack_from("<I", data, 40)[0]
    string_offset = struct.unpack_from("<I", data, 44)[0]
    string_size = struct.unpack_from("<I", data, 48)[0]
    source_map_offset, source_map_size = struct.unpack_from("<II", data, 52)
    if (source_map_offset == 0) != (source_map_size == 0):
        fail("invalid source map range")
    if source_map_size and source_map_offset + source_map_size > len(data):
        fail("source map is truncated")
    if section_offset + section_count * SECTION_SIZE > len(data):
        fail("section table is truncated")
    if symbol_offset + symbol_count * SYMBOL_SIZE > len(data):
        fail("symbol table is truncated")
    if string_offset + string_size > len(data):
        fail("string table is truncated")
    strings = data[string_offset:string_offset + string_size]
    ranges: list[tuple[int, int]] = []

    def add_range(offset: int, length: int, label: str) -> None:
        if length == 0:
            return
        if offset < 0 or offset + length > len(data):
            fail(f"{label} is outside the object")
        end = offset + length
        for prior_offset, prior_length in ranges:
            if offset < prior_offset + prior_length and prior_offset < end:
                fail(f"{label} overlaps another object range")
        ranges.append((offset, length))

    add_range(0, HEADER_SIZE, "header")
    add_range(section_offset, section_count * SECTION_SIZE, "section table")
    add_range(symbol_offset, symbol_count * SYMBOL_SIZE, "symbol table")
    add_range(string_offset, string_size, "string table")
    add_range(source_map_offset, source_map_size, "source map")
    target_offset, target_size = struct.unpack_from("<II", data, 20)
    target_contained = (string_offset <= target_offset and
                        target_offset + target_size <= string_offset + string_size)
    if not target_contained:
        add_range(target_offset, target_size, "target triple")
    sections: list[dict] = []
    section_names: set[str] = set()
    for index in range(section_count):
        record = data[section_offset + index * SECTION_SIZE:
                       section_offset + (index + 1) * SECTION_SIZE]
        name_offset, payload_offset, payload_size, reloc_offset, reloc_count, align = struct.unpack_from("<IIIIII", record, 0)
        kind = record[24]
        target_index = struct.unpack_from("<H", record, 26)[0]
        if (name_offset >= len(strings) or align > 15 or kind > 3 or
                record[25] != 0 or target_index != index or
                struct.unpack_from("<I", record, 28)[0] != 0 or
                struct.unpack_from("<I", record, 32)[0] != 0):
            fail("invalid section record")
        if kind != 3 and payload_size > 256 * 1024 * 1024:
            fail("section payload exceeds 256 MiB")
        if kind != 3 and payload_offset + payload_size > len(data):
            fail("section payload is truncated")
        if kind == 3 and (payload_offset != 0 or struct.unpack_from("<I", record, 36)[0] != 0):
            fail("invalid BSS section")
        if kind != 3 and struct.unpack_from("<I", record, 36)[0] != crc32(
                data[payload_offset:payload_offset + payload_size]):
            fail("section CRC mismatch")
        if ((kind in (0, 1) and align < 4) or
                (reloc_count and reloc_offset + reloc_count * RELOC_SIZE > len(data))):
            fail("invalid section alignment or relocation table")
        if kind != 3:
            add_range(payload_offset, payload_size, "section payload")
        if reloc_count:
            add_range(reloc_offset, reloc_count * RELOC_SIZE,
                      "relocation table")
        name_end = strings.find(b"\0", name_offset)
        if name_end <= name_offset:
            fail("invalid section name")
        name = strings[name_offset:name_end].decode("ascii")
        if name in section_names:
            fail("duplicate section name")
        section_names.add(name)
        sections.append({
            "name": name,
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
        if (name_offset >= len(strings) or binding > 1 or kind > 4 or
                struct.unpack_from("<H", record, 14)[0] != 0 or
                struct.unpack_from("<I", record, 24)[0] != 0 or
                struct.unpack_from("<I", record, 28)[0] != 0):
            fail("invalid symbol record")
        name_end = strings.find(b"\0", name_offset)
        if name_end < 0:
            fail("unterminated symbol name")
        if section_index != 0xFFFFFFFF and section_index >= section_count:
            fail("symbol section is out of range")
        if (section_index != 0xFFFFFFFF and kind in (1, 2, 3) and
                value + size > sections[section_index]["size"]):
            fail("symbol range is outside its section")
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
            if (width not in (4, 8) or kind not in (1, 2, 3, 4, 5) or
                    struct.unpack_from("<I", entry, 28)[0] != 0 or
                    (kind in (1, 3, 4) and width != 4) or
                    (kind in (2, 5) and width != 8)):
                fail("unsupported relocation")
            relocations.append({"section": section_index, "offset": where,
                                "symbol": symbol_index, "type": kind,
                                "addend": addend, "width": width})
    covered = bytearray(len(data))
    for offset, length in ranges:
        covered[offset:offset + length] = b"\1" * length
    if any(value == 0 and byte != 0 for value, byte in zip(covered, data)):
        fail("object contains undescribed data")
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
