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
        second_image = directory / "hello2.com"
        run([str(ROOT / "cc64"), "--link", str(object_file), "-o", str(second_image)])
        if image_file.read_bytes() != second_image.read_bytes():
            raise SystemExit("raw image output is not deterministic")
        if not image_file.is_file() or image_file.read_bytes()[:3] != b"\x48\x83\xec":
            raise SystemExit("raw image output is invalid")

        mz_source = directory / "mz.c"
        mz_object = directory / "mz.cc64o"
        mz_image = directory / "mz.bin"
        mz_source.write_text(
            "int value = 7; int *pointer = &value; "
            "int main(void) { return *pointer; }\n",
            encoding="utf-8",
        )
        run([str(ROOT / "cc64"), "-c", str(mz_source), "-o", str(mz_object)])
        run([str(ROOT / "cc64"), "--link", "--format", "mz64", str(mz_object),
             "-o", str(mz_image)])
        second_mz = directory / "mz2.bin"
        run([str(ROOT / "cc64"), "--link", "--format", "mz64", str(mz_object),
             "-o", str(second_mz)])
        if mz_image.read_bytes() != second_mz.read_bytes():
            raise SystemExit("MZ64 output is not deterministic")
        run(["python3", str(ROOT / "tools/inspect_image.py"), str(mz_image)])
        malformed = directory / "malformed.mz"
        malformed.write_bytes(mz_image.read_bytes())
        malformed_bytes = bytearray(malformed.read_bytes())
        malformed_bytes[28] = 0
        malformed.write_bytes(malformed_bytes)
        rejected_image = subprocess.run(
            ["python3", str(ROOT / "tools/inspect_image.py"), str(malformed)],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        if rejected_image.returncode == 0:
            raise SystemExit("malformed MZ64 header was accepted")

        second_object = directory / "second.cc64o"
        second_image = directory / "second.com"
        second_source = directory / "second.c"
        second_source.write_text(
            "int other = 4; int main(void) { return other; }\n",
            encoding="utf-8",
        )
        run([str(ROOT / "cc64"), "-c", str(second_source), "-o", str(second_object)])
        run([str(ROOT / "cc64"), "--link", str(second_object), "-o", str(second_image)])
        if not second_image.is_file():
            raise SystemExit("single-object link failed")

        runtime_a = directory / "runtime-a.c"
        runtime_b = directory / "runtime-b.c"
        runtime_a.write_text(
            "void cc64_exit(int); int other(void); "
            "int main(void) { cc64_exit(3); return other(); }\n",
            encoding="utf-8",
        )
        runtime_b.write_text(
            "void cc64_exit(int); int other(void) { cc64_exit(4); return 0; }\n",
            encoding="utf-8")
        runtime_a_object = directory / "runtime-a.cc64o"
        runtime_b_object = directory / "runtime-b.cc64o"
        run([str(ROOT / "cc64"), "-c", str(runtime_a), "-o", str(runtime_a_object)])
        run([str(ROOT / "cc64"), "-c", str(runtime_b), "-o", str(runtime_b_object)])
        run([str(ROOT / "cc64"), "--link", str(runtime_a_object),
             str(runtime_b_object), "-o", str(directory / "runtime.com")])

        table_source = directory / "table.c"
        table_object = directory / "table.cc64o"
        table_source.write_text(
            "static const char *const names[] = {\"alpha\", \"beta\", \"gamma\"};\n"
            "int main(void) { return names[2][0] == 'g' ? 7 : 1; }\n",
            encoding="utf-8",
        )
        run([str(ROOT / "cc64"), "-c", str(table_source), "-o", str(table_object)])
        run(["python3", str(ROOT / "tools/inspect_object.py"), str(table_object)])
        run([str(ROOT / "cc64"), "--link", "--format", "mz64", str(table_object),
             "-o", str(directory / "table.mz")])

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
