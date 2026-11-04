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
        --win_x64    path/to/win-x64/bin \\
        --win_arm64  path/to/win-arm64/bin \\
        --linux_x64  path/to/linux-x64/bin \\
        --linux_arm64 path/to/linux-arm64/bin \\
        --osx_arm64  path/to/osx-arm64/bin \\
        --output_dir ./out

Each ``--<rid>`` argument points to the directory containing the built
shared library for that platform.  Only the RIDs you provide are included
in the package — pass as many or as few as you like.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import logging
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent
REPO_ROOT = SCRIPT_DIR.parent  # sdk_v2/cpp

# Map CLI argument name → (NuGet RID, expected primary library).
#
# Vcpkg is statically linked on every platform (see sdk_v2/cpp/triplets/), so
# the foundry_local shared library carries its transitive deps inside itself
# and the upstream platform-build artifact for each RID contains the primary
# library plus platform-specific companions. We copy the redistributable files
# into runtimes/<rid>/native/.
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
    "linux_arm64": ("linux-arm64", "libfoundry_local.so"),
    "osx_arm64":  ("osx-arm64",  "libfoundry_local.dylib"),
}

# Sibling files copied into runtimes/<rid>/native/ when present in the upstream
# artifact. Windows builds provide the import library needed by applications
# that link against foundry_local.dll, plus the WinML runtime DLL. Other
# platforms don't provide these files, so presence alone drives inclusion.
OPTIONAL_SIBLINGS: tuple[str, ...] = (
    "foundry_local.lib",
    "Microsoft.Windows.AI.MachineLearning.dll",
)

log = logging.getLogger("pack")

HOST_MANIFEST = "foundry-local-runtime.json"


def _host_bundle_files(src_dir: Path, rid: str, lib_name: str) -> tuple[Path, Path]:
    host_name = "foundry-local-host.exe" if rid.startswith("win-") else "foundry-local-host"
    host = src_dir / host_name
    manifest_path = src_dir / HOST_MANIFEST
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if (
        not isinstance(manifest, dict)
        or type(manifest.get("version")) is not int
        or manifest["version"] != 1
        or type(manifest.get("protocol")) is not int
        or manifest["protocol"] != 1
        or manifest.get("rid") != rid
        or not isinstance(manifest.get("native_build"), str)
        or not manifest["native_build"].strip()
    ):
        raise ValueError(f"invalid native host manifest for {rid}")
    for role, name in (("native", lib_name), ("host", host_name)):
        entry = manifest.get(role)
        if not isinstance(entry, dict) or entry.get("file_name") != name:
            raise ValueError(f"invalid {role} file name in {HOST_MANIFEST}")
        digest = entry.get("sha256")
        if not isinstance(digest, str) or digest.lower() != hashlib.sha256((src_dir / name).read_bytes()).hexdigest():
            raise ValueError(f"{role} file hash does not match {HOST_MANIFEST}; regenerate after final signing")
    return host, manifest_path


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)

    parser.add_argument("--version", required=True,
                        help="Package version (e.g. 0.1.0 or 0.1.0-dev.20260419).")
    parser.add_argument("--ort_version", required=True,
                        help="Minimum Microsoft.ML.OnnxRuntime version.")
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

    # --- package documentation and notices ---
    for document_name in ("README.md", "Privacy.md", "ThirdPartyNotices.txt"):
        document_file = SCRIPT_DIR / document_name
        if document_file.is_file():
            shutil.copy2(document_file, staging)
        else:
            log.warning("Package document not found at %s", document_file)

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

        try:
            host_files = _host_bundle_files(src_dir, rid, lib_name)
        except (OSError, ValueError) as error:
            log.error("Invalid native host bundle for %s: %s", rid, error)
            sys.exit(1)

        native_dir = staging / "runtimes" / rid / "native"
        native_dir.mkdir(parents=True, exist_ok=True)

        # Forward the primary library plus any present redistributable siblings.
        # Build symbols such as .pdb remain out of the NuGet runtime payload.
        shutil.copy2(lib_path, native_dir)
        log.info("  %s → runtimes/%s/native/%s", lib_path, rid, lib_path.name)
        staged = 1

        for required in host_files:
            _copy_file(required, native_dir)
            staged += 1

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

    with tempfile.TemporaryDirectory(prefix="native-pack-", dir=args.output_dir) as temporary:
        cmd = [
            str(args.nuget_path), "pack", str(nuspec),
            "-Properties", properties,
            "-OutputDirectory", temporary,
            "-NoPackageAnalysis",
        ]
        log.info("Running: %s", " ".join(cmd))
        subprocess.run(cmd, check=True)
        for nupkg in Path(temporary).glob("*.nupkg"):
            _set_host_permissions(nupkg)
            output = args.output_dir / nupkg.name
            shutil.move(str(nupkg), output)
            log.info("Created: %s", output)


def _set_host_permissions(package: Path) -> None:
    """Preserve executable metadata when packing Unix RIDs on Windows, before NuGet signing."""
    updated = package.with_suffix(".tmp")
    try:
        with zipfile.ZipFile(package) as source, zipfile.ZipFile(updated, "w") as target:
            target.comment = source.comment
            for entry in source.infolist():
                info = copy.copy(entry)
                if entry.filename.startswith("runtimes/") and entry.filename.endswith("/native/foundry-local-host"):
                    info.create_system = 3
                    info.external_attr = (0o100755 << 16) | (info.external_attr & 0xFFFF)
                with source.open(entry) as reader, target.open(info, "w") as writer:
                    shutil.copyfileobj(reader, writer)
        updated.replace(package)
    finally:
        updated.unlink(missing_ok=True)


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
