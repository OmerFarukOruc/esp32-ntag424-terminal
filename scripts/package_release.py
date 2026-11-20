#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
#
# SPDX-License-Identifier: Apache-2.0
"""Package a built firmware as a release candidate.

Usage: package_release.py LABEL BUILD_DIR OUT_DIR [--expected-version VERSION]

Writes esp32-ntag424-terminal-LABEL-source.tar.gz, esp32-ntag424-terminal-LABEL.bin
and esp32-ntag424-terminal.elf to the new directory OUT_DIR. The source archive
holds every file committed in the repository and its checked-out submodules, as
stored in Git, the managed components, the resolved sdkconfig and a version.txt
with the version the firmware was built with, so it rebuilds without network
access and gives the firmware the same version.
"""

import argparse
import gzip
import io
import json
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

from release_files import APP_VERSION_BYTES, ELF, LABEL_PATTERN, image, source_archive, source_prefix

REQUIRED_MEMBERS = ("libs/ESP32-PN532/pn532.c", "libs/ESP32-PN532/LICENSE", "dependencies.lock")
GITLINK = "160000"
SYMLINK = "120000"
EXECUTABLE = "100755"


class PackageError(Exception):
    pass


def project_version(build_dir: Path) -> tuple[str, Path]:
    description = json.loads((build_dir / "project_description.json").read_text())
    return description["project_version"], Path(description["config_file"])


def git(repo: Path, *args: str, stdin: bytes | None = None) -> bytes:
    return subprocess.run(["git", "-C", str(repo), *args], input=stdin, check=True, capture_output=True).stdout


def require_committed(repo: Path) -> None:
    if git(repo, "status", "--porcelain", "--untracked-files=no", "--ignore-submodules=none").strip():
        raise PackageError("tracked files differ from the commit; commit them before packaging")


def read_blobs(repo: Path, entries: list[tuple[str, str, str]]) -> dict[str, tuple[bytes, int]]:
    out = git(repo, "cat-file", "--batch", stdin="".join(f"{sha}\n" for _, sha, _ in entries).encode())
    files = {}
    at = 0
    for mode, sha, path in entries:
        header_end = out.index(b"\n", at)
        name, kind, size = out[at:header_end].decode().split()
        if name != sha or kind != "blob":
            raise PackageError(f"git returned {name} {kind} for {path}")
        data_end = header_end + 1 + int(size)
        files[path] = (out[header_end + 1 : data_end], 0o755 if mode == EXECUTABLE else 0o644)
        at = data_end + 1
    return files


def committed_files(repo: Path, prefix: str = "") -> dict[str, tuple[bytes, int]]:
    entries = []
    files = {}
    for line in git(repo, "ls-files", "-z", "-s").decode().split("\0"):
        if not line:
            continue
        meta, path = line.split("\t", 1)
        mode, sha, _ = meta.split()
        if mode == SYMLINK:
            raise PackageError(f"{prefix}{path} is a symlink; the archive holds regular files only")
        if mode == GITLINK:
            if (repo / path / ".git").exists():
                files.update(committed_files(repo / path, f"{prefix}{path}/"))
            else:
                print(f"package_release: leaving out {prefix}{path}, a submodule that is not checked out")
            continue
        entries.append((mode, sha, path))
    files.update({prefix + path: blob for path, blob in read_blobs(repo, entries).items()})
    return files


def source_files(repo: Path, sdkconfig: Path, version: str) -> dict[str, tuple[bytes, int]]:
    files = committed_files(repo)
    managed = repo / "managed_components"
    if not managed.is_dir():
        raise PackageError("managed_components is missing; build the firmware first")
    for p in sorted(managed.rglob("*")):
        if p.is_symlink():
            raise PackageError(f"{p.relative_to(repo)} is a symlink; the archive holds regular files only")
        if p.is_file():
            files[str(p.relative_to(repo))] = (p.read_bytes(), 0o755 if p.stat().st_mode & 0o111 else 0o644)
    missing = [p for p in REQUIRED_MEMBERS if p not in files]
    if missing:
        raise PackageError(f"missing from the source: {', '.join(missing)}")
    files["version.txt"] = (f"{version}\n".encode(), 0o644)
    files["sdkconfig"] = (sdkconfig.read_bytes(), 0o644)
    return files


def write_source_archive(files: dict[str, tuple[bytes, int]], prefix: str, mtime: int, out: Path) -> None:
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w", format=tarfile.PAX_FORMAT) as tar:
        for name in sorted(files):
            data, mode = files[name]
            info = tarfile.TarInfo(f"{prefix}/{name}")
            info.size = len(data)
            info.mode = mode
            info.mtime = mtime
            tar.addfile(info, io.BytesIO(data))
    with open(out, "wb") as raw, gzip.GzipFile(fileobj=raw, mode="wb", mtime=0, filename="") as gz:
        gz.write(buffer.getvalue())


def package(label: str, build_dir: Path, out_dir: Path, expected: str | None) -> None:
    if not LABEL_PATTERN.fullmatch(label):
        raise PackageError(f"label {label!r} must be 1 to 31 characters from [A-Za-z0-9._+-]")
    if out_dir.exists():
        raise PackageError(f"{out_dir} exists already")
    repo = Path(git(build_dir, "rev-parse", "--show-toplevel").decode().strip())
    version, sdkconfig = project_version(build_dir)
    if expected is not None and version != expected:
        raise PackageError(f"the firmware was built as {version!r}, not {expected!r}")
    require_committed(repo)
    subprocess.run(["idf.py", "-C", str(repo), "-B", str(build_dir), "merge-bin", "-o", image(label)], check=True)
    if project_version(build_dir) != (version, sdkconfig):
        raise PackageError("merge-bin rebuilt the firmware with another version or sdkconfig")
    require_committed(repo)
    built = version[:APP_VERSION_BYTES]
    files = source_files(repo, sdkconfig, built)
    mtime = int(git(repo, "log", "-1", "--format=%ct").decode())
    out_dir.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out_dir.parent) as staging:
        stage = Path(staging) / out_dir.name
        stage.mkdir()
        write_source_archive(files, source_prefix(label), mtime, stage / source_archive(label))
        shutil.copyfile(build_dir / image(label), stage / image(label))
        shutil.copyfile(build_dir / ELF, stage / ELF)
        stage.rename(out_dir)
    print(f"Packaged {built} as {label} in {out_dir}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Package a built firmware as a release candidate.")
    parser.add_argument("label")
    parser.add_argument("build_dir", type=Path)
    parser.add_argument("out_dir", type=Path)
    parser.add_argument("--expected-version")
    args = parser.parse_args()
    try:
        package(args.label, args.build_dir.resolve(), args.out_dir.resolve(), args.expected_version)
    except (PackageError, OSError, ValueError, KeyError, subprocess.CalledProcessError) as e:
        print(f"package_release: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
