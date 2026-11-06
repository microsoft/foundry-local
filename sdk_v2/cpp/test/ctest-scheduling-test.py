# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
"""Verify that parallel discovery preserves every unit test and serial fallback."""

import argparse
from collections import Counter
import fnmatch
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ctest", required=True)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--unit-binary", required=True)
    parser.add_argument("--parallel-filter", required=True)
    args = parser.parse_args()

    listing = subprocess.run(
        [args.unit_binary, "--gtest_list_tests"],
        check=True, capture_output=True, text=True,
    )
    expected = Counter()
    suite = None
    for line in listing.stdout.splitlines():
        line = line.split("#", 1)[0].rstrip()
        if not line:
            continue
        if not line[0].isspace():
            suite = line if line.endswith(".") else None
        elif suite:
            expected[suite + line.strip()] += 1

    if not expected:
        raise RuntimeError("The unit binary did not enumerate any tests")

    manifest = subprocess.run(
        [args.ctest, "--test-dir", args.build_dir, "--build-config", args.config,
         "--show-only=json-v1"],
        check=True, capture_output=True, text=True,
    )
    actual = Counter()
    parallel_count = 0
    patterns = args.parallel_filter.split(":")
    for test in json.loads(manifest.stdout)["tests"]:
        command = test.get("command", [])
        properties = {prop["name"]: prop["value"] for prop in test["properties"]}
        parallel = False
        if command and Path(command[0]).resolve() == Path(args.unit_binary).resolve():
            filters = [arg.removeprefix("--gtest_filter=") for arg in command
                       if arg.startswith("--gtest_filter=")]
            if len(filters) != 1:
                raise RuntimeError(f"Expected one exact Google Test filter: {test['name']}")
            name = filters[0]
            actual[name] += 1
            parallel = any(fnmatch.fnmatchcase(name, pattern) for pattern in patterns)
        if bool(properties.get("RUN_SERIAL", False)) == parallel:
            raise RuntimeError(f"Incorrect serial/parallel scheduling: {test['name']}")
        parallel_count += parallel

    if actual != expected:
        raise RuntimeError(f"Missing tests: {expected - actual}; duplicated/unexpected tests: {actual - expected}")
    if parallel_count == 0:
        raise RuntimeError("No parallel-safe unit tests were registered")
    print(f"All {sum(expected.values())} unit tests registered exactly once; "
          f"{parallel_count} parallel-safe tests; all other tests serialized.")


if __name__ == "__main__":
    main()
