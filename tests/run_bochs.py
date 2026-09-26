#!/usr/bin/env python3
"""Run a compiler-produced image under the pinned Bochs target."""

from __future__ import annotations

import os
import pathlib
import pty
import select
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
TARGET = ROOT.parent / "MS-DOS64"
sys.path.insert(0, str(ROOT / "tools"))
import target_revision  # noqa: E402  (path is set above)
TARGET_REVISION = target_revision.TARGET_REVISION
VOLUME_ARGUMENTS = [
    "--vol-lba", "512", "--vol-sectors", "2880", "--sector-size", "512",
    "--kernel-lba", "16", "--kernel-sectors", "256",
]


def check_target() -> None:
    target_revision.check(TARGET)


def check_volume(image: pathlib.Path) -> None:
    result = subprocess.run(
        ["python3", str(TARGET / "tools/check_volume_clean.py"),
         *VOLUME_ARGUMENTS, str(image)],
        cwd=TARGET, capture_output=True, text=True, check=False,
    )
    if result.returncode != 0:
        detail = (result.stdout + result.stderr).strip()
        raise SystemExit(f"target volume cleanliness check failed: {detail}")


def stop_process(process: subprocess.Popen) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def run_bochs(bochs: str, config: pathlib.Path, serial: int,
              console_path: pathlib.Path, command: str, expected: int) -> bytes:
    console = console_path.open("wb")
    process = subprocess.Popen([bochs, "-q", "-f", str(config)],
                               stdin=subprocess.DEVNULL,
                               stdout=console,
                               stderr=subprocess.STDOUT)
    output = bytearray()
    sent = False
    deadline = time.monotonic() + 45.0
    try:
        while time.monotonic() < deadline:
            readable, _, _ = select.select([serial], [], [], 0.1)
            if readable:
                try:
                    output.extend(os.read(serial, 65536))
                except OSError:
                    pass
            if not sent and b"A> " in output:
                os.write(serial, command.encode("ascii") + b"\n")
                sent = True
            marker_text = f"Exit {expected}".encode("ascii")
            if marker_text in output:
                marker = output.index(marker_text) + len(marker_text)
                if b"A> " in output[marker:]:
                    return bytes(output)
            if process.poll() is not None:
                break
        detail = bytes(output).decode("utf-8", errors="replace")
        console_detail = console_path.read_text(encoding="utf-8", errors="replace")[-600:]
        raise SystemExit(f"Bochs did not complete the target run: {detail[-600:]} console={console_detail}")
    finally:
        stop_process(process)
        console.close()


def main() -> int:
    bochs = shutil.which("bochs")
    if bochs is None or not TARGET.is_dir():
        message = "bochs: skipped (Bochs or target checkout unavailable)"
        if os.environ.get("CC64_REQUIRE_EMULATORS") == "1":
            raise SystemExit(message.replace("skipped", "required but unavailable"))
        print(message)
        return 0
    check_target()
    with tempfile.TemporaryDirectory(prefix="cc64-bochs-") as temp:
        work = pathlib.Path(temp)
        subprocess.run(["make", "clean", "lean"], cwd=TARGET, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        cases = [
            ("BOCHR", "int main(void) { return 7; }\n", "raw", 7),
            ("BOCHZ", "int value = 7; int *pointer = &value; "
                      "int main(void) { return *pointer; }\n", "mz64", 7),
        ]
        for name, source_text, image_format, expected in cases:
            source = work / f"{name}.c"
            obj = work / f"{name}.cc64o"
            image = work / (f"{name}.mz64" if image_format == "mz64"
                            else f"{name}.com")
            disk = work / f"{name}.img"
            source.write_text(source_text, encoding="utf-8")
            subprocess.run([str(ROOT / "cc64"), "-c", str(source), "-o", str(obj)],
                           cwd=ROOT, check=True)
            command = [str(ROOT / "cc64"), "--link"]
            if image_format == "mz64":
                command += ["--format", "mz64"]
            command += [str(obj), "-o", str(image)]
            subprocess.run(command, cwd=ROOT, check=True)
            shutil.copy2(TARGET / "build/dos64-lean.img", disk)
            check_volume(disk)
            subprocess.run(["python3", str(ROOT / "tests/embed_fat12.py"),
                            str(disk), str(image), name], cwd=ROOT, check=True,
                           stdout=subprocess.DEVNULL)
            check_volume(disk)

            master, slave = pty.openpty()
            serial_path = os.ttyname(slave)
            os.close(slave)
            display_master, display_slave = pty.openpty()
            template = (TARGET / "bochsrc.txt.in").read_text(encoding="utf-8")
            template = template.replace("@IMAGE@", str(disk))
            template = template.replace("log: bochs.log",
                                        f"log: {work / (name + '.bochs.log')}")
            template = template.replace("display_library: nogui",
                                        "display_library: term")
            template = template.replace(
                "com1: enabled=1, mode=file, dev=serial.log",
                f"com1: enabled=1, mode=term, dev={serial_path}")
            config = work / f"{name}.cfg"
            config.write_text(template, encoding="utf-8")
            try:
                output = run_bochs(bochs, config, master,
                                   work / f"{name}.console.log", name, expected)
            except OSError as error:
                raise SystemExit(f"Bochs serial setup failed: {error}") from error
            finally:
                os.close(master)
                os.close(display_master)
                os.close(display_slave)
            marker = f"Exit {expected}".encode("ascii")
            if marker not in output:
                raise SystemExit(f"Bochs did not report {name} exit {expected}")
            check_volume(disk)
    print("bochs: target returned exit 7 for raw and MZ64 cases")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
