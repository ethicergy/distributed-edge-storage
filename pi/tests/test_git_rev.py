"""scripts/git_rev.py: the firmware revision string embedded in every image
(banner and `stats`): short commit hash, "-dirty" only when the firmware
inputs (src/, include/, platformio.ini) have uncommitted changes."""

import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SCRIPT = os.path.join(ROOT, "scripts", "git_rev.py")

spec = importlib.util.spec_from_file_location("git_rev", SCRIPT)
git_rev = importlib.util.module_from_spec(spec)
sys.modules["git_rev"] = git_rev
spec.loader.exec_module(git_rev)


def git(repo, *args):
    return subprocess.run(
        ["git", "-C", repo, *args], check=True, capture_output=True, text=True
    ).stdout.strip()


class FirmwareRevisionTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = self.tmp.name
        git(self.repo, "init", "-q")
        git(self.repo, "config", "user.email", "test@example.com")
        git(self.repo, "config", "user.name", "test")
        for path, text in (("src/main.cpp", "int x;\n"), ("include/config.h", "#pragma once\n"),
                           ("platformio.ini", "[env]\n"), ("docs/log.md", "log\n")):
            os.makedirs(os.path.join(self.repo, os.path.dirname(path)), exist_ok=True)
            with open(os.path.join(self.repo, path), "w") as f:
                f.write(text)
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "init")
        self.head = git(self.repo, "rev-parse", "--short=7", "HEAD")

    def tearDown(self):
        self.tmp.cleanup()

    def append(self, path, text="more\n"):
        with open(os.path.join(self.repo, path), "a") as f:
            f.write(text)

    def test_clean_tree_is_the_short_hash(self):
        self.assertEqual(git_rev.firmware_revision(self.repo), self.head)
        self.assertEqual(len(self.head), 7)

    def test_changed_firmware_input_is_dirty(self):
        for path in ("src/main.cpp", "include/config.h", "platformio.ini"):
            with self.subTest(path=path):
                git(self.repo, "checkout", "-q", "--", ".")
                self.append(path)
                self.assertEqual(git_rev.firmware_revision(self.repo), self.head + "-dirty")

    def test_staged_firmware_change_is_dirty(self):
        self.append("src/main.cpp")
        git(self.repo, "add", "src/main.cpp")
        self.assertEqual(git_rev.firmware_revision(self.repo), self.head + "-dirty")

    def test_docs_and_logs_do_not_make_it_dirty(self):
        # A console capture appends to tracked logs while images are built.
        self.append("docs/log.md")
        with open(os.path.join(self.repo, "docs", "new.log"), "w") as f:
            f.write("x\n")
        self.assertEqual(git_rev.firmware_revision(self.repo), self.head)

    def test_new_untracked_source_file_is_dirty(self):
        with open(os.path.join(self.repo, "src", "extra.cpp"), "w") as f:
            f.write("int y;\n")
        self.assertEqual(git_rev.firmware_revision(self.repo), self.head + "-dirty")

    def test_not_a_repository_is_unknown(self):
        with tempfile.TemporaryDirectory() as empty:
            self.assertEqual(git_rev.firmware_revision(empty), "unknown")


if __name__ == "__main__":
    unittest.main()
