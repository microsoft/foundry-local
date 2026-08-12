# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Tests for Python SDK package version resolution."""

from __future__ import annotations

from pathlib import Path

import foundry_local_sdk.version as version_module


def test_source_tree_version_takes_precedence(monkeypatch):
    monkeypatch.setattr(version_module, "_version_from_source_tree", lambda: "2.0.0.dev20260812")
    monkeypatch.setattr(version_module, "_dist_version", lambda _: "9.9.9")

    assert version_module._resolve_version() == "2.0.0.dev20260812"


def test_installed_distribution_version_is_used_without_source_tree(monkeypatch):
    monkeypatch.setattr(version_module, "_version_from_source_tree", lambda: None)
    monkeypatch.setattr(version_module, "_dist_version", lambda _: "2.1.0")

    assert version_module._resolve_version() == "2.1.0"


def test_missing_pyproject_returns_none(tmp_path: Path):
    assert version_module._version_from_pyproject(tmp_path / "missing.toml") is None


def test_malformed_pyproject_falls_back_to_unknown(monkeypatch, tmp_path: Path):
    pyproject = tmp_path / "pyproject.toml"
    pyproject.write_text("[project\n", encoding="utf-8")
    monkeypatch.setattr(version_module, "_version_from_source_tree", lambda: version_module._version_from_pyproject(pyproject))

    def missing_distribution(_):
        raise version_module.PackageNotFoundError

    monkeypatch.setattr(version_module, "_dist_version", missing_distribution)
    assert version_module._resolve_version() == "0.0.0.dev0"
