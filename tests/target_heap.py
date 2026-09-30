#!/usr/bin/env python3
"""Measure the target heap with a compiler-produced program.

The target maps a fixed identity range and hands the rest to its MCB chain, so
the heap size is not a constant the compiler can read. A program that has to
fit in the compiler's own working set needs the figure measured rather than
assumed, and this is the measurement: `tests/target_heap.c` claims the largest
block the target allocator will give it and prints the size.

Two things are checked. The program must measure a heap at least as large as
the one the target contract now states, so a target that lost the extension
fails here rather than at some later, less obvious point. And the figure is
printed, so a change in it is visible in the release evidence even when it is
still within the bound.
"""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
TARGET = ROOT.parent / "MS-DOS64"
sys.path.insert(0, str(ROOT / "tools"))
import target_revision  # noqa: E402  (path is set above)

# The heap the edit branch of the target provides, in mebibytes. The chain runs
# from 2 MiB to 14 MiB, which is 12 MiB; the program measures what a process
# can actually claim, which is the chain less the process block and the
# allocator's own bookkeeping, so the bound is the nominal size less a
# mebibyte. The pinned reference provides 6 MiB and measures 5, so this bound
# also fails against an unmodified target, which is what a dependency on the
# edit branch has to do.
MINIMUM_MEBIBYTES = 11



def run(command: list[str], cwd: pathlib.Path) -> None:
    subprocess.run(command, cwd=cwd, check=True, timeout=300,
                   stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def library_objects(work: pathlib.Path) -> list[str]:
    objects = []
    for index, source in enumerate(sorted((ROOT / "src" / "runtime").glob("target_*.c"))):
        output = work / f"L{index}.cc64o"
        run([str(ROOT / "cc64"), "-I", str(ROOT / "include" / "target"),
             "-I", str(ROOT / "include" / "cc64"), "-I", str(ROOT / "src"),
             "-c", str(source), "-o", str(output)], ROOT)
        objects.append(str(output))
    return objects


def main() -> int:
    if not TARGET.is_dir():
        print("target heap: skipped (target checkout unavailable)")
        return 0
    if not target_revision.require_emulator(
            "qemu-system-x86_64",
            "target heap: skipped (QEMU unavailable)"):
        return 0
    target_revision.check(TARGET)
    with tempfile.TemporaryDirectory(prefix="cc64-heap-") as temp:
        work = pathlib.Path(temp)
        source = work / "HEAP.C"
        source.write_text((ROOT / "tests" / "target_heap.c").read_text(encoding="utf-8"),
                          encoding="utf-8")
        obj = work / "HEAP.cc64o"
        run([str(ROOT / "cc64"), "-I", str(ROOT / "include" / "target"),
             "-I", str(ROOT / "include" / "cc64"), "-c", str(source), "-o", str(obj)],
            ROOT)
        image = work / "HEAP.COM"
        run([str(ROOT / "cc64"), "--link", "--format", "mz64", str(obj),
             *library_objects(work), "-o", str(image)], ROOT)
        disk = work / "heap.img"
        run(["make", "clean", "lean"], TARGET)
        disk.write_bytes((TARGET / "build" / "dos64-lean.img").read_bytes())
        run(["python3", str(ROOT / "tests" / "embed_fat12.py"), str(disk),
             str(image), "HEAP"], ROOT)
        transcript = work / "heap.log"
        with transcript.open("w", encoding="utf-8") as stream:
            process = subprocess.Popen(
                ["qemu-system-x86_64", "-drive", f"file={disk},format=raw",
                 "-serial", "stdio", "-display", "none"],
                stdin=subprocess.PIPE, stdout=stream, stderr=subprocess.STDOUT,
                text=True)
            process.stdin.write("HEAP\n")
            process.stdin.close()
            deadline = time.monotonic() + 120.0
            found = ""
            while time.monotonic() < deadline:
                text = transcript.read_text(encoding="utf-8", errors="replace")
                match = re.search(r"heap: (\d+) MiB usable", text)
                if match is not None:
                    found = match.group(1)
                    break
                if process.poll() is not None:
                    break
                time.sleep(0.1)
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
    if not found:
        raise SystemExit("target heap: the program printed no measurement; see "
                         f"{transcript}")
    if int(found) < MINIMUM_MEBIBYTES:
        raise SystemExit(f"target heap: measured {found} MiB, the contract needs "
                         f"at least {MINIMUM_MEBIBYTES} MiB")
    print(f"target heap: {found} MiB claimable, at least "
          f"{MINIMUM_MEBIBYTES} MiB required")
    return 0


if __name__ == "__main__":
    sys.exit(main())
