#!/usr/bin/env python3
"""Apply clang-format to staged C/C++ blobs without staging worktree edits."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

FORMATTABLE_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
}


def git(*args: str, input_bytes: bytes | None = None) -> bytes:
    return subprocess.run(
        ["git", *args],
        input=input_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    ).stdout


def main() -> int:
    clang_format = shutil.which("clang-format")
    if clang_format is None:
        print("pre-commit: clang-format is required but was not found in PATH", file=sys.stderr)
        return 1

    repo_root = Path(os.fsdecode(git("rev-parse", "--show-toplevel").strip()))
    os.chdir(repo_root)
    staged_paths = git(
        "diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z"
    ).split(b"\0")

    changed = False
    for path_bytes in filter(None, staged_paths):
        path = os.fsdecode(path_bytes)
        if Path(path).suffix.lower() not in FORMATTABLE_SUFFIXES:
            continue

        entries = git("ls-files", "--stage", "-z", "--", path).split(b"\0")
        entry = next((item for item in entries if item), None)
        if entry is None:
            continue
        metadata, _ = entry.split(b"\t", 1)
        mode_bytes, blob_bytes, stage_bytes = metadata.split()
        if stage_bytes != b"0":
            print(f"pre-commit: resolve merge conflicts before formatting {path}", file=sys.stderr)
            return 1
        if not mode_bytes.startswith(b"100"):
            continue

        original = git("cat-file", "blob", blob_bytes.decode("ascii"))
        formatted = subprocess.run(
            [clang_format, "--style=file", f"--assume-filename={path}"],
            input=original,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if formatted.returncode != 0:
            sys.stderr.buffer.write(formatted.stderr)
            return formatted.returncode
        if formatted.stdout == original:
            continue

        new_blob = git("hash-object", "-w", "--stdin", input_bytes=formatted.stdout).strip().decode("ascii")
        git("update-index", "--cacheinfo", f"{mode_bytes.decode('ascii')},{new_blob},{path}")

        worktree_file = repo_root / path
        try:
            if worktree_file.is_file() and not worktree_file.is_symlink() and worktree_file.read_bytes() == original:
                worktree_file.write_bytes(formatted.stdout)
        except OSError as exc:
            print(f"pre-commit: could not update working copy of {path}: {exc}", file=sys.stderr)

        print(f"pre-commit: formatted staged {path}")
        changed = True

    if changed:
        print("pre-commit: staged formatting applied with .clang-format")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
