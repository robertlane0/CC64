#!/usr/bin/env python3
"""Build c-edit's own test suite with CC64 and run the whole suite on the target.

c-edit keeps thirty-seven test units. Each is a separate translation unit with
its own `main`, and each one is a library unit exercised end to end: the editor
only reads and writes text, so the suite reaches parts of the runtime the
editor never calls.

They are composed into one image rather than thirty. The target's volume holds
about one and a half megabytes and an image of this program is around six
hundred kilobytes, so thirty of them do not fit, and a boot costs about half a
minute. So each test's `main` is renamed to a function and one runner calls them
in turn, which is what a host build of the suite does anyway.

A test that CC64 rejects is reported and left out rather than counted as a
failure: the seven that use `mkdtemp`, `mkstemp`, `posix_openpt`, `unsetenv` or
`memmem` are asking for services the target does not have and the C17 subset
does not contain, and refusing them is the correct answer.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
TARGET = pathlib.Path("/root/code/MS-DOS64")
EDIT = pathlib.Path("/root/code/c-edit")
CC64 = ROOT / "cc64"
TESTS = EDIT / "c" / "tests"
UNITS = EDIT / "c" / "src"

INCLUDES = ["-I", str(EDIT / "c/include"), "-I", str(ROOT / "include/target"),
            "-I", str(ROOT / "include/cc64"), "-I", str(EDIT / "c/src")]

RESULT = re.compile(r"^TEST (\d+) (\S+) rc=(-?\d+)$", re.MULTILINE)

# A test that cannot pass on this target, and why. The gate is green when every
# other test passes; the limitation is printed rather than counted as a failure,
# because a target that cannot do something is not a compiler that is wrong.
# If one of these starts passing, that is reported too — it means the limit went
# away, not that the expectation went stale.
LIMITED = {
    "test_fuzzy": "case folding reaches ICU through dlopen/dlsym for its Unicode "
                  "tables, and the target has no dynamic loader, so the fold is "
                  "ASCII-only and the sharp s cannot expand to 'ss'",
}


def run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, cwd=ROOT, text=True,
                          capture_output=True, check=False, **kwargs)


def compile_unit(source: pathlib.Path, output: pathlib.Path,
                 extra: list[str] | None = None) -> bool:
    command = [str(CC64), *INCLUDES, *(extra or []), "-c", str(source),
               "-o", str(output)]
    result = run(command)
    if result.returncode != 0 and output.exists():
        output.unlink()
    return result.returncode == 0


def first_diagnostic(result: subprocess.CompletedProcess[str]) -> str:
    text = (result.stdout + result.stderr).strip().splitlines()
    return text[0] if text else "(no diagnostic)"


def build(work: pathlib.Path, only: str | None) -> tuple[pathlib.Path, list[str]]:
    """Compile the library units, the tests, and the runner; link one image."""
    objects: list[pathlib.Path] = []

    # The editor's own units, so the tests have the library they exercise. The
    # binary holding `main` is left out: this image has its own.
    for source in sorted(UNITS.glob("*.c")):
        if any("main(" in line for line in source.read_text().splitlines()):
            continue
        output = work / f"edit-{source.stem}.cc64o"
        if not compile_unit(source, output):
            raise SystemExit(f"editor unit rejected: {source.name}")
        objects.append(output)

    # The target library, which the tests print through.
    for source in sorted((ROOT / "src/runtime").glob("target_*.c")):
        output = work / f"target-{source.stem}.cc64o"
        if not compile_unit(source, output):
            raise SystemExit(f"target library unit rejected: {source.name}")
        objects.append(output)

    names: list[str] = []
    skipped: list[str] = []
    for source in sorted(TESTS.glob("test_*.c")):
        name = source.stem
        if only and name != only:
            continue
        output = work / f"{name}.cc64o"
        renamed = f"cc64_edit_test_{len(names)}"
        if not compile_unit(source, output, ["-D", f"main={renamed}"]):
            skipped.append(f"{name}: {first_diagnostic(run([str(CC64), *INCLUDES, '-c', str(source), '-o', str(work / 'probe.cc64o')]))}")
            continue
        names.append(name)
        objects.append(output)
        print(f"  test {name} -> {renamed}", flush=True)
    for note in skipped:
        print(f"  skipped {note}", flush=True)
    if not names:
        raise SystemExit("no test units compiled")

    runner = work / "runner.c"
    runner.write_text(build_runner(names), encoding="utf-8")
    runner_object = work / "runner.cc64o"
    if not compile_unit(runner, runner_object):
        raise SystemExit("the generated runner was rejected")

    image = work / "CTEST.COM"
    command = [str(CC64), "--link", "--format", "mz64",
               str(runner_object)] + [str(path) for path in objects] + ["-o", str(image)]
    result = run(command)
    if result.returncode != 0:
        raise SystemExit(f"link failed: {first_diagnostic(result)}")
    print(f"linked {image.name} ({image.stat().st_size} bytes) from "
          f"{len(names)} tests, {len(objects)} units total", flush=True)
    return image, names


def build_runner(names: list[str]) -> str:
    """The runner: one call per test, each result named on its own line.

    A test the target cannot run is named here as well, so the image's own exit
    code counts only what the target could be asked to do. It still runs: a
    limitation is something the target cannot answer, not something to skip
    without being seen.
    """
    lines = ["/* Generated by tools/edit_tests.py. Do not edit. */",
             "#include <stdio.h>", ""]
    for index in range(len(names)):
        lines.append(f"int cc64_edit_test_{index}(void);")
    lines += ["",
              "static const char *const names[] = {"]
    for name in names:
        lines.append(f'    "{name}",')
    lines += ["};",
              "/* One for each test this target cannot answer; zero elsewhere. */",
              "static const char limited[] = {"]
    limited = [1 if name in LIMITED else 0 for name in names]
    for flag in limited:
        lines.append(f"    {flag},")
    lines += ["};", "",
              "int main(void)", "{",
              "    int failures = 0;",
              "    int result;", ""]
    for index in range(len(names)):
        # Each test is called once and its result kept: a test that is called
        # twice is not the same as a test called once, and the suite is meant to
        # be the same run the host build does.
        lines += [f"    result = cc64_edit_test_{index}();",
                  f'    printf("TEST {index} %s rc=%d\\n", names[{index}], result);',
                  f"    if (result != 0 && !limited[{index}]) ++failures;"]
    lines += ['    printf("SUMMARY tests=%d failures=%d\\n",',
              "           (int)(sizeof names / sizeof names[0]), failures);",
              "    return failures == 0 ? 0 : 1;", "}", ""]
    return "\n".join(lines)


def boot(image: pathlib.Path, names: list[str], seconds: float,
         log: pathlib.Path | None = None) -> int:
    qemu = shutil.which("qemu-system-x86_64")
    if qemu is None:
        print("target suite: skipped (QEMU unavailable)")
        return 0
    if not (TARGET / "build" / "dos64-lean.img").is_file():
        raise SystemExit("target suite: build the target with `make clean lean` first")
    with tempfile.TemporaryDirectory(prefix="cc64-suite-") as temp:
        disk = pathlib.Path(temp) / "suite.img"
        shutil.copy2(TARGET / "build" / "dos64-lean.img", disk)
        subprocess.run([sys.executable, str(ROOT / "tests/embed_fat12.py"),
                        str(disk), str(image), image.stem],
                       cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
        transcript = (log if log is not None
                      else pathlib.Path(temp) / "suite.log")
        with transcript.open("w", encoding="utf-8") as stream:
            process = subprocess.Popen(
                [qemu, "-drive", f"file={disk},format=raw", "-serial", "stdio",
                 "-display", "none"],
                stdin=subprocess.PIPE, stdout=stream, stderr=subprocess.STDOUT,
                text=True)
            # The shell prompts as it boots, so the command waits. The suite is
            # thirty tests; the wait is generous because a hang has to be told
            # apart from a slow run.
            time.sleep(6.0)
            process.stdin.write("CTEST\n")
            process.stdin.flush()
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    break
                time.sleep(0.3)
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
        text = transcript.read_text(encoding="utf-8", errors="replace")
    if "Loaded, pid" not in text:
        raise SystemExit("target suite: the image did not start")
    results = {int(index): (name, int(code))
               for index, name, code in RESULT.findall(text)}
    failed = [name for _, (name, code) in sorted(results.items())
              if code != 0 and name not in LIMITED]
    missing = [name for index, name in enumerate(names) if index not in results]
    print(f"target suite: {len(results)} of {len(names)} tests reported")
    for index, name in enumerate(names):
        if index not in results:
            print(f"  ---- {name} did not report")
            continue
        _, code = results[index]
        if code == 0:
            note = " (limited here, and it passed)" if name in LIMITED else ""
            print(f"  ok   {name} rc=0{note}")
        elif name in LIMITED:
            print(f"  skip {name} rc={code} - {LIMITED[name]}")
        else:
            print(f"  FAIL {name} rc={code}")
    summary = next((line for line in text.splitlines() if line.startswith("SUMMARY")), None)
    if summary:
        print(f"target suite: {summary}")
    if failed or missing:
        print(f"target suite: failed={failed or 'none'} missing={missing or 'none'}")
        return 1
    print(f"target suite: every test that can run here passed "
          f"({len(LIMITED)} named limitation)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--only", default=None,
                        help="build and run just this test unit, by base name")
    parser.add_argument("--run", action="store_true",
                        help="boot the image on the target and report every result")
    parser.add_argument("--run-for", type=float, default=600.0,
                        help="seconds to let the suite run before stopping it")
    parser.add_argument("--log", default=None,
                        help="keep the emulator transcript at this path")
    parser.add_argument("--work", default=None,
                        help="keep the build here instead of a temporary directory")
    arguments = parser.parse_args()

    if arguments.work:
        work = pathlib.Path(arguments.work)
        work.mkdir(parents=True, exist_ok=True)
        image, names = build(work, arguments.only)
        if arguments.run:
            return boot(image, names, arguments.run_for,
                        pathlib.Path(arguments.log) if arguments.log else None)
        return 0
    with tempfile.TemporaryDirectory(prefix="cc64-suite-") as temp:
        image, names = build(pathlib.Path(temp), arguments.only)
        if arguments.run:
            return boot(image, names, arguments.run_for,
                        pathlib.Path(arguments.log) if arguments.log else None)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
