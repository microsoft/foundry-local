"""Compare two full SDK native bundles in alternating fresh processes; never infer regression from a microbenchmark."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile
import time
import zipfile


def bundle_metadata(library, output, label):
    files = sorted(
        path
        for path in library.parent.iterdir()
        if path.is_file()
        and (
            path.name == library.name
            or path.name.startswith(
                ("onnxruntime", "libonnxruntime", "Microsoft.Windows.AI.")
            )
        )
        and (path.suffix in (".dll", ".dylib", ".so") or ".so." in path.name)
    )
    archive = output / f"{label}-native.zip"
    with zipfile.ZipFile(
        archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6
    ) as package:
        for path in files:
            package.write(path, path.name)
    metadata = {
        "library": str(library),
        "library_sha256": hashlib.sha256(library.read_bytes()).hexdigest(),
        "library_bytes": library.stat().st_size,
        "native_bundle_bytes": sum(path.stat().st_size for path in files),
        "native_bundle_zip_bytes": archive.stat().st_size,
        "bundle_files": [path.name for path in files],
    }
    if library.suffix == ".dll":
        symbols = library.with_suffix(".pdb")
        import_library = library.with_suffix(".lib")
        if not import_library.exists():
            import_library = (
                library.parents[2] / library.parent.name / "foundry_local.lib"
            )
        headers = Path(__file__).resolve().parents[1] / "include"
        sdk_archive = output / f"{label}-cpp-sdk.zip"
        with zipfile.ZipFile(
            sdk_archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6
        ) as package:
            for path in [*files, symbols, import_library]:
                package.write(path, f"lib/{path.name}")
            for path in sorted(headers.rglob("*")):
                if path.is_file():
                    package.write(
                        path, f"include/{path.relative_to(headers).as_posix()}"
                    )
        metadata["cpp_sdk_zip_bytes"] = sdk_archive.stat().st_size
        metadata["cpp_sdk_package_method"] = (
            "Unsigned Windows C++ SDK layout: headers, native runtimes, PDB and import library; ZIP deflate level 6"
        )
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=10)
    parser.add_argument(
        "--model-id",
        help="Exact catalog model ID; each sample downloads into its own empty cache",
    )
    args = parser.parse_args()
    if args.samples < 3:
        parser.error("Use at least three paired samples")
    args.output.mkdir(parents=True, exist_ok=True)
    output = args.output.resolve()
    libraries = {
        "baseline": args.baseline.resolve(strict=True),
        "candidate": args.candidate.resolve(strict=True),
    }
    report = {
        "platform": platform.platform(),
        "telemetry": "CI-suppressed in both bundles (no collector transmission)",
        "method": "Alternating fresh processes and empty app/model caches; OS file cache is not flushed",
        "model_id": args.model_id,
        "bundles": {
            label: bundle_metadata(path, output, label)
            for label, path in libraries.items()
        },
        "samples": [],
    }
    environment = dict(os.environ, CI="1", TF_BUILD="1", ORT_DISABLE_TELEMETRY="1")
    for index in range(args.samples):
        order = (
            ("baseline", "candidate") if index % 2 == 0 else ("candidate", "baseline")
        )
        for label in order:
            with tempfile.TemporaryDirectory(prefix=f"{label}-", dir=output) as home:
                command = [str(args.probe.resolve()), str(libraries[label]), home]
                if args.model_id:
                    command.append(args.model_id)
                start = time.perf_counter()
                result = subprocess.run(
                    command,
                    env=environment,
                    capture_output=True,
                    text=True,
                    timeout=1800,
                )
                if result.returncode:
                    raise RuntimeError(
                        f"SDK probe failed ({result.returncode}): {result.stdout}\n{result.stderr}"
                    )
                wall_ms = (time.perf_counter() - start) * 1000
                rows = [
                    line.removeprefix("FOUNDRY_BENCHMARK ")
                    for line in result.stdout.splitlines()
                    if line.startswith("FOUNDRY_BENCHMARK ")
                ]
                if len(rows) != 1:
                    raise RuntimeError(
                        f"Missing benchmark result: {result.stdout}\n{result.stderr}"
                    )
                report["samples"].append(
                    {
                        "label": label,
                        "pair": index,
                        "process_ms": wall_ms,
                        **json.loads(rows[0]),
                    }
                )
                (output / "measurements.json").write_text(
                    json.dumps(report, indent=2), encoding="utf-8"
                )
    metrics = ("load_ms", "create_ms", "destroy_ms", "process_ms")
    if args.model_id:
        metrics += ("download_ms", "download_bytes")
        if len({row["download_bytes"] for row in report["samples"]}) != 1:
            raise RuntimeError(
                "Download samples did not produce identical artifact sizes"
            )
    report["medians"] = {
        label: {
            metric: statistics.median(
                row[metric] for row in report["samples"] if row["label"] == label
            )
            for metric in metrics
        }
        for label in libraries
    }
    report["median_change_percent"] = {
        metric: (
            report["medians"]["candidate"][metric]
            / report["medians"]["baseline"][metric]
            - 1
        )
        * 100
        for metric in metrics
        if report["medians"]["baseline"][metric] != 0
    }
    (output / "measurements.json").write_text(
        json.dumps(report, indent=2), encoding="utf-8"
    )
    print(
        json.dumps(
            {key: report[key] for key in ("medians", "median_change_percent")}, indent=2
        )
    )


if __name__ == "__main__":
    main()
