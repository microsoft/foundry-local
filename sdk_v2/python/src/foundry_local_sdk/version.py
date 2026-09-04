"""Package version.

The version is single-sourced from ``pyproject.toml``'s ``[project] version``,
which the release pipeline stamps from ``version-info/pyVersion.txt``. Nothing
here needs updating on a version bump.

For an installed wheel we read the metadata recorded at build time. When running
straight from a source checkout (no dist-info, e.g. ``PYTHONPATH=src``) we fall
back to parsing ``pyproject.toml`` so the reported version still matches the
build inputs instead of a stale hardcoded literal.
"""

from __future__ import annotations

from importlib.metadata import PackageNotFoundError, version as _dist_version
from pathlib import Path

_DISTRIBUTION_NAME = "foundry-local-sdk"
_UNKNOWN = "0.0.0.dev0"


def _version_from_pyproject() -> str | None:
    # src/foundry_local_sdk/version.py -> sdk_v2/python/pyproject.toml
    pyproject = Path(__file__).resolve().parents[2] / "pyproject.toml"
    if not pyproject.is_file():
        return None

    try:
        import tomllib
    except ModuleNotFoundError:  # pragma: no cover - tomllib is stdlib on 3.11+
        return None

    try:
        with pyproject.open("rb") as fh:
            data = tomllib.load(fh)
    except (OSError, ValueError):
        return None

    found = data.get("project", {}).get("version")
    return found if isinstance(found, str) and found else None


def _resolve_version() -> str:
    try:
        return _dist_version(_DISTRIBUTION_NAME)
    except PackageNotFoundError:
        return _version_from_pyproject() or _UNKNOWN


__version__ = _resolve_version()
