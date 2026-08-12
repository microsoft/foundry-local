# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.

"""Focused unit tests for pack.py staging behaviour.

The ``cpp_pack_nuget`` stage runs these tests before pack.py to guard the RID
table and ``runtimes/<rid>/native`` staging layout without requiring nuget.exe or
real build artifacts.

Run manually with:
    python -m unittest test_pack        (from sdk_v2/cpp/nuget/)
    python test_pack.py                 (direct invocation)
"""

from __future__ import annotations

import argparse
import sys
import tempfile
import unittest
from pathlib import Path

# Make sure pack module can be imported from any working directory.
sys.path.insert(0, str(Path(__file__).parent))
import pack  # noqa: E402  (side-effect-free until main() is called)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _fake_args(**overrides) -> argparse.Namespace:
    """Return a minimal Namespace that satisfies pack.stage()."""
    defaults: dict = {
        "version": "0.0.0-test",
        "ort_version": "1.0.0",
        "genai_version": "1.0.0",
        "package_id": "Test.Package",
        "nuget_path": "nuget",
        "output_dir": None,   # filled in by callers
        "staging_dir": None,
    }
    for arg_name in pack.RIDS:
        defaults[arg_name] = None
    defaults.update(overrides)
    return argparse.Namespace(**defaults)


def _make_fake_artifact(root: Path, rid_arg: str) -> Path:
    """Create a minimal fake artefact directory for *rid_arg* and return its path."""
    _rid, lib_name = pack.RIDS[rid_arg]
    artifact_dir = root / rid_arg
    artifact_dir.mkdir(parents=True, exist_ok=True)
    (artifact_dir / lib_name).write_bytes(b"fake-binary")
    return artifact_dir


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

class TestRidsMapping(unittest.TestCase):
    """Verify that the RIDS table contains all expected platforms."""

    def test_all_expected_rids_present(self):
        expected_arg_names = {"win_x64", "win_arm64", "linux_x64", "linux_arm64", "osx_arm64"}
        self.assertEqual(set(pack.RIDS.keys()), expected_arg_names,
                         "RIDS table must contain exactly the five supported platforms")

    def test_linux_arm64_maps_to_correct_rid_and_lib(self):
        rid, lib = pack.RIDS["linux_arm64"]
        self.assertEqual(rid, "linux-arm64",
                         "linux_arm64 must map to NuGet RID 'linux-arm64'")
        self.assertEqual(lib, "libfoundry_local.so",
                         "linux_arm64 primary library must be 'libfoundry_local.so'")

    def test_linux_x64_unchanged(self):
        rid, lib = pack.RIDS["linux_x64"]
        self.assertEqual(rid, "linux-x64")
        self.assertEqual(lib, "libfoundry_local.so")

    def test_windows_rids_use_dll(self):
        for arg in ("win_x64", "win_arm64"):
            with self.subTest(arg=arg):
                _rid, lib = pack.RIDS[arg]
                self.assertEqual(lib, "foundry_local.dll")

    def test_osx_arm64_uses_dylib(self):
        _rid, lib = pack.RIDS["osx_arm64"]
        self.assertEqual(lib, "libfoundry_local.dylib")


class TestStagingLayout(unittest.TestCase):
    """Verify that stage() produces the expected runtimes/<rid>/native/ layout."""

    def _run_stage(self, rid_arg: str) -> tuple[set[str], int]:
        """
        Run pack.stage() with a single platform artifact.
        Returns (layout, rid_count) where ``layout`` is the set of staged file
        paths (relative to the staging dir, posix-style) and ``rid_count`` is the
        number of RIDs pack.stage() staged.
        """
        with tempfile.TemporaryDirectory() as tmp_str:
            tmp = Path(tmp_str)
            artifact_dir = _make_fake_artifact(tmp / "artifacts", rid_arg)
            output_dir = tmp / "out"
            staging_dir = tmp / "_staging"
            staging_dir.mkdir()

            # pack.stage() needs nuspec + build/ + buildTransitive/ to exist.
            # Point SCRIPT_DIR-relative paths to real files only if they exist;
            # if not (CI artefacts not present), create stubs so staging doesn't fail.
            # We monkey-patch SCRIPT_DIR temporarily to a minimal fake tree.
            fake_script_dir = tmp / "nuget_stub"
            fake_script_dir.mkdir()
            nuspec_stub = fake_script_dir / "Microsoft.AI.Foundry.Local.Runtime.nuspec"
            nuspec_stub.write_text('<?xml version="1.0"?><package/>', encoding="utf-8")
            (fake_script_dir / "build").mkdir()
            (fake_script_dir / "buildTransitive").mkdir()
            for document_name in ("README.md", "Privacy.md", "ThirdPartyNotices.txt"):
                (fake_script_dir / document_name).write_text(document_name, encoding="utf-8")

            # REPO_ROOT = SCRIPT_DIR.parent; make include/ optional
            (fake_script_dir.parent / "include").mkdir(exist_ok=True)
            (fake_script_dir.parent / "LICENSE.txt").write_text("MIT", encoding="utf-8")

            original_script_dir = pack.SCRIPT_DIR
            original_repo_root = pack.REPO_ROOT
            try:
                pack.SCRIPT_DIR = fake_script_dir
                pack.REPO_ROOT = fake_script_dir.parent

                args = _fake_args(output_dir=output_dir)
                setattr(args, rid_arg, artifact_dir)

                rid_count = pack.stage(args, staging_dir)
            finally:
                pack.SCRIPT_DIR = original_script_dir
                pack.REPO_ROOT = original_repo_root

            # Snapshot the layout before the tempdir is deleted.
            layout = {
                p.relative_to(staging_dir).as_posix()
                for p in staging_dir.rglob("*")
                if p.is_file()
            }
            return layout, rid_count

    def _check_native_file_present(self, layout: set[str], rid: str, lib: str):
        expected = f"runtimes/{rid}/native/{lib}"
        self.assertIn(expected, layout,
                      f"Expected '{expected}' in staging layout.\nActual layout:\n  "
                      + "\n  ".join(sorted(layout)))

    def test_linux_arm64_staged_correctly(self):
        layout, rid_count = self._run_stage("linux_arm64")
        self.assertEqual(rid_count, 1)
        self._check_native_file_present(layout, "linux-arm64", "libfoundry_local.so")

    def test_windows_import_library_is_staged(self):
        with tempfile.TemporaryDirectory() as tmp_str:
            tmp = Path(tmp_str)
            artifact_dir = _make_fake_artifact(tmp / "artifacts", "win_x64")
            (artifact_dir / "foundry_local.lib").write_bytes(b"fake-import-library")
            output_dir = tmp / "out"
            staging_dir = tmp / "_staging"
            staging_dir.mkdir()

            fake_script_dir = tmp / "nuget_stub"
            fake_script_dir.mkdir()
            (fake_script_dir / "Microsoft.AI.Foundry.Local.Runtime.nuspec").write_text(
                '<?xml version="1.0"?><package/>', encoding="utf-8"
            )
            (fake_script_dir / "build").mkdir()
            (fake_script_dir / "buildTransitive").mkdir()
            (fake_script_dir.parent / "include").mkdir(exist_ok=True)
            (fake_script_dir.parent / "LICENSE.txt").write_text("MIT", encoding="utf-8")

            original_script_dir = pack.SCRIPT_DIR
            original_repo_root = pack.REPO_ROOT
            try:
                pack.SCRIPT_DIR = fake_script_dir
                pack.REPO_ROOT = fake_script_dir.parent
                args = _fake_args(output_dir=output_dir, win_x64=artifact_dir)
                pack.stage(args, staging_dir)
            finally:
                pack.SCRIPT_DIR = original_script_dir
                pack.REPO_ROOT = original_repo_root

            self.assertTrue(
                (staging_dir / "runtimes" / "win-x64" / "native" / "foundry_local.lib").is_file()
            )

    def test_package_documents_are_staged_at_root(self):
        layout, rid_count = self._run_stage("linux_x64")
        self.assertEqual(rid_count, 1)
        for document_name in ("README.md", "Privacy.md", "ThirdPartyNotices.txt"):
            with self.subTest(document=document_name):
                self.assertIn(document_name, layout)

    def test_all_platforms_staged_together(self):
        """Staging all five platforms at once yields rid_count == 5."""
        with tempfile.TemporaryDirectory() as tmp_str:
            tmp = Path(tmp_str)

            fake_script_dir = tmp / "nuget_stub"
            fake_script_dir.mkdir()
            nuspec_stub = fake_script_dir / "Microsoft.AI.Foundry.Local.Runtime.nuspec"
            nuspec_stub.write_text('<?xml version="1.0"?><package/>', encoding="utf-8")
            (fake_script_dir / "build").mkdir()
            (fake_script_dir / "buildTransitive").mkdir()
            (fake_script_dir.parent / "include").mkdir(exist_ok=True)
            (fake_script_dir.parent / "LICENSE.txt").write_text("MIT", encoding="utf-8")

            artifact_dirs = {
                rid_arg: _make_fake_artifact(tmp / "artifacts", rid_arg)
                for rid_arg in pack.RIDS
            }

            staging_dir = tmp / "_staging"
            staging_dir.mkdir()
            output_dir = tmp / "out"

            args = _fake_args(output_dir=output_dir)
            for rid_arg, artifact_dir in artifact_dirs.items():
                setattr(args, rid_arg, artifact_dir)

            original_script_dir = pack.SCRIPT_DIR
            original_repo_root = pack.REPO_ROOT
            try:
                pack.SCRIPT_DIR = fake_script_dir
                pack.REPO_ROOT = fake_script_dir.parent
                rid_count = pack.stage(args, staging_dir)
            finally:
                pack.SCRIPT_DIR = original_script_dir
                pack.REPO_ROOT = original_repo_root

            self.assertEqual(rid_count, 5,
                             "All five RIDs should be staged when all --<rid> args are provided")

            layout = {
                p.relative_to(staging_dir).as_posix()
                for p in staging_dir.rglob("*")
                if p.is_file()
            }
            for rid_arg, (rid, lib) in pack.RIDS.items():
                with self.subTest(rid=rid):
                    self._check_native_file_present(layout, rid, lib)

    def test_zero_rids_returns_zero(self):
        """Omitting all --<rid> args should return 0 (main() would then exit(1))."""
        with tempfile.TemporaryDirectory() as tmp_str:
            tmp = Path(tmp_str)

            fake_script_dir = tmp / "nuget_stub"
            fake_script_dir.mkdir()
            nuspec_stub = fake_script_dir / "Microsoft.AI.Foundry.Local.Runtime.nuspec"
            nuspec_stub.write_text('<?xml version="1.0"?><package/>', encoding="utf-8")
            (fake_script_dir / "build").mkdir()
            (fake_script_dir / "buildTransitive").mkdir()
            (fake_script_dir.parent / "include").mkdir(exist_ok=True)
            (fake_script_dir.parent / "LICENSE.txt").write_text("MIT", encoding="utf-8")

            staging_dir = tmp / "_staging"
            staging_dir.mkdir()
            output_dir = tmp / "out"

            args = _fake_args(output_dir=output_dir)  # all rid args stay None

            original_script_dir = pack.SCRIPT_DIR
            original_repo_root = pack.REPO_ROOT
            try:
                pack.SCRIPT_DIR = fake_script_dir
                pack.REPO_ROOT = fake_script_dir.parent
                rid_count = pack.stage(args, staging_dir)
            finally:
                pack.SCRIPT_DIR = original_script_dir
                pack.REPO_ROOT = original_repo_root

            self.assertEqual(rid_count, 0)


class TestArgParser(unittest.TestCase):
    """Verify that the argument parser accepts --linux_arm64."""

    def test_linux_arm64_arg_accepted(self):
        original_argv = sys.argv
        try:
            sys.argv = [
                "pack.py",
                "--version", "1.0.0",
                "--ort_version", "1.0.0",
                "--genai_version", "1.0.0",
                "--linux_arm64", "/tmp/linux-arm64",
            ]
            args = pack._parse_args()
        finally:
            sys.argv = original_argv

        self.assertIsNotNone(args.linux_arm64,
                             "--linux_arm64 should be parsed and non-None when provided")
        # Compare as Path objects so Windows/POSIX separator differences don't matter.
        self.assertEqual(args.linux_arm64, Path("/tmp/linux-arm64"))

    def test_all_rid_args_registered(self):
        original_argv = sys.argv
        try:
            sys.argv = [
                "pack.py",
                "--version", "1.0.0",
                "--ort_version", "1.0.0",
                "--genai_version", "1.0.0",
            ]
            args = pack._parse_args()
        finally:
            sys.argv = original_argv

        for rid_arg in pack.RIDS:
            self.assertTrue(hasattr(args, rid_arg),
                            f"Argument namespace missing attribute '{rid_arg}'")


if __name__ == "__main__":
    unittest.main()
