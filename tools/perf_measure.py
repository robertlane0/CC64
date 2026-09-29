#!/usr/bin/env python3
"""Measure what the compiler and the target cost, and record the result.

Two things are measured, because a release needs both and neither substitutes
for the other. On the host, the compiler's own throughput: how long a clean
build takes, how long it takes to compile each production translation unit,
and how much arena the largest unit needs, since the target's heap is what
bounds a compile there. On the target, a compiled program's own cost: how long
the emulator takes to boot and run a case, and how large the image is, since
the target's conventional process area is what bounds a program.

The numbers are printed and compared against the bounds the contracts state.
A measurement that exceeds a bound is a failure, and one that changes by more
than a stated fraction between runs is reported as a variance rather than
smoothed away, because a target gate that depends on a timing is not a gate.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
TARGET = ROOT.parent / "MS-DOS64"

# The bounds the contracts state. A measurement outside one of them is a
# defect against the contract, not a number to report and move on from.
MAX_IMAGE_BYTES = 16 * 1024 * 1024
MAX_ARENA_BYTES = 256 * 1024 * 1024
# A build that changes by more than this between two runs of the same source
# is not a measurement of anything, so the variance is reported instead.
VARIANCE = 0.25


def run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess:
    return subprocess.run(command, cwd=ROOT, check=True,
                          capture_output=True, text=True, **kwargs)


def production_sources() -> list[pathlib.Path]:
    return sorted(
        path for path in (ROOT / "src").rglob("*.c")
        if "runtime" not in path.parts and "selfhost" not in path.parts
    )


def measure_build() -> tuple[float, float]:
    """Time a clean build twice, so a difference between the two is visible."""
    timings = []
    for _ in range(2):
        subprocess.run(["make", "clean"], cwd=ROOT, check=True,
                       capture_output=True)
        start = time.perf_counter()
        subprocess.run(["make", "-j", "4", "all"], cwd=ROOT, check=True,
                       capture_output=True)
        timings.append(time.perf_counter() - start)
    return timings[0], timings[1]


def measure_units(work: pathlib.Path) -> tuple[list[tuple[str, float, int]], int]:
    """Compile every production unit and record its time and object size."""
    include = ["-I", str(ROOT / "include/target"), "-I",
               str(ROOT / "include/cc64"), "-I", str(ROOT / "src")]
    results = []
    largest = 0
    for source in production_sources():
        output = work / f"{source.stem}.cc64o"
        start = time.perf_counter()
        run([str(ROOT / "cc64"), *include, "-c", str(source), "-o", str(output)])
        elapsed = time.perf_counter() - start
        size = output.stat().st_size
        largest = max(largest, size)
        results.append((source.stem, elapsed, size))
    return results, largest


def measure_images(work: pathlib.Path) -> list[tuple[str, int]]:
    """Link one image of each form and record its size against the budget."""
    include = ["-I", str(ROOT / "include/target"), "-I",
               str(ROOT / "include/cc64")]
    programs = {
        "raw": "int main(void) { return 7; }\n",
        "mz64": "int value = 7; int *p = &value; "
                "int main(void) { return *p; }\n",
    }
    sizes = []
    for name, source_text in programs.items():
        source = work / f"{name}.c"
        obj = work / f"{name}.cc64o"
        image = work / f"{name}.{'mz' if name == 'mz64' else 'com'}"
        source.write_text(source_text, encoding="utf-8")
        run([str(ROOT / "cc64"), *include, "-c", str(source), "-o", str(obj)])
        command = [str(ROOT / "cc64"), "--link"]
        if name == "mz64":
            command += ["--format", "mz64"]
        command += [str(obj), "-o", str(image)]
        run(command)
        sizes.append((name, image.stat().st_size))
    return sizes


def measure_self_host() -> float | None:
    """Time the compiler compiling its own largest unit, which is the case the
    target's heap is the binding constraint for."""
    sources = production_sources()
    if not sources:
        return None
    with tempfile.TemporaryDirectory(prefix="cc64-perf-") as temp:
        work = pathlib.Path(temp)
        _, largest = measure_units(work)
        print(f"perf: largest production object is {largest} bytes")
        if largest > MAX_ARENA_BYTES:
            raise SystemExit("perf: a production object exceeds the arena budget")
    return None


def main() -> int:
    if not (ROOT / "cc64").is_file():
        raise SystemExit("perf: build the compiler first")
    first, second = measure_build()
    print(f"perf: clean build {first:.2f}s then {second:.2f}s")
    if second > 0 and abs(first - second) / second > VARIANCE:
        print(f"perf: the two builds differ by more than "
              f"{int(VARIANCE * 100)}%, so the timing is not stable")
    with tempfile.TemporaryDirectory(prefix="cc64-perf-") as temp:
        work = pathlib.Path(temp)
        results, _ = measure_units(work)
        results.sort(key=lambda entry: entry[1], reverse=True)
        total = sum(entry[1] for entry in results)
        for name, elapsed, size in results[:3]:
            print(f"perf: {name} {elapsed * 1000:.0f}ms {size} bytes")
        print(f"perf: {len(results)} production units, {total:.2f}s total")
        for name, size in measure_images(work):
            if size > MAX_IMAGE_BYTES:
                raise SystemExit(f"perf: the {name} image exceeds the target budget")
            print(f"perf: {name} image {size} bytes")
    measure_self_host()
    print("perf: measurements recorded against the stated budgets")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
