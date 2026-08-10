"""Package version for foundry-local-sdk.

The single source of truth is the distribution metadata written from
``pyproject.toml`` at build time; deriving ``__version__`` from it keeps the
runtime value in lockstep with the installed wheel (e.g. ``2.0.0rc1``) instead
of drifting from a hand-maintained constant. The fallback only applies when the
package is imported from a source tree that was never installed.
"""

from importlib.metadata import PackageNotFoundError, version as _pkg_version

_DIST_NAME = "foundry-local-sdk"
# Fallback for uninstalled source-tree imports; keep in sync with pyproject.toml.
_FALLBACK_VERSION = "2.0.0.dev0"

try:
    __version__ = _pkg_version(_DIST_NAME)
except PackageNotFoundError:  # pragma: no cover - only hit for uninstalled source trees
    __version__ = _FALLBACK_VERSION

__all__ = ["__version__"]
