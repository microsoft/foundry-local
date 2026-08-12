"""Package version resolution."""

from __future__ import annotations

from importlib.metadata import PackageNotFoundError, version as _dist_version
from pathlib import Path

_DISTRIBUTION_NAME = "foundry-local-sdk"
_UNKNOWN = "0.0.0.dev0"


def _version_from_pyproject(pyproject: Path) -> str | None:
    if not pyproject.is_file():
        return None

    try:
        import tomllib

        with pyproject.open("rb") as fh:
            data = tomllib.load(fh)
    except (OSError, ValueError, tomllib.TOMLDecodeError):
        return None

    found = data.get("project", {}).get("version")
    return found if isinstance(found, str) and found else None


def _version_from_source_tree() -> str | None:
    # src/foundry_local_sdk/version.py -> sdk_v2/python/pyproject.toml
    return _version_from_pyproject(Path(__file__).resolve().parents[2] / "pyproject.toml")


def _resolve_version() -> str:
    # Prefer the adjacent source project's version when this module is imported
    # from a checkout, even if a different wheel is installed in the environment.
    source_version = _version_from_source_tree()
    if source_version is not None:
        return source_version

    try:
        return _dist_version(_DISTRIBUTION_NAME)
    except PackageNotFoundError:
        return _UNKNOWN


__version__ = _resolve_version()
