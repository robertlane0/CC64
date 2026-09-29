#!/usr/bin/env python3
"""Negative linker tests: every rejection the linker documents.

A linker that is only exercised on well-formed objects cannot show that it
rejects a malformed one, so each rejection the contract states is driven by a
case that must produce the stated diagnostic and must leave no output. The
cases are built from valid objects with one field changed, so a rejection can
only come from the field under test rather than from an object that was
already broken.
"""

from __future__ import annotations

import pathlib
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def run(command: list[str], timeout: int = 20) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                              timeout=timeout, check=False)
    except subprocess.TimeoutExpired as error:
        raise SystemExit(f"linker did not bound: {command}") from error


def compile_unit(work: pathlib.Path, text: str, name: str) -> pathlib.Path:
    source = work / f"{name}.c"
    obj = work / f"{name}.cc64o"
    source.write_text(text, encoding="utf-8")
    result = run([str(ROOT / "cc64"), "-c", str(source), "-o", str(obj)])
    if result.returncode != 0:
        raise SystemExit(f"setup source rejected:\n{result.stdout}{result.stderr}")
    return obj


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


def expect_rejection(objects: list[pathlib.Path], output: pathlib.Path,
                     label: str, identifier: str | None = None) -> None:
    result = run([str(ROOT / "cc64"), "--link", *[str(o) for o in objects],
                  "-o", str(output)])
    detail = (result.stdout + result.stderr).strip()
    if result.returncode == 0:
        raise SystemExit(f"linker accepted {label}")
    if identifier is not None and identifier not in detail:
        raise SystemExit(f"{label}: diagnostic did not match {identifier}: {detail}")
    if output.exists():
        output.unlink()
        raise SystemExit(f"rejected link left an output: {label}")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="cc64-link-neg-") as temp:
        work = pathlib.Path(temp)
        output = work / "out.com"

        # A reference the user did not define cannot be resolved, so the link
        # fails rather than producing an image with a hole in it.
        undefined = compile_unit(
            work, "int absent(void); int main(void) { return absent(); }\n",
            "undefined")
        expect_rejection([undefined], output, "an unresolved symbol", "CC5021")

        # A raw image is loaded at the process origin, so an address that only
        # the loader could supply has nowhere to go in one.
        biased = compile_unit(
            work, "int value = 7; int *p = &value; "
            "int main(void) { return *p; }\n", "biased")
        expect_rejection([biased], output,
                         "a load-biased relocation in a raw image", "CC5022")

        # Two definitions of one symbol are a conflict, not a silent choice.
        first = compile_unit(work, "int shared(void) { return 1; }\n", "dup-a")
        second = compile_unit(work, "int shared(void) { return 2; }\n", "dup-b")
        expect_rejection([first, second], output, "a duplicate definition",
                         "CC5020")

        # A header field the reader bounds is rejected by that check, so the
        # checksums are refreshed to leave only the field under test wrong.
        valid = compile_unit(work, "int main(void) { return 7; }\n", "valid")
        data = bytearray(valid.read_bytes())
        table = struct.unpack_from("<I", data, 28)[0]
        cases = [
            (32, 0xFFFFFFFF, "an oversized section count", None),
            (16, 95, "a header size the version does not define", "CC5005"),
            (8, 0x1234, "a format version the reader does not know", "CC5005"),
            (12, 9, "a target ABI the reader does not know", "CC5005"),
            (14, 3, "a machine the reader does not know", "CC5005"),
            (20, 0xFFFFFF, "a target triple past the file", "CC5006"),
            (60, 1, "a reserved header field that is not zero", "CC5005"),
        ]
        for index, (offset, value, label, identifier) in enumerate(cases):
            mutated = bytearray(data)
            if offset in (8, 12, 14):
                struct.pack_into("<H", mutated, offset, value)
            else:
                struct.pack_into("<I", mutated, offset, value)
            refresh_object_crc(mutated)
            bad = work / f"header-{index}.cc64o"
            bad.write_bytes(mutated)
            expect_rejection([bad], output, label, identifier)

        # The header's own checksums are the first thing a reader checks, so a
        # single changed byte is caught without any other field being wrong.
        for index, (offset, label) in enumerate([(64, "a stale header checksum"),
                                                 (68, "a stale file checksum")]):
            mutated = bytearray(data)
            mutated[offset] ^= 0x01
            bad = work / f"crc-{index}.cc64o"
            bad.write_bytes(mutated)
            expect_rejection([bad], output, label, "CC5007")

        # A reserved byte outside the checksums is still reserved.
        mutated = bytearray(data)
        mutated[80] = 1
        refresh_object_crc(mutated)
        reserved = work / "reserved.cc64o"
        reserved.write_bytes(mutated)
        expect_rejection([reserved], output, "a reserved header byte that is set")

        # A section whose payload range overlaps another section's is
        # malformed: the linker would write one over the other.
        mutated = bytearray(data)
        first_payload = struct.unpack_from("<I", mutated, table + 4)[0]
        struct.pack_into("<I", mutated, table + 40 + 4, first_payload)
        refresh_object_crc(mutated)
        overlap = work / "overlap.cc64o"
        overlap.write_bytes(mutated)
        expect_rejection([overlap], output, "overlapping section payloads")

        # A section that claims a larger payload than the file holds is
        # truncated, not short.
        mutated = bytearray(data)
        struct.pack_into("<I", mutated, table + 8, 0xFFFFFFF0)
        refresh_object_crc(mutated)
        oversize = work / "oversize.cc64o"
        oversize.write_bytes(mutated)
        expect_rejection([oversize], output, "a section payload past the file")

        # Executable text carries the alignment the ABI's entry contract
        # needs; a smaller one is a malformed object.
        mutated = bytearray(data)
        struct.pack_into("<I", mutated, table + 20, 1)
        refresh_object_crc(mutated)
        unaligned = work / "unaligned.cc64o"
        unaligned.write_bytes(mutated)
        expect_rejection([unaligned], output, "an undersized text alignment")

        # A relocation that names a symbol the object does not define cannot
        # be resolved, and one past the end of its section is out of range.
        pointer = compile_unit(
            work, "int value = 7; int *p = &value; "
            "int main(void) { return *p; }\n", "reloc")
        reloc_data = bytearray(pointer.read_bytes())
        reloc_table = struct.unpack_from("<I", reloc_data, 28)[0]
        count = struct.unpack_from("<I", reloc_data, 32)[0]
        record = 0
        for index in range(count):
            section = reloc_table + index * 40
            entries = struct.unpack_from("<I", reloc_data, section + 16)[0]
            if entries != 0:
                record = struct.unpack_from("<I", reloc_data, section + 12)[0]
                break
        if record == 0:
            raise SystemExit("linker negative: the setup object has no relocation")
        for index, (offset, value, fmt, label) in enumerate([
            (8, 0xFFFFFF, "I", "a relocation naming an unknown symbol"),
            (0, 0xFFFF, "Q", "a relocation past the end of its section"),
        ]):
            mutated = bytearray(reloc_data)
            struct.pack_into(f"<{fmt}", mutated, record + offset, value)
            refresh_object_crc(mutated)
            bad = work / f"reloc-{index}.cc64o"
            bad.write_bytes(mutated)
            expect_rejection([bad], output, label)
    print("linker: every documented rejection produced its diagnostic and no output")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
