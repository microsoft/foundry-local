# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Unit tests for _build_backend pin-rewrite logic.

Exercises the regex patterns and _patch_pyproject_text so we can verify
that all ORT/GenAI package name variants — including the new plain
``onnxruntime`` and ``onnxruntime-genai`` names added for Linux ARM64 —
are correctly rewritten at wheel-build time.

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
        """Plain onnxruntime== (Linux ARM64 CPU-only package)."""
        line = '    "onnxruntime==0.0.0; platform_system == \'Linux\' and platform_machine != \'x86_64\'",\n'
        result = _patch(line)
        assert f'"onnxruntime=={_ORT_VER}' in result
        assert "0.0.0" not in result

    def test_onnxruntime_gpu_sentinel_rewritten(self):
        """onnxruntime-gpu== (Linux x86_64 CUDA package) — must still work."""
        line = '    "onnxruntime-gpu==0.0.0; platform_system == \'Linux\'",\n'
        result = _patch(line)
        assert f'"onnxruntime-gpu=={_ORT_VER}' in result
        assert "0.0.0" not in result

    def test_onnxruntime_core_sentinel_rewritten(self):
        """onnxruntime-core== (non-Linux package) — must still work."""
        line = '    "onnxruntime-core==0.0.0; platform_system != \'Linux\'",\n'
        result = _patch(line)
        assert f'"onnxruntime-core=={_ORT_VER}' in result
        assert "0.0.0" not in result

    def test_ort_pattern_does_not_match_onnxruntime_genai(self):
        """The ORT pattern must not corrupt the genai line."""
        line = '    "onnxruntime-genai==0.0.0; platform_system == \'Linux\'",\n'
        # Only the genai pattern should modify this — apply ORT pattern alone.
        assert _bb is not None
        result = _bb._ORT_PIN_PATTERN.sub(
            lambda m: f"{m.group(1)}{_ORT_VER}", line
        )
        # onnxruntime-genai should be untouched by the ORT pattern.
        assert "onnxruntime-genai==0.0.0" in result

    def test_ort_already_at_target_version_is_idempotent(self):
        line = f'    "onnxruntime=={_ORT_VER}; platform_system == \'Linux\'",\n'
        result = _patch(line)
        assert f'"onnxruntime=={_ORT_VER}' in result


# ---------------------------------------------------------------------------
# Tests: GenAI pin variants
# ---------------------------------------------------------------------------


class TestGenaiPinRewrite:
    def test_plain_onnxruntime_genai_sentinel_rewritten(self):
        """Plain onnxruntime-genai== (Linux ARM64 CPU-only package)."""
        line = '    "onnxruntime-genai==0.0.0; platform_system == \'Linux\' and platform_machine != \'x86_64\'",\n'
        result = _patch(line)
        assert f'"onnxruntime-genai=={_GENAI_VER}' in result
        assert "0.0.0" not in result

    def test_onnxruntime_genai_cuda_sentinel_rewritten(self):
        """onnxruntime-genai-cuda== (Linux x86_64) — must still work."""
        line = '    "onnxruntime-genai-cuda==0.0.0; platform_system == \'Linux\'",\n'
        result = _patch(line)
        assert f'"onnxruntime-genai-cuda=={_GENAI_VER}' in result
        assert "0.0.0" not in result

    def test_onnxruntime_genai_core_sentinel_rewritten(self):
        """onnxruntime-genai-core== (non-Linux) — must still work."""
        line = '    "onnxruntime-genai-core==0.0.0; platform_system != \'Linux\'",\n'
        result = _patch(line)
        assert f'"onnxruntime-genai-core=={_GENAI_VER}' in result
        assert "0.0.0" not in result

    def test_genai_already_at_target_version_is_idempotent(self):
        line = f'    "onnxruntime-genai=={_GENAI_VER}; platform_system == \'Linux\'",\n'
        result = _patch(line)
        assert f'"onnxruntime-genai=={_GENAI_VER}' in result


# ---------------------------------------------------------------------------
# Tests: full pyproject.toml block rewrite (integration-style)
# ---------------------------------------------------------------------------


class TestFullDependenciesBlock:
    """Simulate the complete six-line dependency block from pyproject.toml."""

    _SAMPLE = """\
dependencies = [
    "cffi>=1.16",
    "typing_extensions>=4.5",
    "pydantic>=2.0.0",
    "requests>=2.32.4",
    "openai>=2.24.0",
    "onnxruntime-gpu==0.0.0; platform_system == 'Linux' and platform_machine == 'x86_64'",
    "onnxruntime==0.0.0; platform_system == 'Linux' and platform_machine != 'x86_64'",
    "onnxruntime-core==0.0.0; platform_system != 'Linux'",
    "onnxruntime-genai-cuda==0.0.0; platform_system == 'Linux' and platform_machine == 'x86_64'",
    "onnxruntime-genai==0.0.0; platform_system == 'Linux' and platform_machine != 'x86_64'",
    "onnxruntime-genai-core==0.0.0; platform_system != 'Linux'",
]
"""

    def test_all_six_sentinels_rewritten(self):
        result = _patch(self._SAMPLE)
        assert "0.0.0" not in result

    def test_ort_versions_correct(self):
        result = _patch(self._SAMPLE)
        assert f'"onnxruntime-gpu=={_ORT_VER}' in result
        assert f'"onnxruntime=={_ORT_VER}' in result
        assert f'"onnxruntime-core=={_ORT_VER}' in result

    def test_genai_versions_correct(self):
        result = _patch(self._SAMPLE)
        assert f'"onnxruntime-genai-cuda=={_GENAI_VER}' in result
        assert f'"onnxruntime-genai=={_GENAI_VER}' in result
        assert f'"onnxruntime-genai-core=={_GENAI_VER}' in result

    def test_non_ort_dependencies_unchanged(self):
        result = _patch(self._SAMPLE)
        assert '"cffi>=1.16"' in result
        assert '"pydantic>=2.0.0"' in result
        assert '"openai>=2.24.0"' in result

    def test_platform_markers_preserved(self):
        result = _patch(self._SAMPLE)
        assert "platform_machine == 'x86_64'" in result
        assert "platform_machine != 'x86_64'" in result
        assert "platform_system == 'Linux'" in result
        assert "platform_system != 'Linux'" in result
