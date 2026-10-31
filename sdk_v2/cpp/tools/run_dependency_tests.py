"""Require and run the installed-dependency regressions without allowing missing/skipped tests."""

import argparse
import json
from pathlib import Path
import subprocess
import sys
import xml.etree.ElementTree as ET


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--ctest", default="ctest")
    args = parser.parse_args()
    modes = (
        "retained",
        "threaded",
        "recreated",
        "explicit",
        "prime-only",
        "optout",
        "ci-disabled",
        "enabled-after-ci",
        "late-create",
        "upload-after-ci",
    )
    expected = {
        "TelemetryPortPolicy",
        *(f"TelemetryShutdown-{mode}" for mode in modes),
        *(
            f"NanoarrowConsumer-{config}-{first}"
            for config in ("Debug", "Release")
            for first in ("ON", "OFF")
        ),
    }
    if sys.platform == "win32":
        expected.update(
            f"WindowsTelemetryShutdown-{component}-{mode}"
            for component in ("manager", "telemetry")
            for mode in ("process-exit", "normal-destruction")
        )
        expected.add("WindowsTelemetryShutdown-telemetry-explicit-unload")
    command = [
        args.ctest,
        "--test-dir",
        str(args.build),
        "-C",
        args.config,
        "-R",
        "^(TelemetryShutdown-|WindowsTelemetryShutdown-|TelemetryPortPolicy$|NanoarrowConsumer-)",
    ]
    discovery = subprocess.run(
        command + ["--show-only=json-v1"], capture_output=True, text=True, check=True
    )
    actual = {test["name"] for test in json.loads(discovery.stdout)["tests"]}
    if actual != expected:
        raise SystemExit(
            f"Dependency regression discovery mismatch: {sorted(actual ^ expected)}"
        )
    results = args.build.resolve() / "TestResults"
    results.mkdir(parents=True, exist_ok=True)
    report = results / "dependency-regressions.xml"
    subprocess.run(
        command
        + ["--no-tests=error", "--output-on-failure", "--output-junit", str(report)],
        check=True,
    )
    cases = ET.parse(report).findall(".//testcase")
    if {case.get("name") for case in cases} != expected or any(
        case.find(tag) is not None
        for case in cases
        for tag in ("skipped", "failure", "error")
    ):
        raise SystemExit("Dependency regressions were missing, skipped, or failed")
    print(
        f"All {len(expected)} dependency regressions executed and passed; none skipped."
    )


if __name__ == "__main__":
    main()
