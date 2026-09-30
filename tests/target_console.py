#!/usr/bin/env python3
"""A program receives the console input the target delivers.

The target's shell reads console input from the line, and both emulators put a
test's keystrokes there. The services a user program uses to receive input used
to consult only the PS/2 keyboard, so a child could never see the input the
target actually delivers: it polled, was told nothing was waiting, and a read
returned end of file. A 24,461-line editor drew a full screen on the target and
then never saw a key.

This sends a character to a program running on the target and requires the two
answers a program asks for to agree: the poll says a character is waiting, and
the read that follows returns the character that was sent rather than nothing.
The character is the last byte before a newline so that a run cannot be
satisfied by the command line the harness itself wrote.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
TARGET = ROOT.parent / "MS-DOS64"
sys.path.insert(0, str(ROOT / "tools"))
import target_revision  # noqa: E402  (path is set above)

IMAGE = "CCON.COM"
EXPECTED = "console input: poll reported ready and the read returned the character"
# The character is the last thing written, so a program that read its own
# command line rather than the console cannot produce this.
CHARACTER = "Z"


def run(command: list[str], cwd: pathlib.Path) -> None:
    subprocess.run(command, cwd=cwd, check=True, timeout=300,
                   stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def build(work: pathlib.Path) -> pathlib.Path:
    source = work / "CCON.C"
    source.write_text((ROOT / "tests" / "target_console.c").read_text(encoding="utf-8"),
                      encoding="utf-8")
    obj = work / "CCON.cc64o"
    run([str(ROOT / "cc64"), "-I", str(ROOT / "include" / "target"),
         "-I", str(ROOT / "include" / "cc64"), "-c", str(source), "-o", str(obj)],
        ROOT)
    # poll and read are the target library's, not thunks, so the units that
    # define them are linked in. The image is the load-biased form because the
    # library holds a table of pointers.
    library = []
    for index, unit in enumerate(sorted((ROOT / "src" / "runtime").glob("target_*.c"))):
        output = work / f"L{index}.cc64o"
        run([str(ROOT / "cc64"), "-I", str(ROOT / "include" / "target"),
             "-I", str(ROOT / "include" / "cc64"), "-I", str(ROOT / "src"),
             "-c", str(unit), "-o", str(output)], ROOT)
        library.append(str(output))
    image = work / IMAGE
    run([str(ROOT / "cc64"), "--link", "--format", "mz64", str(obj),
         *library, "-o", str(image)], ROOT)
    return image


def main() -> int:
    if not TARGET.is_dir():
        print("target console: skipped (target checkout unavailable)")
        return 0
    if not target_revision.require_emulator(
            "qemu-system-x86_64",
            "target console: skipped (QEMU unavailable)"):
        return 0
    target_revision.check(TARGET)
    with tempfile.TemporaryDirectory(prefix="cc64-console-") as temp:
        work = pathlib.Path(temp)
        image = build(work)
        run(["make", "clean", "lean"], TARGET)
        disk = work / "console.img"
        disk.write_bytes((TARGET / "build" / "dos64-lean.img").read_bytes())
        run(["python3", str(ROOT / "tests" / "embed_fat12.py"), str(disk),
             str(image), "CCON"], ROOT)
        transcript = work / "console.log"
        with transcript.open("w", encoding="utf-8") as stream:
            process = subprocess.Popen(
                ["qemu-system-x86_64", "-drive", f"file={disk},format=raw",
                 "-serial", "stdio", "-display", "none"],
                stdin=subprocess.PIPE, stdout=stream, stderr=subprocess.STDOUT,
                text=True)
            # The shell prompts as it boots, so the first command is sent after
            # a pause; sending it at once loses the first character. The
            # character the program is waiting for goes in well after the
            # command, so it cannot be the command's own newline.
            time.sleep(6.0)
            process.stdin.write("CCON\n")
            process.stdin.flush()
            time.sleep(10.0)
            process.stdin.write(CHARACTER)
            process.stdin.flush()
            deadline = time.monotonic() + 60.0
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    break
                time.sleep(0.2)
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
        text = transcript.read_text(encoding="utf-8", errors="replace")
    if "Loaded, pid" not in text:
        raise SystemExit("target console: the program did not start")
    if EXPECTED in text:
        print("target console: " + EXPECTED)
        return 0
    reported = [line.strip() for line in text.splitlines()
                if line.startswith("console input:")]
    detail = reported[0] if reported else "the program said nothing"
    raise SystemExit(
        f"target console: {detail}. The target delivers console input on the "
        f"line its own shell reads, and the services a program uses to receive "
        f"input have to see the same input: a program that polls and reads must "
        f"get the character that was sent.")


if __name__ == "__main__":
    sys.exit(main())
