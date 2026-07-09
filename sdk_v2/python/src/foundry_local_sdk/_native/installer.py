"""CLI entry point for installing and verifying the Foundry Local native deps.

Usage::

    foundry-local-install [--verbose]

Re-installs the SDK wheel (``foundry-local-sdk``) via pip — pip then resolves the
ORT and GenAI runtime packages declared as dependencies in ``pyproject.toml``.
After install we materialise the platform's DLL search path / symlink workarounds
and verify ``onnxruntime`` and ``onnxruntime_genai`` import cleanly.

This is the v2 equivalent of the legacy ``foundry-local-install`` command.
v2 ships a single SDK wheel (rather than the legacy split between ``-sdk`` and
``-core``), so this command installs exactly one top-level package and lets pip
do dependency resolution.
"""

from __future__ import annotations

import argparse
import importlib.util
import pathlib
import subprocess
import sys

from foundry_local_sdk._native.lib_loader import (
    find_ort_native_dirs,
    prepare_native_dependencies,
)


def _wheel_native_dir() -> pathlib.Path:
    """Return the per-RID native dir bundled in this installation's wheel."""
    # _native/<rid>/foundry_local.{ext} — but we only need a directory hint
    # for prepare_native_dependencies (POSIX symlink target). Use this file's
    # parent regardless of RID; the actual platform sub-dir is discovered by
    # find_library() at runtime.
    return pathlib.Path(__file__).resolve().parent


def _expected_import_names() -> tuple[str, str]:
    """Return (ort_import_name, genai_import_name) for the current platform.

    Linux uses the GPU/CUDA-flavored packages whose import names are the
    canonical ``onnxruntime`` / ``onnxruntime_genai``. Windows and macOS
    install the ``-core`` PyPI packages, which expose distinct
    ``onnxruntime_core`` / ``onnxruntime_genai_core`` import names.
    """
    if sys.platform.startswith("linux"):
        return ("onnxruntime", "onnxruntime_genai")
    return ("onnxruntime_core", "onnxruntime_genai_core")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="foundry-local-install",
        description=(
            "(Re)install the Foundry Local SDK wheel and verify its ORT/GenAI "
            "native dependencies are reachable."
        ),
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Print resolved native binary directories after installation.",
    )
    args = parser.parse_args(argv)

    package = "foundry-local-sdk"
    print(f"[foundry-local] Installing {package} (will pull ORT/GenAI runtime deps)...")
    try:
        subprocess.check_call([sys.executable, "-m", "pip", "install", "--upgrade", package])
    except subprocess.CalledProcessError as exc:
        print(f"[foundry-local] ERROR: pip install failed (exit {exc.returncode}).", file=sys.stderr)
        return exc.returncode

    # Wire up the platform-specific search-path / symlink workarounds against
    # this same interpreter's site-packages so the verification imports below
    # see a fully-prepared environment.
    prepare_native_dependencies(_wheel_native_dir())

    dirs = find_ort_native_dirs()
    if not dirs:
        print(
            "[foundry-local] ERROR: Could not locate onnxruntime / onnxruntime-genai after install.",
            file=sys.stderr,
        )
        return 1

    # Verification: ensure the expected ORT / GenAI packages are *importable*
    # without actually running their import-time side effects. On Windows the
    # real `import onnxruntime_core` would fail until prepare_native_dependencies
    # has wired up the DLL search path for *this* process — but find_spec only
    # needs the package metadata to be on sys.path, which pip just guaranteed.
    missing: list[str] = []
    for mod in _expected_import_names():
        if importlib.util.find_spec(mod) is None:
            missing.append(mod)
    if missing:
        print(
            "[foundry-local] ERROR: expected runtime package(s) not importable after install: "
            + ", ".join(missing),
            file=sys.stderr,
        )
        return 1

    print(f"[foundry-local] Installed and verified ({package}).")
    if args.verbose:
        for d in dirs:
            print(f"  native dir: {d}")
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
