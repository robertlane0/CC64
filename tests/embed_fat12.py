#!/usr/bin/env python3
"""Place files into the project target's known FAT12 volume.

One file per invocation, or a whole set at once with `--manifest`, so a run that
needs a source tree on the volume does not rewrite the image once per file. The
geometry is the one the target's own volume uses.
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys

SECTOR = 512
RESERVED = 1
FAT_COUNT = 2
FAT_SECTORS = 9
ROOT_ENTRIES = 224
ROOT_SECTORS = (ROOT_ENTRIES * 32 + SECTOR - 1) // SECTOR
# The volume's address is a property of the target image, not of this tool: the
# target chooses where its volume sits and may move it, so the address is found
# in the image rather than assumed here. `volume_geometry` reads the boot sector
# the target stamped, which carries the OEM name and the sector size, and the
# fields below are the rest of the geometry the target documents.
DEFAULT_VOLUME_LBA = 512
VOLUME_LBA = DEFAULT_VOLUME_LBA
DATA_LBA = VOLUME_LBA + RESERVED + FAT_COUNT * FAT_SECTORS + ROOT_SECTORS
FAT_LBA = VOLUME_LBA + RESERVED
ROOT_LBA = FAT_LBA + FAT_COUNT * FAT_SECTORS
TOTAL_CLUSTERS = 2880


def volume_geometry(image: bytes) -> tuple[int, int, int, int, int]:
    """Return (volume LBA, FAT count, FAT sectors, root sectors, total sectors).

    The boot sector the target's own stamper wrote is identified by its OEM
    name and signature, so the only sector that can be mistaken for it is one
    the target also wrote. A volume is required to be found: an image with no
    recognisable volume is reported rather than written at a guessed address.
    """
    for lba in range(1, len(image) // SECTOR):
        sector = image[lba * SECTOR: lba * SECTOR + SECTOR]
        if sector[3:11] != b"MSDOS64 " or sector[510:512] != b"\x55\xaa":
            continue
        byts = sector[11] | (int(sector[12]) << 8)
        if byts != SECTOR:
            continue
        fats = sector[16]
        fat_sectors = sector[22] | (int(sector[23]) << 8)
        total = sector[19] | (int(sector[20]) << 8)
        root_sectors = (ROOT_ENTRIES * 32 + SECTOR - 1) // SECTOR
        return lba, fats, fat_sectors, root_sectors, total
    raise ValueError("no target volume found in the image")


def locate(image: bytes) -> None:
    """Point the geometry constants at the volume this image actually has."""
    global VOLUME_LBA, DATA_LBA, FAT_LBA, ROOT_LBA, FAT_SECTORS
    global ROOT_SECTORS, TOTAL_CLUSTERS, FAT_COUNT
    volume, fats, fat_sectors, roots, total = volume_geometry(image)
    VOLUME_LBA = volume
    FAT_COUNT = fats
    FAT_SECTORS = fat_sectors
    ROOT_SECTORS = roots
    TOTAL_CLUSTERS = total
    DATA_LBA = volume + RESERVED + fats * fat_sectors + roots
    FAT_LBA = volume + RESERVED
    ROOT_LBA = FAT_LBA + fats * fat_sectors


def fat_get(image: bytearray, cluster: int) -> int:
    first = FAT_LBA * SECTOR + (cluster * 3) // 2
    value = image[first] | (image[first + 1] << 8)
    if cluster & 1:
        return value >> 4
    return value & 0x0FFF


def fat_set(image: bytearray, cluster: int, value: int) -> None:
    first = FAT_LBA * SECTOR + (cluster * 3) // 2
    word = image[first] | (image[first + 1] << 8)
    if cluster & 1:
        word = (word & 0x000F) | ((value & 0x0FFF) << 4)
    else:
        word = (word & 0xF000) | (value & 0x0FFF)
    image[first] = word & 0xFF
    image[first + 1] = word >> 8
    mirror = (FAT_LBA + FAT_SECTORS) * SECTOR + (cluster * 3) // 2
    image[mirror] = image[first]
    image[mirror + 1] = image[first + 1]


def allocate(image: bytearray, count: int) -> int:
    """Find a run of `count` free clusters, so a file's chain stays contiguous."""
    run_start = 0
    run = 0
    for cluster in range(2, TOTAL_CLUSTERS):
        if fat_get(image, cluster) != 0:
            if run and run < count:
                run = 0
            continue
        if run == 0:
            run_start = cluster
        run += 1
        if run == count:
            for index in range(count):
                fat_set(image, run_start + index, 0xFFF)
            return run_start
    raise ValueError("FAT12 volume cannot hold the requested payload")


def name83(name: str) -> bytes:
    if "." not in name:
        name += ".COM"
    base, extension = name.split(".", 1)
    if len(base) > 8 or len(extension) > 3 or not base or not extension:
        raise ValueError(f"name is not an 8.3 file name: {name}")
    return base.upper().encode("ascii").ljust(8, b" ") + extension.upper().encode("ascii").ljust(3, b" ")


def free_entry(image: bytearray) -> int:
    directory = ROOT_LBA * SECTOR
    for index in range(ROOT_ENTRIES):
        offset = directory + index * 32
        if image[offset] in (0, 0xE5):
            return offset
    raise ValueError("root directory has no free entry")


def add_file(image: bytearray, name: str, payload: bytes) -> tuple[int, int]:
    # A raw COM test program is small, but a self-hosted compiler image is an
    # MZ64 payload of a few hundred kilobytes. The budget below keeps a
    # malformed or oversized input from filling the volume, and the allocator
    # still fails loudly when the volume is genuinely full.
    payload_budget = 768 * 1024
    if len(payload) > payload_budget:
        raise ValueError(f"{name} exceeds the reserved volume budget")
    cluster_count = max(1, (len(payload) + SECTOR - 1) // SECTOR)
    entry = free_entry(image)
    cluster = allocate(image, cluster_count)
    if cluster_count > 1:
        for index in range(cluster_count - 1):
            fat_set(image, cluster + index, cluster + index + 1)
    data = (DATA_LBA + (cluster - 2)) * SECTOR
    image[data:data + len(payload)] = payload
    if len(payload) % SECTOR:
        image[data + len(payload):data + cluster_count * SECTOR] = (
            b"\0" * (cluster_count * SECTOR - len(payload)))
    encoded = name83(name)
    image[entry:entry + 11] = encoded
    image[entry + 11] = 0x20
    struct.pack_into("<H", image, entry + 26, cluster)
    struct.pack_into("<I", image, entry + 28, len(payload))
    return cluster, cluster_count


def read_manifest(path: pathlib.Path) -> list[tuple[str, pathlib.Path]]:
    entries: list[tuple[str, pathlib.Path]] = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        text = line.strip()
        if not text or text.startswith("#"):
            continue
        parts = text.split()
        if len(parts) != 2:
            raise ValueError(f"{path}:{number}: expected 'NAME PATH'")
        entries.append((parts[0], pathlib.Path(parts[1])))
    return entries


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=pathlib.Path)
    parser.add_argument("program", type=pathlib.Path, nargs="?")
    parser.add_argument("name", nargs="?")
    parser.add_argument("--manifest", type=pathlib.Path,
                        help="file of 'NAME PATH' lines to embed in one pass")
    args = parser.parse_args()
    if args.manifest is None and (args.program is None or args.name is None):
        parser.error("either a program and name, or --manifest, is required")
    if args.manifest is not None and (args.program is not None or args.name is not None):
        parser.error("--manifest cannot be combined with a program and name")
    image = bytearray(args.image.read_bytes())
    locate(bytes(image))
    if args.manifest is not None:
        entries = read_manifest(args.manifest)
        for name, path in entries:
            cluster, sectors = add_file(image, name, path.read_bytes())
            print(f"embedded {name} cluster={cluster} sectors={sectors}")
    else:
        cluster, sectors = add_file(image, args.name, args.program.read_bytes())
        print(f"embedded {args.name} cluster={cluster} sectors={sectors}")
    args.image.write_bytes(image)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ValueError as error:
        print(f"embed: {error}", file=sys.stderr)
        raise SystemExit(1) from None
