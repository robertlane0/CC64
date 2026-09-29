#!/usr/bin/env python3
"""Response-file expansion, driven against the host-built compiler.

The target's process contract gives a child 127 bytes of command tail, 126 of
them usable, so a link of more than about two dozen objects cannot be written
as one line. An argument of the form @NAME is replaced by the words inside
NAME. These cases pin the behaviour a build depends on: that a response file
produces the same image as the same arguments written out, that a word may
name a further response file, that a quoted word keeps its spaces, that
arguments may be mixed with plain ones, and that each of the three ways a
response file can be wrong is reported rather than acted on.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
CC64 = ROOT / "cc64"
RUNTIME = ROOT / "src" / "runtime"
INCLUDES = ["-I", str(ROOT / "include" / "target"), "-I", str(ROOT / "include" / "cc64"),
            "-I", str(ROOT / "src")]

PROGRAM = (
    "#include <stdio.h>\n"
    "int main(void) { printf(\"R\"); return 0; }\n"
)


def run(command: list[str], cwd: pathlib.Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, capture_output=True, text=True, check=False,
                          cwd=cwd)


def build_objects(work: pathlib.Path) -> tuple[pathlib.Path, list[pathlib.Path]]:
    """A program object plus every target library unit, linked into one image."""
    source = work / "R.C"
    source.write_text(PROGRAM, encoding="utf-8")
    program = work / "R.cc64o"
    result = run([str(CC64), *INCLUDES, "-c", str(source), "-o", str(program)], work)
    if result.returncode != 0:
        raise SystemExit(f"program rejected: {result.stdout}{result.stderr}")
    objects = [program]
    for index, library_source in enumerate(sorted(RUNTIME.glob("target_*.c"))):
        output = work / f"L{index}.cc64o"
        result = run([str(CC64), *INCLUDES, "-c", str(library_source),
                      "-o", str(output)], work)
        if result.returncode != 0:
            raise SystemExit(f"library unit rejected: "
                             f"{library_source.name}: {result.stdout}{result.stderr}")
        objects.append(output)
    return program, objects


def link(work: pathlib.Path, objects: list[pathlib.Path], output: pathlib.Path,
         extra: list[str]) -> subprocess.CompletedProcess[str]:
    return run([str(CC64), "--link", "--format", "mz64", *extra,
                *[str(item) for item in objects], "-o", str(output)], work)


def main() -> int:
    if not CC64.is_file():
        print("response files: skipped (compiler not built)")
        return 0
    with tempfile.TemporaryDirectory(prefix="cc64-response-") as temp:
        work = pathlib.Path(temp)
        program, objects = build_objects(work)

        # The reference image: every argument written out.
        reference = work / "REF.COM"
        result = link(work, objects, reference, [])
        if result.returncode != 0:
            raise SystemExit(f"link failed: {result.stdout}{result.stderr}")

        def same(label: str, command: list[str]) -> None:
            image = work / f"{label}.COM"
            result = run(command, work)
            if result.returncode != 0:
                raise SystemExit(f"{label}: {result.stdout}{result.stderr}")
            if image.read_bytes() != reference.read_bytes():
                raise SystemExit(f"{label}: image differs from the reference link")

        # One argument per line, the form the self-host stage uses.
        listed = work / "link.rsp"
        listed.write_text("--link\n--format\nmz64\n" +
                          "\n".join(item.name for item in objects) + "\n",
                          encoding="utf-8")
        same("listed", [str(CC64), "@link.rsp", "-o", str(work / "listed.COM")])

        # A response file may name another, to a stated depth.
        (work / "outer.rsp").write_text("@link.rsp\n", encoding="utf-8")
        same("nested", [str(CC64), "@outer.rsp", "-o", str(work / "nested.COM")])

        # A quoted word keeps a space in it, which an unquoted word cannot.
        spaced = work / "spaced name.cc64o"
        program_bytes = program.read_bytes()
        spaced.write_bytes(program_bytes)
        (work / "spaced.rsp").write_text(
            '--link --format mz64 "' + spaced.name + '"' +
            "".join(f' "{item.name}"' for item in objects[1:]) + "\n",
            encoding="utf-8")
        same("quoted", [str(CC64), "@spaced.rsp", "-o", str(work / "quoted.COM")])

        # A response file and plain arguments combine, in either order.
        same("mixed", [str(CC64), "@link.rsp", "-o", str(work / "mixed.COM")])
        (work / "rest.rsp").write_text(
            " ".join(item.name for item in objects) + "\n", encoding="utf-8")
        same("trailing", [str(CC64), "--link", "--format", "mz64", "@rest.rsp",
                          "-o", str(work / "trailing.COM")])

        # The three ways a response file can be wrong are all reported.
        expected = [
            ("missing", "@absent.rsp", "CC1002"),
            ("self", "@loop.rsp", "CC1001"),
            ("mutual", "@left.rsp", "CC1001"),
        ]
        (work / "loop.rsp").write_text("@loop.rsp\n", encoding="utf-8")
        (work / "left.rsp").write_text("@right.rsp\n", encoding="utf-8")
        (work / "right.rsp").write_text("@left.rsp\n", encoding="utf-8")
        for label, argument, identifier in expected:
            result = run([str(CC64), argument, "-o", str(work / f"{label}.COM")], work)
            if result.returncode == 0:
                raise SystemExit(f"{label}: a bad response file was accepted")
            if identifier not in (result.stdout + result.stderr):
                raise SystemExit(f"{label}: expected {identifier}, got "
                                 f"{result.stdout.strip()}{result.stderr.strip()}")

        # An argument that is only '@' is an argument, not a response file, so
        # the option after it is still seen.
        result = run([str(CC64), "@", "--version"], work)
        if result.returncode != 0 or "cc64 " not in result.stdout:
            raise SystemExit("a bare '@' was not treated as an argument: "
                             f"{result.stdout.strip()}{result.stderr.strip()}")

    print(f"response files: {6} image comparisons and {len(expected)} "
          f"rejections agree with the documented behaviour")
    return 0


if __name__ == "__main__":
    sys.exit(main())
