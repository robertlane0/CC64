#!/usr/bin/env python3
"""Run project-owned CC64 test groups."""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def run(command: list[str], **kwargs: object) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, cwd=ROOT, check=True, **kwargs)


def main() -> int:
    run(["make", "all"])
    run(["make", "test-unit"])
    with tempfile.TemporaryDirectory(prefix="cc64-test-") as temp:
        directory = pathlib.Path(temp)
        source = directory / "hello.c"
        first = directory / "hello.i"
        second = directory / "hello2.i"
        source.write_text(
            "#define VALUE 4\nint value = VALUE;\n", encoding="utf-8"
        )
        run([str(ROOT / "cc64"), "-E", "-P", str(source), "-o", str(first)])
        run([str(ROOT / "cc64"), "-E", "-P", str(source), "-o", str(second)])
        if not first.is_file() or first.read_bytes() != second.read_bytes():
            raise SystemExit("preprocessor output is missing or nondeterministic")
        if b"value" not in first.read_bytes() or b"4" not in first.read_bytes():
            raise SystemExit("preprocessor output lacks expanded tokens")

        bad = directory / "bad.c"
        bad_output = directory / "bad.i"
        bad.write_text('"unterminated\n', encoding="utf-8")
        result = subprocess.run(
            [str(ROOT / "cc64"), "-E", str(bad), "-o", str(bad_output)],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode == 0 or bad_output.exists():
            raise SystemExit("failed preprocessing left an output file")

        rejected = subprocess.run(
            [str(ROOT / "cc64"), "--target", "unknown-target", "-c", str(source),
             "-o", str(directory / "bad.o")],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        if rejected.returncode == 0 or "CC0012" not in rejected.stderr:
            raise SystemExit("negative target diagnostic did not match")

        not_ready = subprocess.run(
            [str(ROOT / "cc64"), "-c", str(source), "-o", str(directory / "x.o")],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        if not_ready.returncode == 0 or "CC1001" not in not_ready.stderr:
            raise SystemExit("M1 compile boundary diagnostic did not match")

    run(["python3", str(ROOT / "tests/audit.py")])
    print("integration: driver groups passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        print(f"test failed with status {error.returncode}", file=sys.stderr)
        raise
