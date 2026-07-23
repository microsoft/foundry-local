# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Tests for the custom Python build backend."""
from __future__ import annotations

import sys
from pathlib import Path


_PYTHON_PROJECT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_PYTHON_PROJECT))

import _build_backend  # noqa: E402


def test_auto_plat_name_for_staged_macos_arm64(monkeypatch, tmp_path):
    native_dir = tmp_path / "_native"
    (native_dir / "osx-arm64").mkdir(parents=True)
    (native_dir / "osx-arm64" / "libfoundry_local.dylib").write_bytes(b"fake")
    monkeypatch.setattr(_build_backend, "_NATIVE_DIR", native_dir)

    settings = _build_backend._with_auto_plat_name(None)

    assert settings == {"--build-option": ["--plat-name=macosx_11_0_arm64"]}


def test_auto_plat_name_preserves_existing_build_options(monkeypatch, tmp_path):
    native_dir = tmp_path / "_native"
    (native_dir / "osx-arm64").mkdir(parents=True)
    (native_dir / "osx-arm64" / "libfoundry_local.dylib").write_bytes(b"fake")
    monkeypatch.setattr(_build_backend, "_NATIVE_DIR", native_dir)

    settings = _build_backend._with_auto_plat_name({"--build-option": ["--foo"]})

    assert settings == {"--build-option": ["--foo", "--plat-name=macosx_11_0_arm64"]}


def test_auto_plat_name_respects_explicit_plat_name(monkeypatch, tmp_path):
    native_dir = tmp_path / "_native"
    (native_dir / "osx-arm64").mkdir(parents=True)
    (native_dir / "osx-arm64" / "libfoundry_local.dylib").write_bytes(b"fake")
    monkeypatch.setattr(_build_backend, "_NATIVE_DIR", native_dir)
    original = {"--build-option": ["--plat-name=macosx_15_0_universal2"]}

    settings = _build_backend._with_auto_plat_name(original)

    assert settings is original


def test_auto_plat_name_not_added_without_macos_arm64_payload(monkeypatch, tmp_path):
    monkeypatch.setattr(_build_backend, "_NATIVE_DIR", tmp_path / "_native")

    assert _build_backend._with_auto_plat_name(None) is None


def test_auto_plat_name_not_added_with_multiple_payloads(monkeypatch, tmp_path):
    native_dir = tmp_path / "_native"
    (native_dir / "osx-arm64").mkdir(parents=True)
    (native_dir / "osx-arm64" / "libfoundry_local.dylib").write_bytes(b"fake")
    (native_dir / "linux-x64").mkdir()
    (native_dir / "linux-x64" / "libfoundry_local.so").write_bytes(b"fake")
    monkeypatch.setattr(_build_backend, "_NATIVE_DIR", native_dir)

    assert _build_backend._with_auto_plat_name(None) is None
