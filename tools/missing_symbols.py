#!/usr/bin/env python3
"""Report external symbols referenced by CC64O objects but not defined by them.

The linker's first unresolved symbol stops the link, so this helper reads every
object with the project-owned reader logic and reports the full set. It is a
development aid for the self-host target library, not a build gate.
"""

from __future__ import annotations

import pathlib
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER_SIZE = 96
SECTION_SIZE = 40
SYMBOL_SIZE = 32
STB_GLOBAL = 1
SHN_UNDEF = 0xFFFFFFFF


def read_object(path: pathlib.Path) -> list[tuple[str, bool]]:
    data = path.read_bytes()
    symbol_table, symbol_count = struct.unpack_from("<II", data, 36)
    strings, strings_size = struct.unpack_from("<II", data, 44)
    symbols: list[tuple[str, bool]] = []
    for index in range(symbol_count):
        record = symbol_table + index * SYMBOL_SIZE
        name_offset, _value, section_index = struct.unpack_from("<III", data, record)
        binding = data[record + 12]
        end = data.index(b"\0", strings + name_offset, strings + strings_size)
        name = data[strings + name_offset:end].decode("utf-8", "replace")
        undefined = section_index == SHN_UNDEF
        symbols.append((name, undefined and binding == STB_GLOBAL))
    return symbols


def main() -> int:
    objects = [pathlib.Path(argument) for argument in sys.argv[1:]]
    if not objects:
        print("usage: missing_symbols.py OBJECT...", file=sys.stderr)
        return 2
    defined: set[str] = set()
    referenced: dict[str, str] = {}
    for path in objects:
        for name, undefined in read_object(path):
            if undefined:
                referenced.setdefault(name, path.name)
            elif name:
                defined.add(name)
    missing = sorted(name for name in referenced if name not in defined)
    for name in missing:
        print(f"{name}\t(referenced by {referenced[name]})")
    print(f"{len(missing)} unresolved of {len(referenced)} referenced")
    return 0 if not missing else 1


if __name__ == "__main__":
    raise SystemExit(main())
