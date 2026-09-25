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
        object_file = directory / "hello.cc64o"
        object_file2 = directory / "hello2.cc64o"
        image_file = directory / "hello.com"
        source.write_text(
            "#define VALUE 4\n"
            "int add(int a, int b) { return a + b; }\n"
            "int main(void) { return add(VALUE, 3); }\n",
            encoding="utf-8",
        )
        run([str(ROOT / "cc64"), "-E", "-P", str(source), "-o", str(first)])
        run([str(ROOT / "cc64"), "-E", "-P", str(source), "-o", str(second)])
        if not first.is_file() or first.read_bytes() != second.read_bytes():
            raise SystemExit("preprocessor output is missing or nondeterministic")
        if b"value" not in first.read_bytes() and b"add" not in first.read_bytes():
            raise SystemExit("preprocessor output lacks expanded tokens")

        run([str(ROOT / "cc64"), "-c", str(source), "-o", str(object_file)])
        run([str(ROOT / "cc64"), "-c", str(source), "-o", str(object_file2)])
        if not object_file.is_file() or object_file.read_bytes() != object_file2.read_bytes():
            raise SystemExit("object output is missing or nondeterministic")
        run(["python3", str(ROOT / "tools/inspect_object.py"), str(object_file)])
        run([str(ROOT / "cc64"), "--link", str(object_file), "-o", str(image_file)])
        if not image_file.is_file() or image_file.read_bytes()[:2] != b"\x31\xff":
            raise SystemExit("raw image output is invalid")

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

        bad_semantic = directory / "bad-semantic.c"
        bad_semantic.write_text("int main(void) { return missing; }\n", encoding="utf-8")
        result = subprocess.run(
            [str(ROOT / "cc64"), "-c", str(bad_semantic), "-o", str(directory / "bad.o2")],
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode == 0 or "CC2075" not in result.stderr:
            raise SystemExit("semantic diagnostic did not match")

    run(["python3", str(ROOT / "tests/audit.py")])
    print("integration: driver, object, and semantic groups passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        print(f"test failed with status {error.returncode}", file=sys.stderr)
        raise
