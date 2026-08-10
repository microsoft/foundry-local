# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Unit tests for _build_backend pin-rewrite logic.

Exercises version-pin rewriting for the two runtime packages.

The module is loaded via spec_from_file_location to avoid a full `pip install
foundry-local-sdk[dev]` cycle; setuptools must be installed since
_build_backend wildcard-imports from setuptools.build_meta.  All tests are
skipped when setuptools is absent.
"""
from __future__ import annotations

import importlib.util
import pathlib
from unittest.mock import patch

import pytest

# ---------------------------------------------------------------------------
# Load _build_backend in isolation.
# ---------------------------------------------------------------------------
_BB_PATH = (
    pathlib.Path(__file__).resolve().parents[2] / "_build_backend" / "__init__.py"
)

try:
    import setuptools  # noqa: F401 — side-effect: confirms setuptools is present

    _spec = importlib.util.spec_from_file_location("_build_backend_isolated", _BB_PATH)
    _bb = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(_bb)  # type: ignore[union-attr]
    _BB_AVAILABLE = True
except (ImportError, ModuleNotFoundError):
    _bb = None  # type: ignore[assignment]
    _BB_AVAILABLE = False

pytestmark = pytest.mark.skipif(
    not _BB_AVAILABLE,
    reason="setuptools not installed or _build_backend not loadable",
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

_ORT_VER = "1.21.0"
_GENAI_VER = "0.7.1"


def _patch(text: str) -> str:
    """Run _patch_pyproject_text with fixed version stubs."""
    assert _bb is not None
    with patch.object(_bb, "_read_versions", return_value=(_ORT_VER, _GENAI_VER)):
        return _bb._patch_pyproject_text(text)


# ---------------------------------------------------------------------------
# Tests: ORT pin variants
# ---------------------------------------------------------------------------


class TestOrtPinRewrite:
    def test_plain_onnxruntime_sentinel_rewritten(self):
        line = '    "onnxruntime==0.0.0",\n'
        result = _patch(line)
        assert f'"onnxruntime=={_ORT_VER}' in result
        assert "0.0.0" not in result

    def test_ort_pattern_does_not_match_onnxruntime_genai_core(self):
        """The ORT pattern must not corrupt the genai line."""
        line = '    "onnxruntime-genai-core==0.0.0",\n'
        # Only the genai pattern should modify this — apply ORT pattern alone.
        assert _bb is not None
        result = _bb._ORT_PIN_PATTERN.sub(
            lambda m: f"{m.group(1)}{_ORT_VER}", line
        )
        assert "onnxruntime-genai-core==0.0.0" in result

    def test_ort_already_at_target_version_is_idempotent(self):
        line = f'    "onnxruntime=={_ORT_VER}; platform_system == \'Linux\'",\n'
        result = _patch(line)
        assert f'"onnxruntime=={_ORT_VER}' in result


# ---------------------------------------------------------------------------
# Tests: GenAI pin variants
# ---------------------------------------------------------------------------


class TestGenaiPinRewrite:
    def test_onnxruntime_genai_core_sentinel_rewritten(self):
        line = '    "onnxruntime-genai-core==0.0.0",\n'
        result = _patch(line)
        assert f'"onnxruntime-genai-core=={_GENAI_VER}' in result
        assert "0.0.0" not in result

    def test_genai_already_at_target_version_is_idempotent(self):
        line = f'    "onnxruntime-genai-core=={_GENAI_VER}",\n'
        result = _patch(line)
        assert f'"onnxruntime-genai-core=={_GENAI_VER}' in result

# ---------------------------------------------------------------------------
# Tests: full pyproject.toml block rewrite (integration-style)
# ---------------------------------------------------------------------------


class TestFullDependenciesBlock:
    """Simulate the complete dependency block from pyproject.toml."""

    _SAMPLE = """\
dependencies = [
    "cffi>=1.16",
    "typing_extensions>=4.5",
    "pydantic>=2.0.0",
    "requests>=2.32.4",
    "openai>=2.24.0",
    "onnxruntime==0.0.0",
    "onnxruntime-genai-core==0.0.0",
]
"""

    def test_all_sentinels_rewritten(self):
        result = _patch(self._SAMPLE)
        assert "0.0.0" not in result

    def test_ort_versions_correct(self):
        result = _patch(self._SAMPLE)
        assert result.count(f'"onnxruntime=={_ORT_VER}') == 1

    def test_genai_versions_correct(self):
        result = _patch(self._SAMPLE)
        assert result.count(f'"onnxruntime-genai-core=={_GENAI_VER}') == 1

    def test_non_ort_dependencies_unchanged(self):
        result = _patch(self._SAMPLE)
        assert '"cffi>=1.16"' in result
        assert '"pydantic>=2.0.0"' in result
        assert '"openai>=2.24.0"' in result

    def test_runtime_dependencies_are_unmarked(self):
        result = _patch(self._SAMPLE)
        assert "platform_machine" not in result
        assert "platform_system" not in result
