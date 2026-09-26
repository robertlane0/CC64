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

        arrow_source = directory / "arrow.c"
        arrow_object = directory / "arrow.cc64o"
        arrow_source.write_text(
            "struct S { char buffer[8]; int value; };\n"
            "int main(void) {\n"
            "  struct S item;\n"
            "  item.value = 65;\n"
            "  item.buffer[0] = 'z';\n"
            "  struct S *p = &item;\n"
            "  const char *first = p->buffer;\n"
            "  if (first[0] != 'z') return 1;\n"
            "  if (first != p->buffer) return 2;\n"
            "  if (p->value != 65) return 3;\n"
            "  return 9;\n"
            "}\n",
            encoding="utf-8",
        )
        run([str(ROOT / "cc64"), "-c", str(arrow_source), "-o", str(arrow_object)])
        run([str(ROOT / "cc64"), "--link", str(arrow_object),
             "-o", str(directory / "arrow.com")])

        global_text = directory / "global-text.c"
        global_object = directory / "global-text.cc64o"
        global_text.write_text(
            "char message[] = \"hello\";\n"
            "char sized[6] = \"hello\";\n"
            "int main(void) { return message[4] == 'o' && sized[5] == '\\0' ? 9 : 1; }\n",
            encoding="utf-8",
        )
        run([str(ROOT / "cc64"), "-c", str(global_text), "-o", str(global_object)])
        run([str(ROOT / "cc64"), "--link", str(global_object),
             "-o", str(directory / "global-text.com")])

        constant_source = directory / "constant-size.c"
        constant_object = directory / "constant-size.cc64o"
        constant_source.write_text(
            "#define N 4\n"
            "int main(void) {\n"
            "  int a[N];\n"
            "  int b[2 + 1];\n"
            "  int c[sizeof(long)];\n"
            "  a[3] = 1; b[2] = 2; c[7] = 3;\n"
            "  return a[3] + b[2] + c[7];\n"
            "}\n",
            encoding="utf-8",
        )
        run([str(ROOT / "cc64"), "-c", str(constant_source), "-o", str(constant_object)])
        run([str(ROOT / "cc64"), "--link", str(constant_object),
             "-o", str(directory / "constant-size.com")])

        attached = directory / "attached.c"
        attached.write_text("#include \"attached.h\"\nint main(void) { return VALUE; }\n",
                            encoding="utf-8")
        (directory / "attached.h").write_text("#define VALUE 0\n", encoding="utf-8")
        attached_object = directory / "attached.cc64o"
        run([str(ROOT / "cc64"), "-I" + str(directory), "-c", str(attached),
             "-o", str(attached_object)])
        run([str(ROOT / "cc64"), "-D", "EXTRA=1", "-c", str(attached),
             "-o", str(directory / "attached2.cc64o")])

        multi_source = directory / "multi.c"
        multi_object = directory / "multi.cc64o"
        multi_source.write_text(
            "int pick(int a) { unsigned long left = 0UL, right = 0UL;\n"
            "  unsigned long *p = &right; *p = (unsigned long)a; return (int)left + (int)right; }\n"
            "int main(void) { return pick(5); }\n",
            encoding="utf-8",
        )
        run([str(ROOT / "cc64"), "-c", str(multi_source), "-o", str(multi_object)])
        run([str(ROOT / "cc64"), "--link", str(multi_object),
             "-o", str(directory / "multi.com")])
        unresolved = subprocess.run(
            ["python3", str(ROOT / "tools/missing_symbols.py"), str(multi_object)],
            cwd=ROOT, capture_output=True, text=True, check=False,
        )
        if unresolved.returncode != 0:
            raise SystemExit(
                "multi-declarator declaration produced an external reference:\n"
                + unresolved.stdout)

        conditional_source = directory / "conditional.c"
        conditional_object = directory / "conditional.cc64o"
        # The conditional operator converts its operands, so a conditional over
        # two string literals, or over a literal and a pointer, is ordinary C
        # rather than a rejected pair of branches.
        conditional_source.write_text(
            "static const char *pick(int flag) {\n"
            "  return flag ? \"yes\" : \"no\";\n"
            "}\n"
            "int main(void) {\n"
            "  const char *a = 1 ? \"alpha\" : \"beta\";\n"
            "  char local[4] = \"xy\";\n"
            "  const char *b = 0 ? local : \"gamma\";\n"
            "  if (a[0] != 'a') return 1;\n"
            "  if (b[0] != 'g') return 2;\n"
            "  if (pick(1)[0] != 'y') return 3;\n"
            "  return 9;\n"
            "}\n",
            encoding="utf-8",
        )
        run([str(ROOT / "cc64"), "-c", str(conditional_source),
             "-o", str(conditional_object)])
        run([str(ROOT / "cc64"), "--link", str(conditional_object),
             "-o", str(directory / "conditional.com")])

        # Several inputs produce one object each, named after the input, and a
        # single -o cannot stand in for them.
        many_a = directory / "many-a.c"
        many_b = directory / "many-b.c"
        many_a.write_text("int first(void) { return 4; }\n", encoding="utf-8")
        many_b.write_text("int first(void);\nint main(void) { return first(); }\n",
                          encoding="utf-8")
        run([str(ROOT / "cc64"), "-c", str(many_a), str(many_b)])
        for produced in (directory / "many-a.cc64o", directory / "many-b.cc64o"):
            if not produced.is_file():
                raise SystemExit("multi-input compile did not produce every object")
        linked = directory / "many.com"
        run([str(ROOT / "cc64"), "--link", str(directory / "many-a.cc64o"),
             str(directory / "many-b.cc64o"), "-o", str(linked)])
        rejected_many = subprocess.run(
            [str(ROOT / "cc64"), "-c", str(many_a), str(many_b),
             "-o", str(directory / "many.o")],
            cwd=ROOT, capture_output=True, text=True, check=False)
        if rejected_many.returncode == 0 or "CC0013" not in rejected_many.stderr:
            raise SystemExit("multi-input -o was not rejected")

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
