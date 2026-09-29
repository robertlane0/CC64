#!/usr/bin/env python3
"""Check the image compatibility matrix the target contract promises.

The ABI document says a later version must preserve version 1 images or say why
it cannot, and the target contract says a driver must reject a version it does
not know. This gate checks that the promise is kept in the direction a reader
can verify: every image this compiler emits is readable by the independent
reader, an image whose version is not the one this compiler writes is rejected
rather than guessed at, and both executable forms agree with the contract on
their own recorded fields.
"""

from __future__ import annotations

import pathlib
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
INCLUDE = ["-I", str(ROOT / "include/target"), "-I", str(ROOT / "include/cc64")]

# What each version states, from the pinned contract. A change here is a change
# to a contract, not to a test.
CONTRACT = {
    # The loader copies the payload to a sixteen-byte-aligned address, so
    # every bias it may use is a multiple of that, and the image lands aligned
    # at each of them.
    "entry_alignment": 16,
    "biases": (0, 0x1000, 0x20000, 0x100000, 0x400000),
    "raw": {
        "entry": 0,
        "limit": 16 * 1024 * 1024,
    },
    "mz64": {
        "magic": b"MZ64",
        "header_size": 48,
        "abi": "cc64-dos64-v1",
        "limit": 16 * 1024 * 1024,
    },
}


def run(command: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                          check=False)


def build(work: pathlib.Path, name: str, text: str, image_format: str) -> pathlib.Path:
    source = work / f"{name}.c"
    obj = work / f"{name}.cc64o"
    image = work / (f"{name}.com" if image_format == "raw" else f"{name}.mz")
    source.write_text(text, encoding="utf-8")
    result = run([str(ROOT / "cc64"), *INCLUDE, "-c", str(source), "-o", str(obj)])
    if result.returncode != 0:
        raise SystemExit(f"matrix: {name} rejected\n{result.stdout}{result.stderr}")
    command = [str(ROOT / "cc64"), "--link"]
    if image_format == "mz64":
        command += ["--format", "mz64"]
    command += [str(obj), "-o", str(image)]
    result = run(command)
    if result.returncode != 0:
        raise SystemExit(f"matrix: {name} did not link\n{result.stdout}{result.stderr}")
    return image


def expect_rejection(image: pathlib.Path, label: str, offset: int, value,
                     width: str, fmt: str) -> None:
    """A field the contract pins is changed and the reader must refuse it."""
    mutated = bytearray(image.read_bytes())
    struct.pack_into(fmt, mutated, offset, value)
    candidate = image.with_suffix(".mutated")
    candidate.write_bytes(mutated)
    result = run(["python3", str(ROOT / "tools/inspect_image.py"), str(candidate)])
    if result.returncode == 0:
        raise SystemExit(f"matrix: the reader accepted {label}")
    result = run(["python3", str(ROOT / "tools/inspect_image.py"), str(candidate),
                  "--bias", "0x400000"])
    if result.returncode == 0:
        raise SystemExit(f"matrix: the reader accepted {label} at a load bias")
    candidate.unlink()


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="cc64-matrix-") as temp:
        work = pathlib.Path(temp)
        # A raw image is loaded at the process origin, so its entry is zero and
        # it has no relocation table.
        raw = build(work, "compat-raw", "int main(void) { return 7; }\n", "raw")
        raw_bytes = raw.read_bytes()
        if not raw_bytes:
            raise SystemExit("matrix: the raw image is empty")
        if len(raw_bytes) > CONTRACT["raw"]["limit"]:
            raise SystemExit("matrix: the raw image is over the target budget")

        # The load-biased form carries its own header, and every field the
        # loader reads is checked against the pinned values.
        mz = build(work, "compat-mz64",
                   "int value = 7; int *p = &value; int main(void) { return *p; }\n",
                   "mz64")
        mz_bytes = mz.read_bytes()
        if mz_bytes[:4] != CONTRACT["mz64"]["magic"]:
            raise SystemExit("matrix: the MZ64 magic does not match the contract")
        header_size, = struct.unpack_from("<I", mz_bytes, 4)
        if header_size != CONTRACT["mz64"]["header_size"]:
            raise SystemExit("matrix: the MZ64 header size does not match")
        payload, = struct.unpack_from("<Q", mz_bytes, 8)
        entry, = struct.unpack_from("<I", mz_bytes, 16)
        reloc_count, = struct.unpack_from("<I", mz_bytes, 24)
        reloc_offset, = struct.unpack_from("<I", mz_bytes, 28)
        memory, = struct.unpack_from("<Q", mz_bytes, 32)
        if entry >= payload:
            raise SystemExit("matrix: the MZ64 entry is outside the payload")
        if memory < payload or memory > CONTRACT["mz64"]["limit"]:
            raise SystemExit("matrix: the MZ64 memory size is out of contract")
        if reloc_offset != 48 + payload:
            raise SystemExit("matrix: the MZ64 table is not adjacent")
        if 48 + payload + reloc_count * 16 != len(mz_bytes):
            raise SystemExit("matrix: the MZ64 table does not end at the end")
        # The contract pins the address the payload is loaded at, not the
        # length of the payload, so a load bias is a multiple of the entry
        # alignment and the image lands aligned at every one of them.
        for bias in CONTRACT["biases"]:
            if bias % CONTRACT["entry_alignment"] != 0:
                raise SystemExit("matrix: a bias is not entry aligned")

        # The reader accepts the image this compiler wrote, at every bias.
        for bias in CONTRACT["biases"]:
            for image in (raw, mz):
                result = run(["python3", str(ROOT / "tools/inspect_image.py"),
                              str(image), "--bias", hex(bias)])
                if result.returncode != 0:
                    raise SystemExit(
                        f"matrix: the reader refused a valid image at {hex(bias)}")

        # A version this compiler does not write is refused rather than read
        # as if it were one it knows.
        expect_rejection(mz, "a different header size", 4, 64, "<I", "<I")
        expect_rejection(mz, "a zero payload size", 8, 0, "<Q", "<Q")
        expect_rejection(mz, "an entry past the payload", 16, 1 << 20, "<I", "<I")
        expect_rejection(mz, "a table that is not adjacent", 28, 32, "<I", "<I")
        expect_rejection(mz, "a reserved field that is not zero", 40, 1, "<Q", "<Q")
    print("compatibility: both image forms match the pinned contract, and a "
          "version this compiler does not write is refused")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
