# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.

"""Assemble and pack the Microsoft.AI.Foundry.Local.Runtime NuGet package.

Collects native build artifacts from each platform into the standard
runtimes/{rid}/native/ layout and runs ``nuget pack``.

Usage (after all platform builds have completed)::

    python pack.py \\
        --version 0.1.0 \\
        --ort_version 1.24.4 \\
        --genai_version 0.13.1 \\
        --win_x64  path/to/win-x64/bin \\
        --win_arm64 path/to/win-arm64/bin \\
        --linux_x64 path/to/linux-x64/bin \\
        --osx_arm64 path/to/osx-arm64/bin \\
        --output_dir ./out

Each ``--<rid>`` argument points to the directory containing the built
shared library for that platform.  Only the RIDs you provide are included
in the package — pass as many or as few as you like.
"""

from __future__ import annotations

import argparse
import logging
import shutil
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent
REPO_ROOT = SCRIPT_DIR.parent  # sdk_v2/cpp

# Map CLI argument name → (NuGet RID, expected primary library).
#
# Vcpkg is statically linked on every platform (see sdk_v2/cpp/triplets/), so
# the foundry_local shared library carries its transitive deps inside itself
# and the upstream platform-build artifact for each RID contains only the
# primary library (plus, on Windows, .pdb and .lib companions consumed by the
# Python wheel build). We copy that one file into runtimes/<rid>/native/.
#
# On Windows the build also drops Microsoft.Windows.AI.MachineLearning.dll — the
# reg-free WinML 2.x runtime — next to foundry_local.dll (the cmake post-build
# copy in sdk_v2/cpp/CMakeLists.txt). We forward any entry from OPTIONAL_SIBLINGS
# that is present in the upstream artifact dir; absent files are silently skipped,
# so the same packing path serves every platform.
RIDS: dict[str, tuple[str, str]] = {
    "win_x64":    ("win-x64",    "foundry_local.dll"),
    "win_arm64":  ("win-arm64",  "foundry_local.dll"),
    "linux_x64":  ("linux-x64",  "libfoundry_local.so"),
    "osx_arm64":  ("osx-arm64",  "libfoundry_local.dylib"),
}

# Sibling files copied into runtimes/<rid>/native/ when present in the upstream
# artifact. Windows builds drop Microsoft.Windows.AI.MachineLearning.dll alongside
# foundry_local.dll; other platforms don't, so presence alone drives inclusion.
OPTIONAL_SIBLINGS: tuple[str, ...] = (
    "Microsoft.Windows.AI.MachineLearning.dll",
)

log = logging.getLogger("pack")


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)

    parser.add_argument("--version", required=True,
                        help="Package version (e.g. 0.1.0 or 0.1.0-dev.20260419).")
    parser.add_argument("--ort_version", required=True,
                        help="Minimum Microsoft.ML.OnnxRuntime.Foundry version.")
    parser.add_argument("--genai_version", required=True,
                        help="Minimum Microsoft.ML.OnnxRuntimeGenAI.Foundry version.")
    parser.add_argument("--package_id", default="Microsoft.AI.Foundry.Local.Runtime",
                        help="NuGet package id for the native runtime. On Windows the package "
                             "includes the reg-free WinML 2.x runtime "
                             "(Microsoft.Windows.AI.MachineLearning.dll) alongside foundry_local.")

    for arg_name, (rid, lib) in RIDS.items():
        parser.add_argument(f"--{arg_name}", type=Path, default=None,
                            help=f"Directory containing {lib} for {rid}.")

    parser.add_argument("--output_dir", type=Path, default=SCRIPT_DIR / "out",
                        help="Directory for the output .nupkg file.")
    parser.add_argument("--staging_dir", type=Path, default=None,
                        help="Staging directory. Default: output_dir/_staging.")
    parser.add_argument("--nuget_path", default="nuget",
                        help="Path to nuget.exe.")

    return parser.parse_args()


def _copy_tree(src: Path, dst: Path) -> None:
    """Copy a directory tree, creating dst if needed."""
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(src, dst)


def _copy_file(src: Path, dst_dir: Path) -> None:
    """Copy a single file into dst_dir, creating it if needed."""
    dst_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst_dir)


def stage(args: argparse.Namespace, staging: Path) -> int:
    """Build the staging directory layout. Returns the number of RIDs included."""
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)

    # --- nuspec ---
    nuspec_src = SCRIPT_DIR / "Microsoft.AI.Foundry.Local.Runtime.nuspec"
    shutil.copy2(nuspec_src, staging)

    # --- build/ and buildTransitive/ ---
    _copy_tree(SCRIPT_DIR / "build", staging / "build")
    _copy_tree(SCRIPT_DIR / "buildTransitive", staging / "buildTransitive")

    # --- C/C++ headers under build/native/include/ ---
    include_src = REPO_ROOT / "include"
    if include_src.is_dir():
        _copy_tree(include_src, staging / "build" / "native" / "include")

    # --- LICENSE ---
    license_file = REPO_ROOT / "LICENSE.txt"
    if license_file.is_file():
        shutil.copy2(license_file, staging)
    else:
        log.warning("LICENSE.txt not found at %s", license_file)

    # --- runtimes/{rid}/native/ ---
    rid_count = 0
    for arg_name, (rid, lib_name) in RIDS.items():
        src_dir: Path | None = getattr(args, arg_name)
        if src_dir is None:
            continue

        src_dir = src_dir.resolve()
        lib_path = src_dir / lib_name

        if not lib_path.is_file():
            log.error("Expected %s at %s but file not found.", lib_name, src_dir)
            sys.exit(1)

        native_dir = staging / "runtimes" / rid / "native"
        native_dir.mkdir(parents=True, exist_ok=True)

        # The upstream artifact for each RID is the primary library plus, on
        # Windows, .pdb / .lib companions used by the Python wheel build, and
        # Microsoft.Windows.AI.MachineLearning.dll (the WinML 2.x runtime).
        # We forward the primary library plus any present OPTIONAL_SIBLINGS;
        # everything else (.pdb, .lib) stays out of the NuGet runtime payload.
        shutil.copy2(lib_path, native_dir)
        log.info("  %s → runtimes/%s/native/%s", lib_path, rid, lib_path.name)
        staged = 1

        for sibling in OPTIONAL_SIBLINGS:
            sibling_path = src_dir / sibling
            if sibling_path.is_file():
                shutil.copy2(sibling_path, native_dir)
                log.info("  %s → runtimes/%s/native/%s", sibling_path, rid, sibling)
                staged += 1

        log.info("  staged %d file(s) into runtimes/%s/native/", staged, rid)
        rid_count += 1

    return rid_count


def pack(args: argparse.Namespace, staging: Path) -> None:
    """Run nuget pack on the staged layout."""
    args.output_dir.mkdir(parents=True, exist_ok=True)

    nuspec = staging / "Microsoft.AI.Foundry.Local.Runtime.nuspec"
    properties = (
        f"version={args.version};"
        f"ort_version={args.ort_version};"
        f"genai_version={args.genai_version};"
        f"package_id={args.package_id}"
    )

    cmd = [
        str(args.nuget_path), "pack", str(nuspec),
        "-Properties", properties,
        "-OutputDirectory", str(args.output_dir),
        "-NoPackageAnalysis",
    ]

    log.info("Running: %s", " ".join(cmd))
    subprocess.run(cmd, check=True)

    # Report result
    for nupkg in args.output_dir.glob("*.nupkg"):
        log.info("Created: %s", nupkg)


def main() -> None:
    logging.basicConfig(format="%(asctime)s %(name)s [%(levelname)s] %(message)s",
                        level=logging.INFO)

    args = _parse_args()

    staging = args.staging_dir or (args.output_dir / "_staging")
    staging = staging.resolve()

    log.info("Staging directory: %s", staging)
    rid_count = stage(args, staging)

    if rid_count == 0:
        log.error("No platform artifacts provided. Pass at least one --<rid> argument.")
        sys.exit(1)

    log.info("Staged %d RID(s). Packing...", rid_count)
    pack(args, staging)


if __name__ == "__main__":
    main()
