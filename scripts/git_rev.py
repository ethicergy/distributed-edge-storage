#!/usr/bin/env python3
"""Firmware revision embedded in every image (EDGE_GIT_REV, printed in the
boot banner and `stats`).

    <short hash>        the firmware inputs match HEAD
    <short hash>-dirty  src/, include/ or platformio.ini differ from HEAD
                        (modified, staged or new files)
    unknown             not a git checkout

Docs and logs are deliberately ignored: a console capture appends to tracked
logs while images are built, which must neither mark the image dirty nor
change the build flags (and so force a rebuild).

    scripts/git_rev.py [repo]     print the revision
"""

import subprocess
import sys
from pathlib import Path

FIRMWARE_INPUTS = ("src", "include", "platformio.ini")


def _git(repo, *args) -> str:
    return subprocess.run(
        ["git", "-C", str(repo), *args], check=True, capture_output=True, text=True
    ).stdout.strip()


def firmware_revision(repo) -> str:
    try:
        head = _git(repo, "rev-parse", "--short=7", "HEAD")
        changes = _git(repo, "status", "--porcelain", "--untracked-files=all", "--", *FIRMWARE_INPUTS)
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "unknown"
    return head + ("-dirty" if changes else "")


if __name__ == "__main__":
    print(firmware_revision(Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent))
