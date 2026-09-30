#!/usr/bin/env python3
"""The clock a program reads back is the clock that was set.

Two defects meet at this interface, and this test is built so that each is
visible on its own.

The target's own selftest asserts that a date survives a set and a get, and it
now passes. The target used to write the month while still holding the old day,
which can name a date that never existed, and what the clock resolved that to
was not the date it was given. That half is evidenced by the target's own
suite, which goes from 94 passed and 1 failed to 95 passed and none. It is not
reproduced here on purpose: whether the clock's update tick lands between the
guest's writes decides what an impossible date normalises to, so a test that
depended on it would be testing the emulator's timing rather than either side.

What this test pins is the compiler's half, which is deterministic. The runtime
thunk that reads the date folded the two registers the service returns together
by shifting them, so it returned the year shifted down and the month shifted
up, and dropped the day: a program asking the target for the time of day was
told 2334-7-23. The registers are joined by moving the one that already holds
its fields into place instead. This test fails with the old thunk and passes
with the new one.
"""

from __future__ import annotations

import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
TARGET = ROOT.parent / "MS-DOS64"
sys.path.insert(0, str(ROOT / "tools"))
import target_revision  # noqa: E402  (path is set above)

# A date the target must return exactly. February 2001 has 28 days.
EXPECTED = "2001-02-28"
# Two dates, so the reading cannot be whatever the clock happened to hold when
# the program started. The second crosses a month boundary, which is the shape
# the target's own setter has to be careful about.
SET_COMMANDS = ["DATE 2001-01-29", "DATE 2001-02-28"]


def run(command: list[str], cwd: pathlib.Path) -> None:
    subprocess.run(command, cwd=cwd, check=True, timeout=300,
                   stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def build(work: pathlib.Path) -> pathlib.Path:
    source = work / "RTC.C"
    source.write_text((ROOT / "tests" / "target_rtc.c").read_text(encoding="utf-8"),
                      encoding="utf-8")
    obj = work / "RTC.cc64o"
    run([str(ROOT / "cc64"), "-I", str(ROOT / "include" / "target"),
         "-I", str(ROOT / "include" / "cc64"), "-c", str(source), "-o", str(obj)],
        ROOT)
    image = work / "RTC.COM"
    run([str(ROOT / "cc64"), "--link", "--format", "mz64", str(obj),
         "-o", str(image)], ROOT)
    return image


def session(disk: pathlib.Path, commands: list[str], seconds: float) -> str:
    """Run shell commands on one boot and return the whole transcript."""
    transcript = disk.with_suffix(".log")
    with transcript.open("w", encoding="utf-8") as stream:
        process = subprocess.Popen(
            ["qemu-system-x86_64", "-drive", f"file={disk},format=raw",
             "-serial", "stdio", "-display", "none"],
            stdin=subprocess.PIPE, stdout=stream, stderr=subprocess.STDOUT,
            text=True)
        for command in commands:
            process.stdin.write(command + "\n")
            process.stdin.flush()
            time.sleep(1.5)
        process.stdin.write("EXIT\n")
        process.stdin.flush()
        process.stdin.close()
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if process.poll() is not None:
                break
            time.sleep(0.2)
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
    return transcript.read_text(encoding="utf-8", errors="replace")


def main() -> int:
    if not TARGET.is_dir():
        print("target clock: skipped (target checkout unavailable)")
        return 0
    if not target_revision.require_emulator(
            "qemu-system-x86_64",
            "target clock: skipped (QEMU unavailable)"):
        return 0
    target_revision.check(TARGET)
    with tempfile.TemporaryDirectory(prefix="cc64-rtc-") as temp:
        work = pathlib.Path(temp)
        image = build(work)
        run(["make", "clean", "lean"], TARGET)
        disk = work / "rtc.img"
        disk.write_bytes((TARGET / "build" / "dos64-lean.img").read_bytes())
        run(["python3", str(ROOT / "tests" / "embed_fat12.py"), str(disk),
             str(image), "RTC"], ROOT)
        # The shell prompts for input as it boots, so the first command is sent
        # after a pause; sending it at once loses the first character.
        text = session(disk, ["", *SET_COMMANDS, "RTC"], 120.0)
    if "Loaded, pid" not in text:
        raise SystemExit("target clock: the reader did not start")
    readings = re.findall(r"(\d{4,})-(\d{1,2})-(\d{1,2})", text)
    if not readings:
        raise SystemExit(f"target clock: no date was read back; see {disk}.log")
    reported = "-".join(readings[-1])
    if reported != EXPECTED:
        raise SystemExit(
            f"target clock: the target was told {SET_COMMANDS[-1].split()[1]} "
            f"and a program reads {reported}. Either the clock did not keep the "
            f"date, or the runtime did not read it: the thunk that folds the "
            f"two registers the service returns has to join them by moving the "
            f"one that already holds its fields into place, because shifting "
            f"them changes which field is which.")
    print(f"target clock: {reported} read back by a compiler-produced program")
    return 0


if __name__ == "__main__":
    sys.exit(main())
