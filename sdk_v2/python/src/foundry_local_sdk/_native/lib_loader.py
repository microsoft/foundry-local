"""Runtime discovery of the foundry_local native library.

Search order:
1. ``FOUNDRY_LOCAL_LIB_DIR`` environment variable
2. Wheel-bundled location: ``<_native>/<platform>/foundry_local.{ext}``
3. Development build: walk up from ``__file__`` looking for ``sdk_v2/cpp/build/<Platform>/<Config>/...``
4. System path fallback — return ``None`` and let the OS resolve the bare library name at dlopen time
"""

from __future__ import annotations

import importlib.util
import logging
import os
import pathlib
import platform
import sys

logger = logging.getLogger(__name__)


def _lib_name() -> str:
    if sys.platform == "win32":
        return "foundry_local.dll"
    if sys.platform == "darwin":
        return "libfoundry_local.dylib"
    return "libfoundry_local.so"


def _platform_subdir() -> str:
    if sys.platform == "win32":
        return "win-x64"
    if sys.platform == "darwin":
        return "osx-arm64" if platform.machine() == "arm64" else "osx-x64"
    return "linux-x64"


def _build_platform_dir() -> str:
    """Return the platform sub-directory under ``sdk_v2/cpp/build/`` (matches the C++ build script)."""
    if sys.platform == "win32":
        return "Windows"
    if sys.platform == "darwin":
        return "macOS"
    return "Linux"


# Build configurations to try, most-likely first.
_BUILD_CONFIGS = ("RelWithDebInfo", "Release", "Debug")


def _dev_build_candidates(sdk_v2: pathlib.Path, name: str) -> list[pathlib.Path]:
    """Enumerate development-build paths to probe under ``sdk_v2/cpp/build/<Platform>/<Config>/`` for *name*.

    On Windows (multi-config MSBuild generators) the binary lands at ``bin/<Config>/<name>``. On Linux and
    macOS (single-config Make/Ninja generators) it lands at ``bin/<name>`` — we probe both layouts so the same
    code path works on every host.
    """
    platform_dir = _build_platform_dir()
    base = sdk_v2 / "cpp" / "build" / platform_dir
    candidates: list[pathlib.Path] = []
    for config in _BUILD_CONFIGS:
        candidates.append(base / config / "bin" / config / name)  # multi-config (MSBuild)
        candidates.append(base / config / "bin" / name)  # single-config (Ninja/Make)
    return candidates


def find_library() -> pathlib.Path | None:
    """Return the path to the foundry_local native library, or ``None`` if only the system path can resolve it.

    The caller (``_native/api.py``) treats a ``None`` result as "don't try to add a DLL directory; just let the
    cffi extension's own dlopen find the library on the system search path."
    """
    name = _lib_name()

    # 1. Explicit directory override via environment variable.
    env_dir = os.environ.get("FOUNDRY_LOCAL_LIB_DIR")
    if env_dir:
        candidate = pathlib.Path(env_dir) / name
        if candidate.exists():
            return candidate
        # Honour the env var even if the file isn't there yet — the error message from dlopen will point at
        # the right path.
        return candidate

    # 2. Wheel-bundled location: _native/<platform>/foundry_local.{ext}
    native_pkg_dir = pathlib.Path(__file__).resolve().parent
    wheel_candidate = native_pkg_dir / _platform_subdir() / name
    if wheel_candidate.exists():
        return wheel_candidate

    # 3. Development build — walk up the directory tree looking for sdk_v2/ then probe explicit per-platform
    #    build layouts. We deliberately avoid recursive globs here: on Linux/macOS a ``**`` glob over the build
    #    tree can be very slow.
    for parent in pathlib.Path(__file__).resolve().parents:
        sdk_v2 = parent / "sdk_v2"
        if sdk_v2.is_dir():
            for dev_candidate in _dev_build_candidates(sdk_v2, name):
                if dev_candidate.exists():
                    return dev_candidate
            break  # Found sdk_v2/ but no library — fall through to system path.

    # 4. System path fallback — let cffi's dlopen use the OS search path. Returning ``None`` (rather than a
    #    relative ``Path(name)``) prevents callers from accidentally treating CWD as the DLL directory.
    return None


# ---------------------------------------------------------------------------
# ORT / GenAI native dependency discovery
#
# foundry_local.{dll|so|dylib} is dynamically linked against onnxruntime and
# onnxruntime-genai. Those libraries ship in separate PyPI packages
# (onnxruntime-{core,gpu}, onnxruntime-genai-{core,cuda}) declared as
# install-time deps in pyproject.toml. At process start we have to:
#   * On Windows: add each package's bin dir to the DLL search path so the
#     loader can resolve onnxruntime.dll / onnxruntime-genai.dll when our
#     foundry_local.dll is loaded.
#   * On Linux/macOS: bridge the "lib" filename prefix mismatch
#     (libonnxruntime.so vs onnxruntime.so the binary was linked against)
#     by symlinking, since dlopen has no equivalent of add_dll_directory
#     and we don't want to mutate LD_LIBRARY_PATH for the whole process.
# ---------------------------------------------------------------------------

# On Linux/macOS the ORT packages ship their shared libs with a "lib" prefix;
# Windows uses no prefix. Our foundry_local binary was linked against the
# unprefixed names on every platform.
_ORT_PREFIX = "" if sys.platform == "win32" else "lib"


def _native_binary_names() -> tuple[str, str]:
    """Return ``(ort_filename, genai_filename)`` for the current platform."""
    ext = _lib_name().rsplit(".", 1)[-1]
    ext = "." + ext
    return (f"{_ORT_PREFIX}onnxruntime{ext}", f"{_ORT_PREFIX}onnxruntime-genai{ext}")


def _find_file_in_package(package_name: str, filename: str) -> pathlib.Path | None:
    """Locate a native binary *filename* inside an installed Python package.

    Probes the package root and well-known sub-dirs (``capi/``, ``native/``,
    ``lib/``, ``bin/``) before falling back to a recursive scan. Accepts
    hyphenated package names (translated to underscores for import lookup).
    """
    import_name = package_name.replace("-", "_")
    spec = importlib.util.find_spec(import_name)
    if spec is None or spec.origin is None:
        return None

    pkg_root = pathlib.Path(spec.origin).parent

    for candidate_dir in (pkg_root, pkg_root / "capi", pkg_root / "native", pkg_root / "lib", pkg_root / "bin"):
        # Glob with a wildcard around the filename to tolerate versioned suffixes
        # (e.g. libonnxruntime.so.1.25.1) but skip debug-info side files.
        candidates = [p for p in candidate_dir.glob(f"*{filename}*") if not p.name.endswith(".dbg")]
        if candidates:
            return candidates[0]

    # Recursive fallback — slow but only hit when the layout is unexpected.
    for match in pkg_root.rglob(filename):
        return match

    return None


def _resolve_ort_package_path(filename: str) -> pathlib.Path | None:
    """Locate ORT shared library, preferring the platform-specific variant."""
    if sys.platform.startswith("linux"):
        primary, fallback = "onnxruntime-gpu", "onnxruntime"
    else:
        primary, fallback = "onnxruntime-core", "onnxruntime"
    return _find_file_in_package(primary, filename) or _find_file_in_package(fallback, filename)


def _resolve_genai_package_path(filename: str) -> pathlib.Path | None:
    """Locate GenAI shared library, preferring the platform-specific variant."""
    if sys.platform.startswith("linux"):
        primary, fallback = "onnxruntime-genai-cuda", "onnxruntime-genai"
    else:
        primary, fallback = "onnxruntime-genai-core", "onnxruntime-genai"
    return _find_file_in_package(primary, filename) or _find_file_in_package(fallback, filename)


def find_ort_native_dirs() -> list[pathlib.Path]:
    """Return the deduplicated directories that contain the ORT and GenAI shared libs.

    Empty list if neither package is importable — the caller decides how to react
    (typically: log a warning and let the OS loader produce a clear error).
    """
    ort_name, genai_name = _native_binary_names()
    found: list[pathlib.Path] = []
    for path in (_resolve_ort_package_path(ort_name), _resolve_genai_package_path(genai_name)):
        if path is None:
            continue
        parent = path.parent.resolve()
        if parent not in found:
            found.append(parent)
    return found


def prepare_native_dependencies(foundry_local_dir: pathlib.Path) -> list:
    """Preload ORT and GenAI before libfoundry_local is loaded.

    This is the same pattern the legacy ``sdk/python`` SDK uses (see
    ``sdk/python/src/detail/core_interop.py::_initialize_native_libraries``)
    and the C# SDK uses (``sdk/cs/src/Detail/CoreInterop.cs::LoadOrtDllsIfInSameDir``).

    Why explicit preload — and not just RPATH:

    * The wheel ships libfoundry_local in ``_native/<rid>/`` but ORT and GenAI
      live in *sibling* PyPI packages (``onnxruntime-{core,gpu}`` /
      ``onnxruntime-genai-{core,cuda}``). They are NOT next to libfoundry_local,
      so libfoundry_local's RPATH (``$ORIGIN`` / ``@loader_path``) cannot find
      them.
    * Once ORT and GenAI are loaded into the process by absolute path, the
      OS loader resolves libfoundry_local's references to them by *name* from
      the already-loaded module table — no filesystem search, no RPATH
      involved. ORT must be loaded first because GenAI depends on it.

    Returns a list of ``ctypes.CDLL`` handles the caller MUST keep alive at
    module scope so the loaded libraries are not unloaded mid-process.

    Best effort: returns an empty list and logs (rather than raising) when
    the ORT/GenAI packages are missing — that lets ``libfoundry_local`` itself
    surface a clearer error, and lets unit tests that mock out the native
    layer keep working without ORT installed.
    """
    import ctypes

    handles: list = []

    ort_name, genai_name = _native_binary_names()
    ort_path = _resolve_ort_package_path(ort_name)
    genai_path = _resolve_genai_package_path(genai_name)

    if ort_path is None or genai_path is None:
        logger.info(
            "ORT/GenAI native packages not found via importlib (ort=%s, genai=%s); "
            "relying on OS search path. libfoundry_local will likely fail to load.",
            ort_path, genai_path,
        )
        return handles

    if sys.platform == "win32":
        # Windows: AddDllDirectory for every native dir first. This isn't
        # what makes ORT findable for libfoundry_local — the explicit CDLL
        # below does that — but it does help the cffi extension and any
        # other dependent DLLs find their own siblings.
        add_dll_directory = getattr(os, "add_dll_directory", None)
        if add_dll_directory is not None:
            for d in {ort_path.parent, genai_path.parent, foundry_local_dir}:
                try:
                    add_dll_directory(str(d))
                except OSError as exc:
                    logger.warning("os.add_dll_directory(%s) failed: %s", d, exc)

        # Explicit preload by absolute path. ORT first — GenAI's load-time
        # import of ORT then resolves to the already-loaded module by name.
        try:
            handles.append(ctypes.CDLL(str(ort_path)))
            logger.info("Preloaded ORT: %s", ort_path)
        except OSError as exc:
            logger.warning("Failed to preload ORT (%s): %s", ort_path, exc)
            return handles

        try:
            handles.append(ctypes.CDLL(str(genai_path)))
            logger.info("Preloaded GenAI: %s", genai_path)
        except OSError as exc:
            logger.warning("Failed to preload GenAI (%s): %s", genai_path, exc)

        return handles

    # POSIX (Linux/macOS): need RTLD_GLOBAL so symbols are visible to
    # libfoundry_local's later dlopen-by-name lookups.
    try:
        handles.append(ctypes.CDLL(str(ort_path), mode=ctypes.RTLD_GLOBAL))
        logger.info("Preloaded ORT (RTLD_GLOBAL): %s", ort_path)
    except OSError as exc:
        logger.warning("Failed to preload ORT (%s): %s", ort_path, exc)
        return handles

    # macOS only: GenAI's static initializer does its own dlopen("libonnxruntime.dylib"),
    # which on Darwin only matches by leafname against dyld search paths or images
    # whose install_name leaf is exactly "libonnxruntime.dylib". ORT's dylib has a
    # versioned install_name instead (the soversion, e.g. "@rpath/libonnxruntime.1.dylib"),
    # so neither match path succeeds and GenAI aborts in dyld init before any of our code
    # runs. The second name GenAI tries is "<genai_dir>/libonnxruntime.dylib", so a symlink
    # there fixes it. Linux dlopen consults the loaded-soname table by leafname and finds
    # our already-RTLD_GLOBAL'd image without help.
    if sys.platform == "darwin":
        symlink_path = genai_path.parent / "libonnxruntime.dylib"
        try:
            if not symlink_path.exists():
                symlink_path.symlink_to(ort_path)
                logger.info("Created macOS GenAI->ORT symlink: %s -> %s", symlink_path, ort_path)
        except OSError as exc:
            logger.warning("Failed to create macOS ORT symlink at %s: %s", symlink_path, exc)

    try:
        handles.append(ctypes.CDLL(str(genai_path), mode=ctypes.RTLD_GLOBAL))
        logger.info("Preloaded GenAI (RTLD_GLOBAL): %s", genai_path)
    except OSError as exc:
        logger.warning("Failed to preload GenAI (%s): %s", genai_path, exc)

    return handles
