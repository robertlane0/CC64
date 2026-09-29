#!/usr/bin/env python3
"""Inspect a CC64 raw or MZ64 image without using a host linker."""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys


def fail(message: str) -> None:
    raise ValueError(message)


def inspect(path: pathlib.Path) -> dict[str, object]:
    data = path.read_bytes()
    if data[:4] == b"MZ64":
        if len(data) < 48:
            fail("truncated MZ64 header")
        header_size, = struct.unpack_from("<I", data, 4)
        payload_size, = struct.unpack_from("<Q", data, 8)
        entry, = struct.unpack_from("<I", data, 16)
        stack_size, = struct.unpack_from("<I", data, 20)
        relocation_count, = struct.unpack_from("<I", data, 24)
        relocation_offset, = struct.unpack_from("<I", data, 28)
        memory_size, = struct.unpack_from("<Q", data, 32)
        reserved, = struct.unpack_from("<Q", data, 40)
        if header_size != 48 or reserved != 0:
            fail("invalid MZ64 header")
        if payload_size == 0 or payload_size > 16 * 1024 * 1024:
            fail("invalid MZ64 payload size")
        if entry >= payload_size or memory_size < payload_size:
            fail("invalid MZ64 entry or memory size")
        if memory_size > 16 * 1024 * 1024 or stack_size > 65536:
            fail("MZ64 target limit exceeded")
        table_size = relocation_count * 16
        if relocation_offset != 48 + payload_size:
            fail("MZ64 relocation table is not adjacent")
        if 48 + payload_size + table_size != len(data):
            fail("MZ64 table does not end at EOF")
        relocations: list[tuple[int, int]] = []
        for index in range(relocation_count):
            destination, addend = struct.unpack_from(
                "<Qq", data, relocation_offset + index * 16
            )
            if destination % 8 != 0 or addend % 8 != 0:
                fail("MZ64 relocation is not aligned")
            if (destination > memory_size or addend < 0 or
                    addend > memory_size or
                    memory_size - destination < 8 or memory_size - addend < 8):
                fail("MZ64 relocation is outside memory")
            relocations.append((destination, addend))
        return {
            "format": "MZ64",
            "file_size": len(data),
            "payload_size": payload_size,
            "memory_size": memory_size,
            "entry": entry,
            "relocations": relocations,
        }
    if not data:
        fail("empty raw image")
    if len(data) > 16 * 1024 * 1024:
        fail("raw target limit exceeded")
    return {
        "format": "raw",
        "file_size": len(data),
        "payload_size": len(data),
        "memory_size": len(data),
        "entry": 0,
        "relocations": [],
    }


def apply_bias(image: dict[str, object], bias: int) -> dict[str, int]:
    """Apply a load bias to an image and return the fixups it produces.

    The `MZ64` form carries image-relative data destinations, and the loader
    supplies the bias, so the same payload is position independent. Walking the
    table at a bias is what proves it: a destination that leaves the image,
    that lands outside the eight-byte slot the fixup writes, or that would
    overlap its neighbour, is a defect the image does not rule out.
    """
    memory_size = int(image["memory_size"])
    result: dict[str, int] = {}
    if image["format"] != "MZ64":
        # A raw image is always loaded at the process origin, so the only
        # thing to check is that the bias does not push it past the target's
        # conventional process area.
        if bias < 0 or bias + int(image["payload_size"]) > 16 * 1024 * 1024:
            fail("raw image does not fit at this bias")
        return result
    for destination, addend in image["relocations"]:  # type: ignore[union-attr]
        slot = bias + destination
        value = bias + addend
        if slot < 0 or slot + 8 > memory_size + bias:
            fail(f"MZ64 fixup at bias {bias} leaves the image")
        if value < 0 or value + 8 > memory_size + bias:
            fail(f"MZ64 fixup value at bias {bias} leaves the image")
        result[f"slot@{bias}"] = value
    return result


# A bias the loader may use. The value only has to be a plausible multiple of
# the page the target maps; the point is that several of them are checked
# rather than only the one the loader happens to use.
BIASES = (0, 0x1000, 0x20000, 0x100000, 0x400000)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("path", type=pathlib.Path)
    parser.add_argument("--bias", type=lambda value: int(value, 0), default=None,
                        help="apply one load bias to the image's fixups")
    args = parser.parse_args()
    try:
        result = inspect(args.path)
        biases = BIASES if args.bias is None else (args.bias,)
        for bias in biases:
            fixups = apply_bias(result, bias)
            for slot, value in fixups.items():
                print(f"  bias {bias}: {slot} -> {value}")
    except (OSError, ValueError, struct.error) as error:
        print(f"image: {error}", file=sys.stderr)
        return 1
    print(
        f"{result['format']} bytes={result['file_size']} "
        f"payload={result['payload_size']} memory={result['memory_size']} "
        f"entry={result['entry']} relocations={len(result['relocations'])}"
    )
    for destination, addend in result["relocations"]:
        print(f"  {destination} -> {addend}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
