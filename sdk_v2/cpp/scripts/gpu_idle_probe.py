# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
"""Measure GPU memory and power across the native idle-unload integration test."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import threading


def snapshot(gpu: str) -> dict[str, object]:
    fields = ["uuid", "name", "memory.used", "power.draw", "pstate"]
    result = subprocess.run(
        [
            "nvidia-smi",
            f"--id={gpu}",
            f"--query-gpu={','.join(fields)}",
            "--format=csv,noheader,nounits",
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    values = result.stdout.strip().split(", ")
    if len(values) != len(fields):
        raise RuntimeError(f"Unexpected nvidia-smi output: {result.stdout!r}")
    processes = subprocess.run(
        [
            "nvidia-smi",
            f"--id={gpu}",
            "--query-compute-apps=pid,gpu_uuid,used_gpu_memory",
            "--format=csv,noheader,nounits",
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    measurement: dict[str, object] = dict(zip(fields, values, strict=True))
    measurement["compute_processes"] = processes.stdout.strip().splitlines()
    return measurement


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-binary", type=Path, required=True)
    parser.add_argument("--model-path", type=Path, required=True)
    parser.add_argument("--gpu", default="0")
    parser.add_argument("--cuda-runtime-dir", type=Path)
    args = parser.parse_args()

    if (
        not args.test_binary.is_file()
        or not (args.model_path / "genai_config.json").is_file()
    ):
        parser.error(
            "Provide an existing native test binary and a cached CUDA model directory"
        )

    env = os.environ.copy()
    env["FOUNDRY_TEST_GPU_MODEL_PATH"] = str(args.model_path.resolve())
    env["FOUNDRY_TEST_GPU_PROBE"] = "1"
    if args.cuda_runtime_dir:
        env["FOUNDRY_TEST_CUDA_RUNTIME_DIR"] = str(args.cuda_runtime_dir.resolve())
        env["PATH"] = (
            str(args.cuda_runtime_dir.resolve()) + os.pathsep + env.get("PATH", "")
        )

    measurements = [{"phase": "before-process", **snapshot(args.gpu)}]
    command = [
        str(args.test_binary.resolve()),
        "--gtest_filter=GpuIdleUnloadTest.AutomaticUnloadPreservesHttpAndResponseHistoryAndReloadsOnInference",
    ]
    with subprocess.Popen(
        command,
        env=env,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    ) as process:
        deadline = threading.Timer(180, process.kill)
        deadline.start()
        context_states: dict[str, int] = {}
        try:
            assert process.stdout is not None
            assert process.stdin is not None
            for line in process.stdout:
                print(line, end="", flush=True)
                if line.startswith("GPU_IDLE_CONTEXT "):
                    _, phase, active = line.split()
                    context_states[phase] = int(active)
                if line.startswith("GPU_IDLE_PHASE "):
                    phase = line.strip().split(" ", 1)[1]
                    measurement = {
                        "phase": phase,
                        "cuda_primary_context_active": context_states[phase],
                        **snapshot(args.gpu),
                    }
                    measurements.append(measurement)
                    print(json.dumps(measurement), flush=True)
                    process.stdin.write("\n")
                    process.stdin.flush()
            code = process.wait(timeout=30)
        finally:
            deadline.cancel()
            if process.poll() is None:
                process.kill()
                process.wait()

    measurements.append({"phase": "after-process", **snapshot(args.gpu)})
    print(json.dumps({"exit_code": code, "measurements": measurements}, indent=2))
    if code == 0 and {row["phase"] for row in measurements} != {
        "before-process",
        "baseline",
        "loaded",
        "idle",
        "reloaded",
        "after-process",
    }:
        raise RuntimeError("GPU test did not execute all measurement phases")
    return code


if __name__ == "__main__":
    sys.exit(main())
