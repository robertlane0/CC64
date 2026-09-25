#!/usr/bin/env python3
"""Run compiler-produced images in the pinned DOS64 target."""

from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
TARGET = ROOT.parent / "MS-DOS64"
TARGET_REVISION = "13c3cedb05ad75592c17bf2006ba8617c8761a38"
VOLUME_ARGUMENTS = [
    "--vol-lba", "512", "--vol-sectors", "2880", "--sector-size", "512",
    "--kernel-lba", "16", "--kernel-sectors", "256",
]


def run(command: list[str], cwd: pathlib.Path, timeout: int = 180) -> None:
    subprocess.run(command, cwd=cwd, check=True, timeout=timeout,
                   stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def check_target() -> None:
    if not TARGET.is_dir():
        return
    override = os.environ.get("CC64_TARGET_REVISION")
    if override is not None and os.environ.get("CC64_REQUIRE_EMULATORS") == "1":
        raise SystemExit("strict release mode does not allow a target revision override")
    expected = override or TARGET_REVISION
    result = subprocess.run(["git", "-C", str(TARGET), "rev-parse", "HEAD"],
                            capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise SystemExit("target checkout has no readable revision")
    actual = result.stdout.strip()
    if actual != expected:
        raise SystemExit(f"target revision {actual} does not match pinned {expected}")
    dirty = subprocess.run(["git", "-C", str(TARGET), "diff", "--quiet", "HEAD", "--"],
                           check=False)
    if dirty.returncode != 0:
        raise SystemExit("target checkout has tracked source changes")


def check_volume(image: pathlib.Path, label: str) -> None:
    result = subprocess.run(
        ["python3", str(TARGET / "tools/check_volume_clean.py"),
         *VOLUME_ARGUMENTS, str(image)],
        cwd=TARGET, capture_output=True, text=True, check=False,
    )
    if result.returncode != 0:
        detail = (result.stdout + result.stderr).strip()
        raise SystemExit(f"target volume cleanliness check failed ({label}): {detail}")


def stop_process(process: subprocess.Popen) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def execute(qemu: str, disk: pathlib.Path, name: str, command: str,
            expected: int) -> str:
    transcript = disk.with_suffix(".log")
    process = None
    with transcript.open("w", encoding="utf-8") as stream:
        process = subprocess.Popen(
            [qemu, "-drive", f"file={disk},format=raw", "-serial", "stdio",
             "-display", "none"],
            stdin=subprocess.PIPE, stdout=stream, stderr=subprocess.STDOUT,
            text=True,
        )
        process.stdin.write(command + "\n")
        process.stdin.close()
        marker = f"Exit {expected}"
        deadline = time.monotonic() + 15.0
        complete = False
        while time.monotonic() < deadline:
            text = transcript.read_text(encoding="utf-8", errors="replace")
            if marker in text and "A> " in text[text.index(marker) + len(marker):]:
                complete = True
                break
            if process.poll() is not None:
                break
            time.sleep(0.05)
        stop_process(process)
    text = transcript.read_text(encoding="utf-8", errors="replace")
    if not complete:
        if process.returncode not in (0, -15):
            raise SystemExit(f"{name}: QEMU exited with status {process.returncode}")
        raise SystemExit(f"{name}: QEMU timed out before a complete target result")
    return text


def target_library_objects(work: pathlib.Path) -> list[str]:
    """Compile the target C library with CC64 for one linked case."""
    sources = sorted((ROOT / "src" / "runtime").glob("target_*.c"))
    objects = []
    for index, source in enumerate(sources):
        output = work / f"target-lib-{index}.cc64o"
        run([str(ROOT / "cc64"), "-I", str(ROOT / "include" / "target"),
             "-I", str(ROOT / "include" / "cc64"), "-I", str(ROOT / "src"),
             "-c", str(source), "-o", str(output)], ROOT)
        objects.append(str(output))
    return objects


LIBRARY_PROGRAM = (
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "#include <string.h>\n"
    "int cc64_write(int, const void *, unsigned long);\n"
    "int cc64_putc(int);\n"
    "int main(void) {\n"
    "  cc64_write(1, \"A\", 1);\n"
    "  cc64_putc(48 + (int)strlen(\"abcd\"));\n"
    "  cc64_write(1, \"B\", 1);\n"
    "  char *copy = (char *)malloc(16);\n"
    "  if (copy == 0) return 1;\n"
    "  copy[0] = 'Z'; copy[1] = 0;\n"
    "  cc64_write(1, copy, 1);\n"
    "  if (strcmp(copy, \"Z\") != 0) return 2;\n"
    "  if (strlen(\"xy\") != 2) return 3;\n"
    "  memset(copy, 'W', 4);\n"
    "  if (copy[0] != 'W') return 4;\n"
    "  cc64_write(1, \"C\", 1);\n"
    "  if (fputs(\"D\", stdout) != 0) return 5;\n"
    "  free(copy);\n"
    "  cc64_write(1, \"E\", 1);\n"
    "  return 9;\n"
    "}\n"
)


def compile_and_link(work: pathlib.Path, source_text: str, name: str,
                     image_format: str = "raw") -> pathlib.Path:
    source = work / f"{name}.c"
    obj = work / f"{name}.cc64o"
    image = work / (f"{name}.mz" if image_format == "mz64" else f"{name}.com")
    source.write_text(source_text, encoding="utf-8")
    run([str(ROOT / "cc64"), "-I", str(ROOT / "include/target"),
         "-I", str(ROOT / "include/cc64"), "-c", str(source), "-o", str(obj)], ROOT)
    command = [str(ROOT / "cc64"), "--link"]
    if image_format == "mz64":
        command += ["--format", "mz64"]
    command += [str(obj), "-o", str(image)]
    run(command, ROOT)
    return image


FILE_WRITE_PROGRAM = (
    "int cc64_create(const char *, int);\n"
    "int cc64_open(const char *);\n"
    "int cc64_write(int, const void *, unsigned long);\n"
    "int cc64_read(int, void *, unsigned long);\n"
    "int cc64_close(int);\n"
    "char scratch[8];\n"
    "int main(void) {\n"
    "  int handle = cc64_create(\"CC64W.TXT\", 0);\n"
    "  if (handle < 0) return 1;\n"
    "  if (cc64_write(handle, \"hello\", 5) != 5) return 2;\n"
    "  if (cc64_close(handle) != 0) return 3;\n"
    "  handle = cc64_open(\"CC64W.TXT\");\n"
    "  if (handle < 0) return 4;\n"
    "  if (cc64_read(handle, scratch, 5) != 5) return 5;\n"
    "  cc64_close(handle);\n"
    "  if (scratch[0] != 'h') return 6;\n"
    "  if (scratch[4] != 'o') return 7;\n"
    "  return 7;\n"
    "}\n"
)

SEEK_PROGRAM = (
    "int cc64_open(const char *);\n"
    "int cc64_close(int);\n"
    "int cc64_lseek(int, long, int);\n"
    "int cc64_read(int, void *, unsigned long);\n"
    "char scratch[8];\n"
    "int main(void) {\n"
    "  int handle = cc64_open(\"HELLO.TXT\");\n"
    "  if (handle < 0) return 1;\n"
    "  if (cc64_lseek(999, 0L, 0) != 6L) return 2;\n"
    "  if (cc64_lseek(handle, 0L, 3) != 1L) return 3;\n"
    "  long size = cc64_lseek(handle, 0L, 2);\n"
    "  if (size <= 0L) return 4;\n"
    "  if (cc64_lseek(handle, 0L, 0) != 0L) return 5;\n"
    "  if (cc64_lseek(handle, 1L, 0) != 1L) return 6;\n"
    "  if (cc64_lseek(handle, 0L, 1) != 1L) return 7;\n"
    "  if (cc64_lseek(handle, 0L, 0) != 0L) return 8;\n"
    "  if (cc64_read(handle, scratch, 1) != 1) return 9;\n"
    "  cc64_close(handle);\n"
    "  return 7;\n"
    "}\n"
)


VARARG_PROGRAM = (
    "#include <stdarg.h>\n"
    "int cc64_write(int, const void *, unsigned long);\n"
    "static int sum(int count, ...) {\n"
    "  va_list args;\n"
    "  va_start(args, count);\n"
    "  int total = 0;\n"
    "  int index;\n"
    "  for (index = 0; index < count; ++index) total += va_arg(args, int);\n"
    "  va_end(args);\n"
    "  return total;\n"
    "}\n"
    "static int named(int first, const char *second, int third, ...) {\n"
    "  va_list args;\n"
    "  va_start(args, third);\n"
    "  int extra = va_arg(args, int);\n"
    "  long more = va_arg(args, long);\n"
    "  va_end(args);\n"
    "  return first + (int)second[0] + third + extra + (int)more;\n"
    "}\n"
    "int main(void) {\n"
    "  int total = sum(4, 1, 2, 3, 4);\n"
    "  int mixed = named(1, \"A\", 2, 30, 40L);\n"
    "  cc64_write(1, \"s=\", 2);\n"
    "  if (total == 10) cc64_write(1, \"10\", 2); else cc64_write(1, \"??\", 2);\n"
    "  cc64_write(1, \" n=\", 3);\n"
    "  if (mixed == 138) cc64_write(1, \"138\", 3); else cc64_write(1, \"???\", 3);\n"
    "  cc64_write(1, \"\\n\", 1);\n"
    "  if (total != 10) return 1;\n"
    "  if (mixed != 138) return 2;\n"
    "  return 9;\n"
    "}\n"
)


# The pinned target shell starts a program with an empty command tail, so the
# observable startup contract here is an empty, null-terminated argument vector.
# A non-empty tail is exercised by the linker startup unit test on the host side.
ARGUMENT_PROGRAM = (
    "int cc64_write(int, const void *, unsigned long);\n"
    "int main(int argc, char **argv) {\n"
    "  cc64_write(1, \"argc=\", 5);\n"
    "  if (argc != 0) return 1;\n"
    "  cc64_write(1, (const char *)\"0\", 1);\n"
    "  if (argv[0] != 0) return 2;\n"
    "  cc64_write(1, \" a0=null\\n\", 8);\n"
    "  return 9;\n"
    "}\n"
)


def main() -> int:
    qemu = shutil.which("qemu-system-x86_64")
    if qemu is None or not TARGET.is_dir():
        message = "target: skipped (QEMU or target checkout unavailable)"
        if os.environ.get("CC64_REQUIRE_EMULATORS") == "1":
            raise SystemExit(message.replace("skipped", "required but unavailable"))
        print(message)
        return 0
    check_target()
    run(["make", "clean", "lean"], TARGET)
    with tempfile.TemporaryDirectory(prefix="cc64-target-") as temp:
        work = pathlib.Path(temp)
        cases = [
            ("C64R", "int main(void) { return 7; }", "raw", 7, None),
            ("C64S", "int f(int x){int y=0; switch(x){case 7: y=9; break; default: y=3;} return y;} int main(void){return f(7);}", "raw", 9, None),
            ("C64A", "int main(void){int a[2][2]={{1,2},{3,4}}; return a[1][1];}", "raw", 4, None),
            ("C64P", "int x=7; int *p=&x; int main(void){return *p;}", "mz64", 7, None),
            ("C64I", "int main(void){int x=4; int y=x++; return y*10+x;}", "raw", 45, None),
            ("C64X", "void cc64_exit(int); int main(void){cc64_exit(9); return 3;}", "raw", 9, None),
            ("C64O", "int cc64_open(const char *); int cc64_close(int); int main(void){int h=cc64_open(\"HELLO.TXT\"); if(h>=0) cc64_close(h); return h>=0?7:1;}", "raw", 7, None),
            ("C64U", "int cc64_putc(int); int main(void){cc64_putc(65); return 7;}", "raw", 7, "A"),
            ("C64T", "int cc64_write(int,const void*,unsigned long); int main(void){cc64_write(1,\"W\",1); return 8;}", "raw", 8, "W"),
            ("C64M", "void *cc64_alloc(unsigned long); void cc64_free(void*); int main(void){char *p=cc64_alloc(16); if(!p)return 1; p[0]=7; int v=p[0]; cc64_free(p); return v;}", "raw", 7, None),
            ("C64F", "int cc64_open(const char*); int cc64_read(int,void*,unsigned long); int cc64_close(int); int main(void){char b[4]; int h=cc64_open(\"HELLO.TXT\"); if(h<0)return 1; cc64_read(h,b,4); cc64_close(h); return b[0]==72?7:2;}", "raw", 7, None),
            ("C64B", "int zero_global; int main(void){zero_global=9; return zero_global;}", "raw", 9, None),
            ("C64Z", "int zero_global; int main(void){zero_global=9; return zero_global;}", "mz64", 9, None),
            ("C64G", "int main(int argc, char **argv){return argc;}", "raw", 0, None),
            ("C64H", ARGUMENT_PROGRAM, "raw", 9, "argc=0 a0=null"),
            ("C64W", FILE_WRITE_PROGRAM, "raw", 7, None),
            ("C64K", SEEK_PROGRAM, "raw", 7, None),
            ("C64V", VARARG_PROGRAM, "raw", 9, "s=10 n=138"),
        ]
        library_objects = target_library_objects(work)
        for entry in cases:
            name, source, image_format, expected, expected_text = entry[:5]
            command = entry[5] if len(entry) > 5 else name
            disk = work / f"{name}.img"
            image = compile_and_link(work, source, name, image_format)
            shutil.copy2(TARGET / "build/dos64-lean.img", disk)
            check_volume(disk, f"{name} fresh")
            run(["python3", str(ROOT / "tests/embed_fat12.py"), str(disk),
                 str(image), name], ROOT)
            check_volume(disk, f"{name} embedded")
            text = execute(qemu, disk, name, command, expected)
            check_volume(disk, f"{name} after QEMU")
            if f"Exit {expected}" not in text:
                raise SystemExit(f"{name}: target did not return {expected}")
            if expected_text is not None and expected_text not in text:
                raise SystemExit(f"{name}: target output lacked {expected_text!r}")
        library_source = work / "C64L.c"
        library_object = work / "C64L.cc64o"
        library_source.write_text(LIBRARY_PROGRAM, encoding="utf-8")
        run([str(ROOT / "cc64"), "-I", str(ROOT / "include" / "target"),
             "-I", str(ROOT / "include" / "cc64"), "-c", str(library_source),
             "-o", str(library_object)], ROOT)
        library_image = work / "C64L.mz"
        run([str(ROOT / "cc64"), "--link", "--format", "mz64",
             str(library_object), *library_objects, "-o", str(library_image)], ROOT)
        library_disk = work / "C64L.img"
        shutil.copy2(TARGET / "build/dos64-lean.img", library_disk)
        run(["python3", str(ROOT / "tests/embed_fat12.py"), str(library_disk),
             str(library_image), "C64L"], ROOT)
        text = execute(qemu, library_disk, "C64L", "C64L", 9)
        if "Exit 9" not in text:
            raise SystemExit("C64L: target library case did not return 9")
        if "A4BZCDE" not in text:
            raise SystemExit(f"C64L: target library output unexpected: {text[-200:]}")
        print(f"target: QEMU passed {len(cases)} raw/MZ64 image cases and "
              "one linked target-library case")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
