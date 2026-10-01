#!/usr/bin/env python3
"""Hold the aggregate-initializer and formatter behaviour that only the target showed.

Three of the defects these hold were silent: a program compiled with them ran,
returned plausible values, and was wrong. A pointer member of a struct
initialized inside a braced list held the first character of a string instead of
the address of the literal, so reading through it returned whatever byte answered
at address 104; a compound literal whose element came from an expression kept
whatever the zeroing had put there; and a precision or width taken from the
argument list was not read at all, so a diagnostic meant to show a mismatch
printed its own format string. None of that is visible to a host test of the
compiler, because it is the generated program that is wrong, so it is held here
by running the program on the target.

The probe prints the values rather than only pass or fail, so a failure says
which construct broke rather than that something did.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
PROBE = ROOT / "tests" / "target_formats.c"

# Each expectation is a line the probe must print, with the value spelled out.
# A substring is given so the line may carry the label as well.
EXPECTED = [
    # An initializer inside a braced list is converted to the member's type, so
    # a string literal in a pointer position holds the literal's address.
    "PROBE variable: kind=1 len=5 byte0=104 r=0",
    # The same object through a compound literal of an array of structs.
    "PROBE compound: kind=1 len=5 byte0=104",
    # An aggregate initialized from one expression is a copy of the whole object.
    "PROBE listed: kind=4 len=1",
    # A width and a precision taken from the argument list, and the forms a
    # negative precision and a zero precision have.
    "PROBE printf star: [ab]",
    "PROBE printf star0: []",
    "PROBE printf starneg: [abc]",
    "PROBE printf wstar: [    42]",
    "PROBE printf dot: [ab]",
    "PROBE printf prec: [00042]",
    "PROBE printf prec0: [42][]",
    "PROBE printf zero: [00042]",
    "PROBE printf left: [42   ]",
    "PROBE printf pad: [   42]",
    "PROBE done checks=25 failed=0",
]


def main() -> int:
    result = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "target_probe.py"), str(PROBE)],
        cwd=ROOT, text=True, capture_output=True, timeout=900,
    )
    text = result.stdout + result.stderr
    # The transcript is the whole boot, banner and all, so the marker the probe
    # prints when it cannot run is matched in full rather than by a word that
    # the boot output also contains.
    for line in text.splitlines():
        if line.startswith("probe: skipped") or line.startswith("probe: the image"):
            print("target formats: " + line)
            return 0 if line.startswith("probe: skipped") else 1
    missing = [want for want in EXPECTED if want not in text]
    if missing:
        for want in missing:
            print(f"target formats: missing {want}")
        for line in text.splitlines():
            if line.startswith("PROBE"):
                print("  " + line)
        raise SystemExit("target formats: the probe did not report what it must")
    print(f"target formats: {len(EXPECTED)} expectations held on the target")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
