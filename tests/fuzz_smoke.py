#!/usr/bin/env python3
"""Deterministic malformed source, object, and image smoke tests."""

from __future__ import annotations

import pathlib
import random
import struct
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def run_compiler(arguments: list[str], label: str, expect_success: bool) -> int:
    try:
        result = subprocess.run(
            [str(ROOT / "cc64"), *arguments], cwd=ROOT,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            timeout=5, check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise SystemExit(f"compiler timed out on {label}") from error
    if result.returncode < 0:
        raise SystemExit(f"compiler crashed on {label}")
    if expect_success and result.returncode != 0:
        raise SystemExit(f"compiler rejected {label}")
    if not expect_success and result.returncode == 0:
        raise SystemExit(f"compiler accepted malformed {label}")
    return result.returncode


def run_inspector(path: pathlib.Path, label: str, expect_success: bool) -> None:
    try:
        result = subprocess.run(
            ["python3", str(ROOT / "tools/inspect_image.py"), str(path)],
            cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            timeout=5, check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise SystemExit(f"image inspector timed out on {label}") from error
    if expect_success and result.returncode != 0:
        raise SystemExit(f"image inspector rejected {label}")
    if not expect_success and result.returncode == 0:
        raise SystemExit(f"image inspector accepted malformed {label}")


def run_object_inspector(path: pathlib.Path, label: str, expect_success: bool) -> None:
    try:
        result = subprocess.run(
            ["python3", str(ROOT / "tools/inspect_object.py"), str(path)],
            cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            timeout=5, check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise SystemExit(f"object inspector timed out on {label}") from error
    if expect_success and result.returncode != 0:
        raise SystemExit(f"object inspector rejected {label}")
    if not expect_success and result.returncode == 0:
        raise SystemExit(f"object inspector accepted malformed {label}")


def crc32(data: bytes | bytearray) -> int:
    value = 0xFFFFFFFF
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0xEDB88320 if value & 1 else 0)
    return value ^ 0xFFFFFFFF


def refresh_object_crc(data: bytearray) -> None:
    data[64:68] = b"\0\0\0\0"
    struct.pack_into("<I", data, 64, crc32(data[:64]))
    data[68:72] = b"\0\0\0\0"
    struct.pack_into("<I", data, 68, crc32(data[:68]))


def changed_u32(original: bytes, offset: int, value: int, label: str,
                work: pathlib.Path, index: int) -> None:
    mutated = bytearray(original)
    struct.pack_into("<I", mutated, offset, value)
    refresh_object_crc(mutated)
    bad = work / f"structural-{index}.o"
    bad.write_bytes(mutated)
    run_object_inspector(bad, label, False)
    output = work / f"structural-{index}.com"
    run_compiler(["--link", str(bad), "-o", str(output)], label, False)
    if output.exists():
        output.unlink()
        raise SystemExit(f"failed structural link left output: {label}")


def main() -> int:
    rng = random.Random(0xCC64)
    with tempfile.TemporaryDirectory(prefix="cc64-fuzz-") as temp:
        work = pathlib.Path(temp)
        source_cases = [
            b"#include \"missing-header.h\"\n",
            b"#define X X\nX\n",
            b"int main(void) { return 1;\n",
            b"\xff\xfe\x00",
        ]
        for index in range(40):
            length = rng.randrange(0, 257)
            source_cases.append(bytes(rng.randrange(0, 256) for _ in range(length)))
        for index, contents in enumerate(source_cases):
            source = work / f"source-{index}.c"
            output = work / f"source-{index}.o"
            source.write_bytes(contents)
            run_compiler(["-c", str(source), "-o", str(output)],
                         f"malformed source {index}", False)
            if output.exists():
                output.unlink()
                raise SystemExit(f"failed source compile left output: {index}")

        valid = work / "valid.c"
        valid.write_text("int main(void) { return 3; }\n", encoding="utf-8")
        object_file = work / "valid.o"
        run_compiler(["-c", str(valid), "-o", str(object_file)],
                     "valid source", True)
        run_object_inspector(object_file, "valid object", True)
        data = object_file.read_bytes()
        for index in range(20):
            mutated = bytearray(data)
            mutated[rng.randrange(len(mutated))] ^= 1 << rng.randrange(8)
            bad = work / f"bit-{index}.o"
            output = work / f"bit-{index}.com"
            bad.write_bytes(mutated)
            run_object_inspector(bad, f"mutated object {index}", False)
            run_compiler(["--link", str(bad), "-o", str(output)],
                         f"mutated object {index}", False)
            if output.exists():
                output.unlink()
                raise SystemExit(f"failed object link left output: {index}")

        section_table = struct.unpack_from("<I", data, 28)[0]
        changed_u32(data, 32, 0xFFFFFFFF, "oversized section count", work, 0)
        changed_u32(data, section_table + 20, 1, " undersized text alignment", work, 1)
        changed_u32(data, section_table + 16, 0xFFFFFFFF,
                    "oversized relocation count", work, 2)
        changed_u32(data, section_table + 8, 0xFFFFFFFF,
                    "oversized section payload", work, 3)
        truncated = work / "truncated.o"
        truncated.write_bytes(data[:max(1, len(data) // 2)])
        run_object_inspector(truncated, "truncated object", False)
        truncated_output = work / "truncated.com"
        run_compiler(["--link", str(truncated), "-o", str(truncated_output)],
                     "truncated object", False)
        if truncated_output.exists():
            truncated_output.unlink()
            raise SystemExit("failed truncated link left output")

        image_source = work / "image.c"
        image_source.write_text(
            "int value = 7; int *pointer = &value; "
            "int main(void) { return *pointer; }\n", encoding="utf-8")
        image_object = work / "image.o"
        image = work / "image.mz64"
        run_compiler(["-c", str(image_source), "-o", str(image_object)],
                     "image source", True)
        run_compiler(["--link", "--format", "mz64", str(image_object),
                      "-o", str(image)], "valid MZ64 image", True)
        run_inspector(image, "valid MZ64 image", True)
        image_data = image.read_bytes()
        relocation_offset = struct.unpack_from("<I", image_data, 28)[0]
        # Every field the loader reads is bounded, so a value the loader would
        # act on is rejected before it is used rather than after.
        image_mutations = [
            ("empty image", b""),
            ("truncated MZ64 header", b"MZ64"),
            ("truncated MZ64", image_data[:-1]),
        ]
        field_mutations = [
            (4, 47, "wrong MZ64 header size"),
            (8, 0, "zero MZ64 payload size"),
            (8, 17 * 1024 * 1024, "oversized MZ64 payload size"),
            (16, 1 << 20, "MZ64 entry past the payload"),
            (20, 1 << 20, "oversized MZ64 stack request"),
            (24, 2, "MZ64 relocation count disagrees with the table"),
            (28, 32, "MZ64 relocation table is not adjacent"),
            (32, 1, "MZ64 memory size below the payload"),
            (32, 17 * 1024 * 1024, "oversized MZ64 memory size"),
            (40, 1, "reserved MZ64 field is not zero"),
        ]
        for index, (offset, value, label) in enumerate(field_mutations):
            mutated = bytearray(image_data)
            struct.pack_into("<I", mutated, offset, value)
            image_mutations.append((label, bytes(mutated)))
        negative = bytearray(image_data)
        struct.pack_into("<q", negative, relocation_offset + 8, -8)
        image_mutations.append(("negative MZ64 addend", bytes(negative)))
        unaligned = bytearray(image_data)
        struct.pack_into("<Q", unaligned, relocation_offset, 1)
        image_mutations.append(("unaligned MZ64 fixup", bytes(unaligned)))
        for index, (label, contents) in enumerate(image_mutations):
            bad = work / f"image-{index}.mz64"
            bad.write_bytes(contents)
            run_inspector(bad, label, False)

        # A load bias is applied to a valid image at several biases, and a
        # bias that would push the image past the target's process area is
        # refused. The `MZ64` form is position independent, so every bias has
        # to leave its fixups inside the loaded image.
        raw_source = work / "raw.c"
        raw_source.write_text("int main(void) { return 7; }\n", encoding="utf-8")
        raw_object = work / "raw.o"
        raw_image = work / "raw.com"
        run_compiler(["-c", str(raw_source), "-o", str(raw_object)],
                     "raw source", True)
        run_compiler(["--link", str(raw_object), "-o", str(raw_image)],
                     "valid raw image", True)
        run_inspector(raw_image, "valid raw image", True)
        for bias in (0, 0x1000, 0x20000, 0x100000, 0x400000):
            accepted = subprocess.run(
                ["python3", str(ROOT / "tools/inspect_image.py"), str(image),
                 "--bias", hex(bias)], cwd=ROOT,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                timeout=5, check=False)
            if accepted.returncode != 0:
                raise SystemExit(f"image inspector refused bias {hex(bias)}")
            accepted = subprocess.run(
                ["python3", str(ROOT / "tools/inspect_image.py"), str(raw_image),
                 "--bias", hex(bias)], cwd=ROOT,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                timeout=5, check=False)
            if accepted.returncode != 0:
                raise SystemExit(f"raw inspector refused bias {hex(bias)}")
        for bias in (-1, 16 * 1024 * 1024):
            rejected = subprocess.run(
                ["python3", str(ROOT / "tools/inspect_image.py"), str(raw_image),
                 "--bias", str(bias)], cwd=ROOT,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                timeout=5, check=False)
            if rejected.returncode == 0:
                raise SystemExit(f"raw inspector accepted bias {bias}")
    print("fuzz: deterministic source/object/image smoke passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
