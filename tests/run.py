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
        source = pathlib.Path(temp) / "hello.c"
        output = pathlib.Path(temp) / "hello.cc64o"
        source.write_text("int main(void) { return 7; }\n", encoding="utf-8")
        run([str(ROOT / "cc64"), "--target", "x86_64-pc-dos64", "-c", str(source), "-o", str(output)])
        bad = pathlib.Path(temp) / "bad.c"
        bad.write_text("int main(void) { return 0; }\n", encoding="utf-8")
        result = subprocess.run(
            [str(ROOT / "cc64"), "--target", "unknown-target", "-c", str(bad), "-o", str(pathlib.Path(temp) / "bad.o")],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode == 0 or "CC0012" not in result.stderr:
            raise SystemExit("negative target diagnostic did not match")
    run(["python3", str(ROOT / "tests/audit.py")])
    print("integration: driver groups passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        print(f"test failed with status {error.returncode}", file=sys.stderr)
        raise
