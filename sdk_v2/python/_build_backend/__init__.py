# PEP 517 backend shim for foundry-local-sdk.
#
# Delegates every hook to ``setuptools.build_meta``. The added behavior keeps
# dependency pins and native wheel tags aligned with the staged SDK payload.
#
# This is wired into pyproject.toml via::
#
#   [build-system]
#   build-backend = "_build_backend"
#   backend-path  = ["."]

from __future__ import annotations

import contextlib
import json
import re
from collections.abc import Generator
from pathlib import Path

# Re-export every PEP 517 hook from setuptools so any future additions are
# picked up automatically; only the wheel/metadata hooks need wrapping.
from setuptools.build_meta import *  # noqa: F401,F403
from setuptools.build_meta import (
    build_sdist as _orig_build_sdist,
    build_wheel as _orig_build_wheel,
    get_requires_for_build_sdist as _orig_get_requires_for_build_sdist,
    get_requires_for_build_wheel as _orig_get_requires_for_build_wheel,
    prepare_metadata_for_build_wheel as _orig_prepare_metadata_for_build_wheel,
)

try:  # editable hooks are optional in older setuptools versions.
    from setuptools.build_meta import (
        build_editable as _orig_build_editable,
        get_requires_for_build_editable as _orig_get_requires_for_build_editable,
        prepare_metadata_for_build_editable as _orig_prepare_metadata_for_build_editable,
    )

    _HAS_EDITABLE = True
except ImportError:  # pragma: no cover - newer setuptools only
    _HAS_EDITABLE = False


_PYPROJECT = Path(__file__).resolve().parent.parent / "pyproject.toml"
_DEPS_JSON = _PYPROJECT.parent.parent / "deps_versions.json"
_NATIVE_DIR = _PYPROJECT.parent / "src" / "foundry_local_sdk" / "_native"

# Patterns for rewriting ORT/GenAI version pins in the dependencies list. Each
# captures the package name + ``==`` and we substitute in the version read from
# deps_versions.json.
_ORT_PIN_PATTERN = re.compile(r'("onnxruntime(?:-core|-gpu)==)[^\s";]+')
_GENAI_PIN_PATTERN = re.compile(r'("onnxruntime-genai(?:-core|-cuda)==)[^\s";]+')
_NATIVE_PAYLOADS = {
    "win-x64": ("foundry_local.dll", None),
    "win-arm64": ("foundry_local.dll", None),
    "linux-x64": ("libfoundry_local.so", None),
    "linux-arm64": ("libfoundry_local.so", None),
    "osx-arm64": ("libfoundry_local.dylib", "macosx_11_0_arm64"),
}


def _read_versions() -> tuple[str, str]:
    if not _DEPS_JSON.is_file():
        raise RuntimeError(f"Required versions file not found: {_DEPS_JSON}")
    data = json.loads(_DEPS_JSON.read_text(encoding="utf-8"))
    try:
        ort = data["onnxruntime"]["version"]
        genai = data["onnxruntime-genai"]["version"]
    except (KeyError, TypeError) as exc:
        raise RuntimeError(
            f"{_DEPS_JSON} is missing required keys 'onnxruntime.version' / 'onnxruntime-genai.version'"
        ) from exc
    return str(ort), str(genai)


def _patch_pyproject_text(original: str) -> str:
    """Return *original* with the ORT/GenAI version pins rewritten from deps_versions.json."""
    ort_ver, genai_ver = _read_versions()
    patched = _ORT_PIN_PATTERN.sub(lambda m: f"{m.group(1)}{ort_ver}", original)
    patched = _GENAI_PIN_PATTERN.sub(lambda m: f"{m.group(1)}{genai_ver}", patched)
    return patched


def _config_values(value) -> list[str]:
    if value is None:
        return []
    if isinstance(value, str):
        return [value]
    return [str(item) for item in value]


def _has_explicit_plat_name(config_settings) -> bool:
    if not config_settings:
        return False
    build_options = _config_values(config_settings.get("--build-option"))
    return any(option == "--plat-name" or option.startswith("--plat-name=") for option in build_options)


def _auto_plat_name() -> str | None:
    staged = [
        (rid, plat_name)
        for rid, (lib_name, plat_name) in _NATIVE_PAYLOADS.items()
        if (_NATIVE_DIR / rid / lib_name).is_file()
    ]
    if len(staged) != 1:
        return None
    return staged[0][1]


def _with_auto_plat_name(config_settings):
    if _has_explicit_plat_name(config_settings):
        return config_settings

    plat_name = _auto_plat_name()
    if plat_name is None:
        return config_settings

    updated = dict(config_settings or {})
    updated["--build-option"] = _config_values(updated.get("--build-option")) + [f"--plat-name={plat_name}"]
    return updated


@contextlib.contextmanager
def _rewrite_version_pins() -> Generator[None, None, None]:
    """Rewrite pyproject.toml ORT/GenAI pins from deps_versions.json during build.

    deps_versions.json is the single source of truth for the native dependency
    versions; rewriting here keeps the wheel's declared pins in lockstep.
    """
    original = _PYPROJECT.read_text(encoding="utf-8")
    patched = _patch_pyproject_text(original)

    if patched == original:
        # Pins already match deps_versions.json — nothing to rewrite.
        yield
        return

    try:
        _PYPROJECT.write_text(patched, encoding="utf-8")
        yield
    finally:
        _PYPROJECT.write_text(original, encoding="utf-8")


def get_requires_for_build_wheel(config_settings=None):
    with _rewrite_version_pins():
        return _orig_get_requires_for_build_wheel(config_settings)


def prepare_metadata_for_build_wheel(metadata_directory, config_settings=None):
    with _rewrite_version_pins():
        return _orig_prepare_metadata_for_build_wheel(metadata_directory, config_settings)


def build_wheel(wheel_directory, config_settings=None, metadata_directory=None):
    with _rewrite_version_pins():
        return _orig_build_wheel(wheel_directory, _with_auto_plat_name(config_settings), metadata_directory)


def get_requires_for_build_sdist(config_settings=None):
    with _rewrite_version_pins():
        return _orig_get_requires_for_build_sdist(config_settings)


def build_sdist(sdist_directory, config_settings=None):
    with _rewrite_version_pins():
        return _orig_build_sdist(sdist_directory, config_settings)


if _HAS_EDITABLE:

    def get_requires_for_build_editable(config_settings=None):
        with _rewrite_version_pins():
            return _orig_get_requires_for_build_editable(config_settings)

    def prepare_metadata_for_build_editable(metadata_directory, config_settings=None):
        with _rewrite_version_pins():
            return _orig_prepare_metadata_for_build_editable(metadata_directory, config_settings)

    def build_editable(wheel_directory, config_settings=None, metadata_directory=None):
        with _rewrite_version_pins():
            return _orig_build_editable(wheel_directory, config_settings, metadata_directory)
