#!/usr/bin/env python3
"""Read a file back out of the project target's FAT12 volume.

`embed_fat12.py` places a file into the volume; this tool reads one back so a
test can compare bytes the target produced with bytes the host produced. The
geometry is the one the target's own volume uses, read from the same constants
the writer uses, so the two cannot disagree about where a file lives.
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from embed_fat12 import (  # noqa: E402  (path is set above)
    DATA_LBA, FAT_COUNT, FAT_SECTORS, ROOT_ENTRIES, ROOT_LBA, SECTOR, fat_get,
)

MAXIMUM_CLUSTERS = 4096


def directory(image: bytes) -> list[tuple[str, int, int]]:
    """Return (name, first cluster, size) for every root directory entry."""
    entries: list[tuple[str, int, int]] = []
    root = ROOT_LBA * SECTOR
    for index in range(ROOT_ENTRIES):
        record = image[root + index * 32: root + index * 32 + 32]
        if len(record) < 32 or record[0] in (0x00, 0xE5):
            if record[0] == 0x00:
                break
            continue
        if record[11] == 0x0F:          # long-name filler, not a real entry
            continue
        stem = record[0:8].decode("ascii", "replace").rstrip()
        extension = record[8:11].decode("ascii", "replace").rstrip()
        name = stem + ("." + extension if extension else "")
        cluster = struct.unpack_from("<H", record, 26)[0]
        size = struct.unpack_from("<I", record, 28)[0]
        entries.append((name, cluster, size))
    return entries


def read_file(image: bytes, name: str) -> bytes:
    wanted = name.upper()
    for entry, cluster, size in directory(image):
        if entry != wanted:
            continue
        out = bytearray()
        seen = 0
        while 2 <= cluster < 0xFF0 and seen <= MAXIMUM_CLUSTERS:
            start = (DATA_LBA + cluster - 2) * SECTOR
            out += image[start:start + SECTOR]
            cluster = fat_get(bytearray(image), cluster)
            seen += 1
        return bytes(out[:size])
    raise SystemExit(f"extract: {name} is not present in the volume")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image")
    parser.add_argument("name", nargs="?",
                        help="file to read; omit to list the volume")
    parser.add_argument("-o", "--output", help="write the file here instead of stdout")
    parser.add_argument("--expect-fat", type=int, default=FAT_COUNT,
                        help="number of file allocation tables (sanity check)")
    arguments = parser.parse_args()
    image = pathlib.Path(arguments.image).read_bytes()
    fat_start = (512 + 1) * SECTOR
    if arguments.expect_fat != FAT_COUNT:
        raise SystemExit("extract: unexpected volume geometry")
    if len(image) < fat_start + SECTOR:
        raise SystemExit("extract: image is smaller than the volume header")
    if arguments.name is None:
        for entry, cluster, size in directory(image):
            print(f"{entry:14s} cluster={cluster:<5d} size={size}")
        return 0
    data = read_file(image, arguments.name)
    if arguments.output:
        pathlib.Path(arguments.output).write_bytes(data)
    else:
        sys.stdout.buffer.write(data)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
