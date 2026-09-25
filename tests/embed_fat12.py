#!/usr/bin/env python3
"""Place a raw COM file into the project target's known FAT12 volume."""

from __future__ import annotations

import argparse
import pathlib
import struct

SECTOR = 512
RESERVED = 1
FAT_COUNT = 2
FAT_SECTORS = 9
ROOT_ENTRIES = 224
ROOT_SECTORS = (ROOT_ENTRIES * 32 + SECTOR - 1) // SECTOR
VOLUME_LBA = 512
DATA_LBA = VOLUME_LBA + RESERVED + FAT_COUNT * FAT_SECTORS + ROOT_SECTORS
FAT_LBA = VOLUME_LBA + RESERVED
ROOT_LBA = FAT_LBA + FAT_COUNT * FAT_SECTORS


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
    mirror = (FAT_LBA + FAT_COUNT - 1) * SECTOR + (cluster * 3) // 2
    image[mirror] = image[first]
    image[mirror + 1] = image[first + 1]


def allocate(image: bytearray) -> int:
    for cluster in range(2, 2880):
        if fat_get(image, cluster) == 0:
            fat_set(image, cluster, 0xFFF)
            return cluster
    raise ValueError("FAT12 volume is full")


def name83(name: str) -> bytes:
    if "." not in name:
        name += ".COM"
    base, extension = name.split(".", 1)
    if len(base) > 8 or len(extension) > 3 or not base or not extension:
        raise ValueError("name is not an 8.3 file name")
    return base.upper().encode("ascii").ljust(8, b" ") + extension.upper().encode("ascii").ljust(3, b" ")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=pathlib.Path)
    parser.add_argument("program", type=pathlib.Path)
    parser.add_argument("name")
    args = parser.parse_args()
    image = bytearray(args.image.read_bytes())
    payload = args.program.read_bytes()
    if len(payload) > 0x10000:
        raise ValueError("test program exceeds reserved raw COM budget")
    directory = ROOT_LBA * SECTOR
    encoded = name83(args.name)
    entry = None
    for index in range(ROOT_ENTRIES):
        offset = directory + index * 32
        first = image[offset]
        if first in (0, 0xE5):
            entry = offset
            break
    if entry is None:
        raise ValueError("root directory has no free entry")
    cluster = allocate(image)
    cluster_count = (len(payload) + SECTOR - 1) // SECTOR
    if cluster_count > 1:
        for index in range(cluster_count - 1):
            fat_set(image, cluster + index, cluster + index + 1)
        fat_set(image, cluster + cluster_count - 1, 0xFFF)
    data = (DATA_LBA + (cluster - 2)) * SECTOR
    image[data:data + len(payload)] = payload
    if len(payload) % SECTOR:
        image[data + len(payload):data + cluster_count * SECTOR] = b"\0" * (cluster_count * SECTOR - len(payload))
    image[entry:entry + 11] = encoded
    image[entry + 11] = 0x20
    struct.pack_into("<H", image, entry + 26, cluster)
    struct.pack_into("<I", image, entry + 28, len(payload))
    args.image.write_bytes(image)
    print(f"embedded {args.name} cluster={cluster} sectors={cluster_count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
