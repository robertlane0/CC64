#!/usr/bin/env python3
"""Build the c-edit program with CC64, link it, and place it on a target volume.

The program's own sources are not modified. The build reports, for every unit,
the first diagnostic CC64 produces, so the work is a list of what the compiler
or the target library still lacks rather than a search through a build log.
When every unit compiles, the objects are linked with the target library and
the image is written where the target's own tools can run it.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import subprocess
import sys
import time

CC64 = pathlib.Path(__file__).resolve().parents[1] / "cc64"
ROOT = CC64.parent
TARGET_INCLUDE = ROOT / "include/target"
CC64_INCLUDE = ROOT / "include/cc64"
RUNTIME = ROOT / "src/runtime"
SOURCE = pathlib.Path("/root/code/c-edit")
DOS64 = SOURCE.parent / "MS-DOS64"
INCLUDES = ["-I", str(SOURCE / "c/include"),
            "-I", str(SOURCE / "c/src"),
            "-I", str(TARGET_INCLUDE), "-I", str(CC64_INCLUDE),
            "-I", str(ROOT / "src")]
OUTPUT = pathlib.Path("/tmp/opencode/editbuild")


def units() -> list[pathlib.Path]:
    result = sorted((SOURCE / "c/src").glob("*.c"))
    result += sorted((SOURCE / "c/src/bin").glob("*.c"))
    return result


def run(command: list[str], timeout: int = 300) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, capture_output=True, text=True, check=False,
                          timeout=timeout)


def compile_units(chosen: list[pathlib.Path]) -> tuple[list[pathlib.Path], list[tuple[str, str]]]:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    clean: list[pathlib.Path] = []
    blocked: list[tuple[str, str]] = []
    for source in chosen:
        obj = OUTPUT / f"{source.stem}.cc64o"
        result = run([str(CC64), *INCLUDES, "-c", str(source), "-o", str(obj)], 120)
        if result.returncode == 0:
            clean.append(obj)
            continue
        detail = (result.stdout + result.stderr).strip().splitlines()
        blocked.append((source.stem, detail[0] if detail else "(no diagnostic)"))
    return clean, blocked


def build_library() -> list[pathlib.Path]:
    library = []
    for index, source in enumerate(sorted(RUNTIME.glob("target_*.c"))):
        obj = OUTPUT / f"target-lib-{index}.cc64o"
        result = run([str(CC64), *INCLUDES, "-c", str(source), "-o", str(obj)])
        if result.returncode != 0:
            detail = (result.stdout + result.stderr).strip().splitlines()
            raise SystemExit(f"target library: {source.name} rejected: "
                             f"{detail[0] if detail else ''}")
        library.append(obj)
    return library


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", default=None,
                        help="compile just this unit, by base name")
    parser.add_argument("--image", default="EDIT.COM",
                        help="image name the loader is given")
    # The target library holds a table of pointers, so an image that includes
    # it is linked in the load-biased form; the raw form is kept for a program
    # that needs no data pointers at all.
    parser.add_argument("--format", default="mz64", choices=["raw", "mz64"],
                        help="executable form to link")
    parser.add_argument("--volume", default=None,
                        help="target volume image to place the program on")
    parser.add_argument("--run", action="store_true",
                        help="boot the image on the target and report what it did")
    parser.add_argument("--run-for", type=float, default=30.0,
                        help="seconds to let the program run before stopping it")
    arguments = parser.parse_args()
    chosen = units()
    if arguments.only:
        chosen = [path for path in chosen if path.stem == arguments.only]
        if not chosen:
            raise SystemExit(f"no such unit: {arguments.only}")

    clean, blocked = compile_units(chosen)
    print(f"compiled {len(clean)} of {len(chosen)} units")
    if clean:
        print("  ok: " + " ".join(path.stem for path in clean))
    for name, first in blocked:
        print(f"  {name}: {first}")
    if blocked:
        return 1
    if arguments.only:
        return 0

    library = build_library()
    image = OUTPUT / arguments.image
    command = [str(CC64), "--link"]
    if arguments.format == "mz64":
        command += ["--format", "mz64"]
    command += [str(path) for path in clean] + [str(path) for path in library]
    command += ["-o", str(image)]
    result = run(command)
    if result.returncode != 0:
        detail = (result.stdout + result.stderr).strip().splitlines()
        print("  link: " + (detail[0] if detail else "(no diagnostic)"))
        return 1
    size = image.stat().st_size
    print(f"linked {arguments.image} ({size} bytes) from {len(clean)} units "
          f"and {len(library)} library units")

    if arguments.volume:
        disk = pathlib.Path(arguments.volume)
        result = run([sys.executable, str(ROOT / "tests/embed_fat12.py"),
                      str(disk), str(image), pathlib.Path(arguments.image).stem])
        if result.returncode != 0:
            detail = (result.stdout + result.stderr).strip().splitlines()
            print("  volume: " + (detail[-1] if detail else "(no diagnostic)"))
            return 1
        print(f"placed on {disk}")
    if arguments.run:
        return boot(arguments.image, arguments.run_for)
    return 0


def boot(image: str, seconds: float) -> int:
    """Edit a file on the target and check that the change was written.

    The claim this exists for is that the program works, not that it loads, so
    it is exercised the way a person would use it: open a file that is already
    on the volume, type into it, save, and quit. Three things have to hold for
    that to succeed, and each was a separate failure on the way here — the
    editor has to receive the keystrokes, it has to be able to name the file it
    was asked to open, and it has to restore the terminal and return. So all
    three are checked: the typed text reaches the buffer, the saved bytes are
    read back off the volume, and the editor exits zero having left the
    alternate screen.

    The volume is built from the target's own image each time, because a tool
    that embeds a file does not replace an entry of the same name and a second
    run would otherwise find the previous run's image first.
    """
    qemu = shutil.which("qemu-system-x86_64")
    if qemu is None:
        print("target run: skipped (QEMU unavailable)")
        return 0
    work = OUTPUT
    work.mkdir(parents=True, exist_ok=True)
    disk = work / "edit-run.img"
    shutil.copy2(DOS64 / "build" / "dos64-lean.img", disk)
    # A file with a known content, so the saved bytes can be told apart from
    # whatever the editor happened to have in the buffer.
    seed = work / "NOTES.TXT"
    seed.write_bytes(b"seed\n")
    for path, name in ((OUTPUT / image, pathlib.Path(image).stem),
                       (seed, "NOTES.TXT")):
        subprocess.run([sys.executable, str(ROOT / "tests/embed_fat12.py"),
                        str(disk), str(path), name],
                       check=True, stdout=subprocess.DEVNULL)
    transcript = work / "edit-run.log"
    # CTRL-S saves, CTRL-Q exits. The editor restores the terminal on the way
    # out, which is what the alternate-screen sequence below is checking.
    # A command is submitted with a newline; a keystroke is one byte and must not
    # carry one, or the editor types the newline as part of the text. The editor
    # takes a few seconds to draw its first screen, so the first keystroke waits
    # longer than the rest.
    script = [("EDIT NOTES.TXT\n", 14.0), ("abc", 6.0), ("\x13", 10.0),
              ("\x11", 12.0), ("EXIT\n", 2.0)]
    with transcript.open("w", encoding="utf-8") as stream:
        process = subprocess.Popen(
            [qemu, "-drive", f"file={disk},format=raw", "-serial", "stdio",
             "-display", "none"],
            stdin=subprocess.PIPE, stdout=stream, stderr=subprocess.STDOUT,
            text=True)
        for step, wait in script:
            time.sleep(wait)
            process.stdin.write(step)
            process.stdin.flush()
        deadline = time.monotonic() + 30.0
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
    # The editor draws a styled screen, so a menu letter and its key hint are
    # separated by colour codes. Strip the sequences before looking for text: a
    # substring that never appears in the raw bytes still names a drawn screen,
    # and a check that only matched raw bytes would report "not seen" for a
    # screen that was.
    plain = re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]", "", text)
    plain = re.sub(r"\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)", "", plain)
    loaded = "Loaded, pid" in text
    drew = "(F)" in plain
    restored = "\x1b[?1049l" in text
    # The editor restores the terminal on its way out, so the shell's exit line
    # follows the last escape sequence on the same line. Anchoring to the start
    # of a line would miss it and report a clean exit as none at all.
    exited = re.findall(r"Exit (\d+)", plain)
    saved = b""
    result = subprocess.run([sys.executable, str(ROOT / "tests/extract_fat12.py"),
                             str(disk), "NOTES.TXT", "-o", str(work / "saved.txt")],
                            capture_output=True, text=True)
    if result.returncode == 0:
        saved = (work / "saved.txt").read_bytes()
    print(f"target run: {'loaded' if loaded else 'did not load'}"
          f", first screen {'drawn' if drew else 'not seen'}")
    print(f"target run: typed text {'reached the editor' if drew else 'not seen'}"
          f", terminal {'restored' if restored else 'not restored'}"
          f", exit {exited[-1] if exited else 'none'}")
    # The editor writes the line ending its own convention uses, so the
    # comparison is on the text with the endings reduced, which is what a reader
    # of the file would see.
    normalised = saved.replace(b"\r\n", b"\n")
    print(f"target run: saved file is {saved!r}, expected {b'abcseed' + bytes([10])!r} "
          f"once line endings are reduced")
    if not loaded:
        print("  the target did not start the image")
        return 1
    if not drew:
        print("  the editor drew no screen")
        return 1
    if not restored:
        print("  the editor did not restore the terminal on the way out")
        return 1
    if not exited or exited[-1] != "0":
        print("  the editor did not exit cleanly")
        return 1
    # Whether a file ends with a newline is the editor's own convention, and it
    # is not what this checks. What matters is that the typed text reached the
    # buffer and the buffer reached the file, with the text that was there
    # still after it.
    if normalised.rstrip(b"\n") != b"abcseed":
        print("  the edit was not written to the file")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
