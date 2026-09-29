#!/usr/bin/env python3
"""Run the target conformance corpus with a compiler the target built.

The bootstrap compiler builds a complete image of itself and links it with the
target runtime library. That image is then staged on a volume together with the
project's own headers and the conformance corpus. The compiler in the image
compiles every corpus program with its own front end, lowering, and encoder,
links each one with its own linker, and the resulting image is then executed.

Two things are compared for every case. The object the target-built compiler
wrote is compared byte for byte with the object the bootstrap compiler wrote
from the same source, so a construct the front end lowers differently is caught
even when the program happens to produce the same exit code. The image is then
run and its exit code and output are compared with what the corpus records, so
a program the target-built compiler produced but that does not work is caught
too.

The volume holds no directories and its data area is smaller than the compiler
image, the whole corpus, and every output at once, so the cases run in batches:
each batch stages the image, the headers, and the sources of the cases it
holds, and the objects are read back and released before the next batch.
"""

from __future__ import annotations

import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
TARGET = ROOT.parent / "MS-DOS64"
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))
import corpus  # noqa: E402  (path is set above)
import target_revision  # noqa: E402  (path is set above)

IMAGE_NAME = "CC64S.COM"
TIMEOUT = 900.0
# The command tail is bounded to 143 bytes on the target, so a compile names
# one source and one object and nothing else.
COMMAND_BUDGET = 143
VOLUME_ARGUMENTS = [
    "--vol-lba", "512", "--vol-sectors", "2880", "--sector-size", "512",
    "--kernel-lba", "16", "--kernel-sectors", "256",
]
INCLUDE_PATHS = ["-I", "include/target", "-I", "include/cc64", "-I", "src"]


def include_tree() -> list[pathlib.Path]:
    """Every header a corpus program can need, staged under its base name."""
    headers = sorted((ROOT / "include/target").glob("*.h"))
    headers += [ROOT / "include/cc64/dos64.h"]
    headers += sorted((ROOT / "src").rglob("*.h"))
    names = [path.name.upper() for path in headers]
    duplicates = {name for name in names if names.count(name) > 1}
    if duplicates:
        raise SystemExit(f"self-host corpus cannot flatten {sorted(duplicates)}")
    return headers


def production_sources() -> list[pathlib.Path]:
    return sorted(
        path for path in (ROOT / "src").rglob("*.c")
        if "runtime" not in path.parts and "selfhost" not in path.parts
    )


def library_sources() -> list[pathlib.Path]:
    return sorted((ROOT / "src/runtime").glob("target_*.c"))


def stage_name(index: int) -> str:
    """A one-letter staged base name, so every command stays inside the tail.

    The tail the target accepts is a fixed 143 bytes, and a compile names the
    source, the object, and the output. One letter plus a one-character
    extension leaves room for the longest case name, so a case is staged under
    a letter and the mapping is derived only from the case list, which means a
    given case is always staged and read back under the same name.
    """
    if index >= 26:
        raise SystemExit(f"self-host corpus has no staged name for item {index}")
    return chr(ord("A") + index)


def build_image(work: pathlib.Path) -> tuple[pathlib.Path, list[pathlib.Path]]:
    """Compile every unit and the target library, then link the compiler."""
    sources = production_sources() + library_sources()
    if not sources:
        raise SystemExit("self-host corpus found no sources to build")
    objects = []
    for source in sources:
        output = work / f"{source.stem}.cc64o"
        command = [str(ROOT / "cc64"), *INCLUDE_PATHS, "-c", str(source),
                   "-o", str(output)]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        if result.returncode != 0:
            raise SystemExit(f"self-host corpus cannot compile {source.name}:\n"
                             f"{result.stdout}{result.stderr}")
        objects.append(output)
    image = work / "compiler.mz64"
    command = [str(ROOT / "cc64"), "--link", "--format", "mz64"]
    command += [str(path) for path in objects]
    command += ["-o", str(image)]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"self-host corpus cannot link the compiler image:\n"
                         f"{result.stdout}{result.stderr}")
    return image, objects


def check_volume(image: pathlib.Path, label: str) -> None:
    result = subprocess.run(
        ["python3", str(TARGET / "tools/check_volume_clean.py"),
         *VOLUME_ARGUMENTS, str(image)],
        cwd=TARGET, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        detail = (result.stdout + result.stderr).strip()
        raise SystemExit(f"target volume cleanliness check failed ({label}): "
                         f"{detail}")


def write_manifest(path: pathlib.Path, entries: list[tuple[str, pathlib.Path]]) -> None:
    path.write_text("\n".join(f"{name} {source}" for name, source in entries) + "\n",
                    encoding="utf-8")


def embed_manifest(disk: pathlib.Path, manifest: pathlib.Path) -> None:
    subprocess.run(["python3", str(ROOT / "tests/embed_fat12.py"), str(disk),
                    "--manifest", str(manifest)], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL)


def embed_file(disk: pathlib.Path, payload: pathlib.Path, name: str) -> None:
    subprocess.run(["python3", str(ROOT / "tests/embed_fat12.py"), str(disk),
                    str(payload), name], cwd=ROOT, check=True,
                   stdout=subprocess.DEVNULL)


def extract(disk: pathlib.Path, name: str, output: pathlib.Path) -> bytes:
    result = subprocess.run(["python3", str(ROOT / "tests/extract_fat12.py"),
                             str(disk), name, "-o", str(output)],
                            cwd=ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"cannot read {name} back off the volume:\n"
                         f"{result.stdout}{result.stderr}")
    return output.read_bytes()


def boot(disk: pathlib.Path, command: str, marker: str = "Exit 0",
         timeout: float = TIMEOUT) -> str:
    """Run one command on the target and wait for its result in the log.

    The target shell stays resident after a program returns, so the run is
    complete when the transcript shows the result followed by another prompt.
    """
    if len(command) + 1 > COMMAND_BUDGET:
        raise SystemExit(f"self-host corpus command exceeds the target tail "
                         f"budget: {command!r}")
    transcript = disk.with_suffix(".log")
    process = None
    with transcript.open("w", encoding="utf-8") as stream:
        process = subprocess.Popen(
            ["qemu-system-x86_64", "-drive", f"file={disk},format=raw",
             "-serial", "stdio", "-display", "none"],
            stdin=subprocess.PIPE, stdout=stream, stderr=subprocess.STDOUT,
            text=True)
        process.stdin.write(command + "\n")
        process.stdin.close()
        deadline = time.monotonic() + timeout
        complete = False
        while time.monotonic() < deadline:
            text = transcript.read_text(encoding="utf-8", errors="replace")
            if marker in text and "A> " in text[text.index(marker) + len(marker):]:
                complete = True
                break
            if process.poll() is not None:
                break
            time.sleep(0.1)
        if process.poll() is None:
            process.kill()
        process.wait()
    text = transcript.read_text(encoding="utf-8", errors="replace")
    if not complete:
        raise SystemExit(f"target run of {command!r} did not reach {marker}:\n"
                         f"{text[-600:]}")
    return text


def host_compile(work: pathlib.Path, source: pathlib.Path,
                 name: str) -> pathlib.Path:
    """What the bootstrap compiler produces from the same source."""
    obj = work / f"host-{name}.cc64o"
    command = [str(ROOT / "cc64"), *INCLUDE_PATHS, "-c", str(source),
               "-o", str(obj)]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"bootstrap compiler rejected corpus case {name}:\n"
                         f"{result.stdout}{result.stderr}")
    return obj


def batches(entries: list, sizes: dict, limit_bytes: int) -> list[list]:
    """Group cases so one volume holds their sources and objects at once."""
    groups: list[list] = []
    current: list = []
    current_bytes = 0
    for entry in entries:
        size = sizes[entry[0]]
        if current and current_bytes + size > limit_bytes:
            groups.append(current)
            current = []
            current_bytes = 0
        current.append(entry)
        current_bytes += size
    if current:
        groups.append(current)
    return groups


def main() -> int:
    if not TARGET.is_dir():
        message = "self-host corpus: skipped (QEMU or target checkout unavailable)"
        if not target_revision.require_emulator(
                f"{tool}", f"{message}"):
            return 0
    target_revision.check(TARGET)
    headers = include_tree()
    cases = corpus.CASES
    with tempfile.TemporaryDirectory(prefix="cc64-self-host-corpus-") as temp:
        work = pathlib.Path(temp)
        image, _ = build_image(work)
        sources = work / "sources"
        sources.mkdir()
        # Every case is written once, under a one-letter staged name that fits
        # the target's command tail, and the bootstrap objects are produced
        # from the very same bytes the target-built compiler will read.
        names: dict[str, str] = {}
        for index, entry in enumerate(cases):
            name = entry[0]
            if name in names:
                raise SystemExit(f"corpus case name {name} is used twice")
            staged = stage_name(index)
            names[name] = staged
            (sources / f"{staged}.C").write_text(entry[1], encoding="utf-8")
        reference = {entry[0]: host_compile(work, sources / f"{names[entry[0]]}.C",
                                           entry[0]) for entry in cases}
        sizes = {entry[0]: path.stat().st_size for entry, path in
                 ((entry, reference[entry[0]]) for entry in cases)}        # The volume holds the compiler image, the headers, the sources, and
        # the objects a batch produces, so a batch is bounded by what is left
        # of the data area with room to spare.
        limit = 200 * 1024
        groups = batches(cases, sizes, limit)
        base = [(IMAGE_NAME, image)]
        base += [(header.name.upper(), header) for header in headers]
        produced: dict[str, bytes] = {}
        for index, group in enumerate(groups, start=1):
            disk = work / f"compile{index}.img"
            shutil.copy2(TARGET / "build/dos64-lean.img", disk)
            check_volume(disk, f"compile batch {index} fresh")
            manifest = work / f"compile{index}.txt"
            tree = list(base)
            for entry in group:
                staged = names[entry[0]]
                tree.append((f"{staged}.C", sources / f"{staged}.C"))
            write_manifest(manifest, tree)
            embed_manifest(disk, manifest)
            check_volume(disk, f"compile batch {index} staged")
            for entry in group:
                staged = names[entry[0]]
                boot(disk, f"CC64S -c {staged}.C -o {staged}.O")
            check_volume(disk, f"compile batch {index} compiled")
            for entry in group:
                staged = names[entry[0]]
                output = work / f"produced-{entry[0]}.cc64o"
                payload = extract(disk, f"{staged}.O", output)
                if payload != reference[entry[0]].read_bytes():
                    raise SystemExit(
                        f"target-built compiler's object for case {entry[0]} "
                        f"differs from the bootstrap object")
                produced[entry[0]] = payload
            check_volume(disk, f"compile batch {index} read back")
            print(f"self-host corpus: batch {index}/{len(groups)} compiled "
                  f"{len(group)} cases identically")

        # The objects the target-built compiler wrote are linked by the
        # target-built linker and the images are run, so a case that compiles
        # to the same bytes but does not work on the target is still caught.
        for index, group in enumerate(groups, start=1):
            disk = work / f"link{index}.img"
            shutil.copy2(TARGET / "build/dos64-lean.img", disk)
            check_volume(disk, f"link batch {index} fresh")
            embed_file(disk, image, IMAGE_NAME)
            for entry in group:
                staged = names[entry[0]]
                embed_file(disk, work / f"produced-{entry[0]}.cc64o", f"{staged}.O")
            check_volume(disk, f"link batch {index} staged")
            for entry in group:
                name, _, image_format, expected, expected_text = entry[:5]
                staged = names[name]
                command = entry[5] if len(entry) > 5 else name
                option = "mz64" if image_format == "mz64" else "com"
                # The compiler process itself returns zero; the case's own
                # exit code belongs to the program it linked.
                boot(disk, f"CC64S --link --format {option} {staged}.O "
                           f"-o {staged}.COM")
                built = extract(disk, f"{staged}.COM", work / f"built-{name}.bin")
                # A case that reads its own name has to be started under that
                # name, so the image is written to a volume with the case's
                # own name rather than the staged letter.
                run_disk = work / f"run-{name}.img"
                shutil.copy2(TARGET / "build/dos64-lean.img", run_disk)
                embed_file(run_disk, work / f"built-{name}.bin", name)
                text = boot(run_disk, command, marker=f"Exit {expected}",
                            timeout=120.0)
                if f"Exit {expected}" not in text:
                    raise SystemExit(f"{name}: target-built image did not return "
                                     f"{expected}")
                if expected_text is not None and expected_text not in text:
                    raise SystemExit(f"{name}: target-built image output lacked "
                                     f"{expected_text!r}: {text[-200:]}")
            check_volume(disk, f"link batch {index} linked")
    print(f"self-host corpus: {len(cases)} conformance cases were compiled, "
          "linked, and run by a compiler the target built, and every object "
          "matched the bootstrap compiler byte for byte")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
