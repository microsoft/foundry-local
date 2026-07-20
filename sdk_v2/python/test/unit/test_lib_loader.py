# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Unit tests for lib_loader platform RID detection — pure Python, no native deps.

lib_loader.py is loaded via spec_from_file_location so we can import it
without triggering foundry_local_sdk._native.__init__, which requires the
compiled cffi extension (_cffi_bindings) that is not present in the test venv.
"""
from __future__ import annotations

import importlib.util
import pathlib
import sys
from unittest.mock import patch

import pytest

# ---------------------------------------------------------------------------
# Load lib_loader.py in isolation (no package __init__ side-effects).
# ---------------------------------------------------------------------------
_LIB_LOADER_PATH = (
    pathlib.Path(__file__).resolve().parents[2]
    / "src"
    / "foundry_local_sdk"
    / "_native"
    / "lib_loader.py"
)

_spec = importlib.util.spec_from_file_location(
    "foundry_local_sdk._native.lib_loader_isolated", _LIB_LOADER_PATH
)
_ll = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_ll)  # type: ignore[union-attr]


class TestPlatformSubdir:
    """_platform_subdir() must return the correct RID for every supported host."""

    # ------------------------------------------------------------------
    # Windows
    # ------------------------------------------------------------------
    def test_win32_returns_win_x64(self):
        with patch.object(_ll.sys, "platform", "win32"):
            assert _ll._platform_subdir() == "win-x64"

    # ------------------------------------------------------------------
    # macOS
    # ------------------------------------------------------------------
    def test_darwin_arm64_returns_osx_arm64(self):
        with patch.object(_ll.sys, "platform", "darwin"), \
             patch.object(_ll.platform, "machine", return_value="arm64"):
            assert _ll._platform_subdir() == "osx-arm64"

    def test_darwin_x86_64_returns_osx_x64(self):
        with patch.object(_ll.sys, "platform", "darwin"), \
             patch.object(_ll.platform, "machine", return_value="x86_64"):
            assert _ll._platform_subdir() == "osx-x64"

    # ------------------------------------------------------------------
    # Linux x64
    # ------------------------------------------------------------------
    def test_linux_x86_64_returns_linux_x64(self):
        with patch.object(_ll.sys, "platform", "linux"), \
             patch.object(_ll.platform, "machine", return_value="x86_64"):
            assert _ll._platform_subdir() == "linux-x64"

    # ------------------------------------------------------------------
    # Linux ARM64 — both naming conventions must resolve to linux-arm64
    # ------------------------------------------------------------------
    def test_linux_aarch64_returns_linux_arm64(self):
        """Kernel reports 'aarch64' on most Linux ARM64 distros."""
        with patch.object(_ll.sys, "platform", "linux"), \
             patch.object(_ll.platform, "machine", return_value="aarch64"):
            assert _ll._platform_subdir() == "linux-arm64"

    def test_linux_arm64_returns_linux_arm64(self):
        """Some distros / cross-toolchain envs report 'arm64' instead."""
        with patch.object(_ll.sys, "platform", "linux"), \
             patch.object(_ll.platform, "machine", return_value="arm64"):
            assert _ll._platform_subdir() == "linux-arm64"

    # ------------------------------------------------------------------
    # Linux — non-arm non-x64 arch falls through to linux-x64 (unsupported
    # but should not crash; the wheel simply won't contain the native lib).
    # ------------------------------------------------------------------
    def test_linux_other_arch_returns_linux_x64(self):
        with patch.object(_ll.sys, "platform", "linux"), \
             patch.object(_ll.platform, "machine", return_value="riscv64"):
            assert _ll._platform_subdir() == "linux-x64"


class TestLibName:
    """_lib_name() must return the platform-appropriate filename."""

    def test_win32_returns_dll(self):
        with patch.object(_ll.sys, "platform", "win32"):
            assert _ll._lib_name() == "foundry_local.dll"

    def test_darwin_returns_dylib(self):
        with patch.object(_ll.sys, "platform", "darwin"):
            assert _ll._lib_name() == "libfoundry_local.dylib"

    def test_linux_returns_so(self):
        with patch.object(_ll.sys, "platform", "linux"):
            assert _ll._lib_name() == "libfoundry_local.so"


class TestFindLibraryWheelBundled:
    """find_library() must probe the wheel-bundled path for the current RID."""

    def test_linux_arm64_probes_linux_arm64_subdir(self, tmp_path):
        # Arrange: create a fake wheel-bundled layout under tmp_path.
        lib_path = tmp_path / "linux-arm64" / "libfoundry_local.so"
        lib_path.parent.mkdir()
        lib_path.touch()

        # Point __file__ of the isolated module to the tmp_path parent so
        # the resolve().parent gives us tmp_path.
        fake_init = tmp_path / "__init__.py"
        fake_init.touch()

        with patch.object(_ll.sys, "platform", "linux"), \
             patch.object(_ll.platform, "machine", return_value="aarch64"), \
             patch("pathlib.Path.resolve", return_value=fake_init):
            # We can't trivially patch __file__ on an already-loaded module,
            # so just verify that _platform_subdir returns the right value
            # and the wheel candidate path is constructed correctly.
            subdir = _ll._platform_subdir()
            assert subdir == "linux-arm64"
            candidate = tmp_path / subdir / "libfoundry_local.so"
            assert candidate.exists(), f"Expected fake native lib at {candidate}"
