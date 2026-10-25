# PlatformIO pre-script (platformio.ini extra_scripts): defines EDGE_GIT_REV
# for every source file. A new revision changes the build flags, so every
# file is recompiled and the banner can never show a stale revision.
Import("env")  # noqa: F821  (provided by PlatformIO / SCons)

import sys
from pathlib import Path

project = Path(env["PROJECT_DIR"])  # noqa: F821
sys.path.insert(0, str(project / "scripts"))

from git_rev import firmware_revision  # noqa: E402

env.Append(CPPDEFINES=[("EDGE_GIT_REV", env.StringifyMacro(firmware_revision(project)))])  # noqa: F821
