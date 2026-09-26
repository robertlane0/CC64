"""Single source of the MS-DOS64 revision policy for CC64 integration runs.

AGENTS.md section 1 pins the target: the checkout must sit at the recorded
reference revision, and the only permitted change is one on the edit branch that
exists to make the compiler operational. Every harness that boots a target image
asks this module, so the rule is stated once and the evidence it prints is the
same everywhere.
"""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys

# The unmodified reference revision CC64 is developed against.
TARGET_REVISION = "13c3cedb05ad75592c17bf2006ba8617c8761a38"
# The only branch allowed to carry target changes.
TARGET_EDIT_BRANCH = "edit"


def _git(target: pathlib.Path, *arguments: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", "-C", str(target), *arguments],
                          capture_output=True, text=True, check=False)


def check(target: pathlib.Path) -> None:
    """Verify the checkout, or explain why the run is being made against an edit."""
    if not target.is_dir():
        return
    override = os.environ.get("CC64_TARGET_REVISION")
    if override is not None and os.environ.get("CC64_REQUIRE_EMULATORS") == "1":
        raise SystemExit("strict release mode does not allow a target revision override")
    head = _git(target, "rev-parse", "HEAD")
    if head.returncode != 0:
        raise SystemExit("target checkout has no readable revision")
    actual = head.stdout.strip()
    branch = _git(target, "rev-parse", "--abbrev-ref", "HEAD").stdout.strip()
    if _git(target, "diff", "--quiet", "HEAD", "--").returncode != 0:
        raise SystemExit("target checkout has uncommitted tracked source changes")
    if branch == TARGET_EDIT_BRANCH:
        # The dependency is deliberate, so the evidence has to name it: the edit
        # revision, its ancestry from the pinned reference, and what it changes.
        if override is not None:
            raise SystemExit("strict release mode does not allow the target edit branch")
        if _git(target, "merge-base", "--is-ancestor", TARGET_REVISION, "HEAD").returncode != 0:
            raise SystemExit(f"target edit branch does not descend from pinned {TARGET_REVISION}")
        changed = _git(target, "diff", "--name-only", f"{TARGET_REVISION}..HEAD", "--").stdout.split()
        print(f"target: edit branch {actual[:7]} over pinned {TARGET_REVISION[:7]}; "
              f"changed {', '.join(changed) if changed else '(none)'}")
        return
    expected = override or TARGET_REVISION
    if actual != expected:
        raise SystemExit(f"target revision {actual} does not match pinned {expected}")


def main() -> int:
    root = pathlib.Path(__file__).resolve().parent.parent
    check(root.parent / "MS-DOS64")
    return 0


if __name__ == "__main__":
    sys.exit(main())
