#!/usr/bin/env python3
"""Produce a versioned tool-calling qualification for one model and backend.

The runner composes, rather than replaces, the real-model protocol gate and the
real GitHub Copilot CLI coding-agent gate. Raw child artifacts stay in their
existing private directories. The top-level qualification contains fixed check
names, environment and artifact fingerprints, and links to local child runs.

Pass --genai-report to add the direct ORT GenAI layer produced by
benchmark/python/toolcalling/benchmark_toolcalling.py. Without it, the verdict
is explicitly scoped to Foundry HTTP and coding-agent behavior.
"""

from __future__ import annotations

import argparse
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time
from urllib.parse import urlsplit
import uuid


SCRIPT_DIR = Path(__file__).resolve().parent
CPP_ROOT = SCRIPT_DIR.parents[1]
QUALIFICATION_ROOT = CPP_ROOT / "build" / "coding-agent-gate"
PROTOCOL_GATE = SCRIPT_DIR / "tool_call_gate.py"
CODING_GATE = SCRIPT_DIR / "coding_agent_gate.py"
FORMAT_VERSION = "foundry-tool-calling-qualification/v1"


def read_json(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"{path.name} must contain a JSON object")
    return value


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def command_output(command: list[str]) -> str | None:
    try:
        result = subprocess.run(command, check=True, capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.SubprocessError):
        return None
    return result.stdout.strip() or None


def environment_fingerprint(backend: str, artifacts: list[Path]) -> dict:
    gpu = command_output([
        "nvidia-smi",
        "--query-gpu=name,compute_cap,driver_version,memory.total",
        "--format=csv,noheader",
    ])
    commit = command_output(["git", "-C", str(CPP_ROOT), "rev-parse", "HEAD"])
    return {
        "backend": backend,
        "platform": platform.platform(),
        "python": platform.python_version(),
        "repository_commit": commit,
        "gpu_inventory": gpu.splitlines() if gpu else [],
        "artifacts": [
            {"name": path.name, "size_bytes": path.stat().st_size, "sha256": sha256(path)}
            for path in artifacts
        ],
    }


def assess_genai_report(report: dict, backend: str, minimum_percent: float) -> dict:
    checks = {
        "end_to_end_mode": report.get("mode") == "end_to_end",
        "expected_backend": report.get("execution_provider") == backend,
        "cases_present": type(report.get("cases")) is int and report["cases"] > 0,
        "all_case_results_present": (
            isinstance(report.get("results"), list)
            and report.get("cases") == len(report["results"])
        ),
        "minimum_end_to_end_accuracy": (
            isinstance(report.get("end_to_end_correct_percent"), (int, float))
            and report["end_to_end_correct_percent"] >= minimum_percent
        ),
    }
    return {
        "qualified": all(checks.values()),
        "checks": checks,
        "cases": report.get("cases"),
        "end_to_end_correct_percent": report.get("end_to_end_correct_percent"),
        "minimum_percent": minimum_percent,
    }


def newest_child(parent: Path, before: set[Path], prefix: str) -> Path:
    created = {path for path in parent.glob(prefix + "*") if path.is_dir()} - before
    if len(created) != 1:
        raise RuntimeError(f"Expected one new {prefix} child run, found {len(created)}")
    return created.pop()


def run_command(command: list[str], stdout_path: Path, timeout: int) -> int:
    with stdout_path.open("w", encoding="utf-8") as output:
        result = subprocess.run(
            command,
            stdout=output,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
            check=False,
        )
    return result.returncode


def relative(path: Path, root: Path) -> str:
    return str(path.relative_to(root)).replace(os.sep, "/")


def run_protocol_gate(args: argparse.Namespace, run: Path, repetition: int) -> dict:
    output = run / f"protocol-{repetition:02d}"
    command = [
        sys.executable,
        str(PROTOCOL_GATE),
        "--endpoint",
        args.endpoint,
        "--model",
        args.model,
        "--output-dir",
        str(output),
        "--timeout-seconds",
        str(args.request_timeout_seconds),
    ]
    code = run_command(command, run / f"protocol-{repetition:02d}.log", args.run_timeout_seconds)
    summary = read_json(output / "summary.json") if (output / "summary.json").is_file() else {}
    return {
        "qualified": code == 0 and summary.get("qualified") is True,
        "exit_code": code,
        "artifact": relative(output, run),
        "cases": len(summary.get("results", [])),
    }


def run_coding_gate(args: argparse.Namespace, run: Path, repetition: int, profile: str) -> dict:
    parent = run / "coding"
    parent.mkdir(exist_ok=True)
    before = {path for path in parent.glob("run-*") if path.is_dir()}
    command = [
        sys.executable,
        str(CODING_GATE),
        "--endpoint",
        args.endpoint,
        "--model",
        args.model,
        "--output-dir",
        str(parent),
        "--schema-profile",
        profile,
        "--timeout-seconds",
        str(args.run_timeout_seconds),
        "--max-requests",
        str(args.max_requests),
        "--max-prompt-tokens",
        str(args.max_prompt_tokens),
        "--max-output-tokens",
        str(args.max_output_tokens),
    ]
    if args.copilot:
        command.extend(["--copilot", str(args.copilot)])
    log = run / f"coding-{repetition:02d}-{profile}.log"
    code = run_command(command, log, args.run_timeout_seconds + 60)
    child = newest_child(parent, before, "run-")
    summary = read_json(child / "summary.json")
    return {
        "qualified": code == 0 and summary.get("qualified") is True,
        "exit_code": code,
        "profile": profile,
        "artifact": relative(child, run),
        "http_requests": summary.get("http_requests"),
        "provider_error_events": summary.get("provider_error_events"),
    }


def self_test() -> int:
    good = {
        "mode": "end_to_end",
        "execution_provider": "cuda",
        "cases": 2,
        "end_to_end_correct_percent": 100.0,
        "results": [{}, {}],
    }
    checks = {
        "accept_complete_report": assess_genai_report(good, "cuda", 100)["qualified"],
        "reject_wrong_backend": not assess_genai_report(dict(good, execution_provider="cpu"), "cuda", 100)[
            "qualified"
        ],
        "reject_missing_result": not assess_genai_report(dict(good, results=[{}]), "cuda", 100)["qualified"],
        "reject_below_threshold": not assess_genai_report(
            dict(good, end_to_end_correct_percent=99.9), "cuda", 100
        )["qualified"],
    }
    passed = all(checks.values())
    print(json.dumps({"self_test_passed": passed, "endpoint_qualification_performed": False, "checks": checks}, indent=2))
    return 0 if passed else 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", help="Loopback OpenAI-compatible base URL, including /v1")
    parser.add_argument("--model", help="Exact Foundry wire model ID")
    parser.add_argument("--backend", default="cuda", choices=["cuda", "cpu", "webgpu"])
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--genai-report", type=Path)
    parser.add_argument("--genai-min-percent", type=float, default=100.0)
    parser.add_argument("--repetitions", type=int, default=1)
    parser.add_argument(
        "--schema-profile",
        action="append",
        choices=["native", "minlength", "oneof"],
        dest="schema_profiles",
        help="Coding-agent schema profile; repeat to require multiple profiles (default: native)",
    )
    parser.add_argument("--artifact", action="append", type=Path, default=[], help="Binary or model file to fingerprint")
    parser.add_argument("--copilot", type=Path)
    parser.add_argument("--request-timeout-seconds", type=int, default=120)
    parser.add_argument("--run-timeout-seconds", type=int, default=600)
    parser.add_argument("--max-requests", type=int, default=24)
    parser.add_argument("--max-prompt-tokens", type=int, default=32768)
    parser.add_argument("--max-output-tokens", type=int, default=8192)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return args
    if not args.endpoint or not args.model or args.output_dir is None:
        parser.error("--endpoint, --model, and --output-dir are required unless --self-test is selected")
    target = urlsplit(args.endpoint)
    try:
        address = ipaddress.ip_address(target.hostname or "")
        port = target.port
    except ValueError:
        parser.error("--endpoint must be an unauthenticated loopback HTTP(S) API base URL")
    if (
        target.scheme not in {"http", "https"}
        or not address.is_loopback
        or target.username
        or target.password
        or target.query
        or target.fragment
        or (port is not None and not 1 <= port <= 65535)
    ):
        parser.error("--endpoint must be an unauthenticated loopback HTTP(S) API base URL")
    if not args.model.strip() or args.model.lower() == "auto" or any(ord(char) < 32 for char in args.model):
        parser.error("--model must be an explicit local model ID")
    args.output_dir = args.output_dir.expanduser().resolve()
    if not args.output_dir.is_relative_to(QUALIFICATION_ROOT.resolve()):
        parser.error("--output-dir must be beneath sdk_v2/cpp/build/coding-agent-gate")
    if args.repetitions <= 0:
        parser.error("--repetitions must be positive")
    if not 0 <= args.genai_min_percent <= 100:
        parser.error("--genai-min-percent must be between 0 and 100")
    if min(
        args.request_timeout_seconds,
        args.run_timeout_seconds,
        args.max_requests,
        args.max_prompt_tokens,
        args.max_output_tokens,
    ) <= 0:
        parser.error("timeouts, request limits, and token limits must be positive")
    args.schema_profiles = args.schema_profiles or ["native"]
    if len(set(args.schema_profiles)) != len(args.schema_profiles):
        parser.error("--schema-profile values must be unique")
    args.artifact = [path.expanduser().resolve(strict=True) for path in args.artifact]
    if any(not path.is_file() for path in args.artifact):
        parser.error("--artifact values must be files")
    if args.genai_report:
        args.genai_report = args.genai_report.expanduser().resolve(strict=True)
    if args.copilot:
        args.copilot = args.copilot.expanduser().resolve(strict=True)
    return args


def main() -> int:
    args = parse_args()
    if args.self_test:
        return self_test()

    run = args.output_dir / ("qualification-" + time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:8])
    run.mkdir(parents=True)
    result = {
        "format": FORMAT_VERSION,
        "qualified": False,
        "scope": "cross-stack" if args.genai_report else "foundry",
        "model": args.model,
        "environment": environment_fingerprint(args.backend, args.artifact),
        "requirements": {
            "repetitions": args.repetitions,
            "schema_profiles": args.schema_profiles,
            "genai_min_percent": args.genai_min_percent if args.genai_report else None,
        },
        "genai": None,
        "runs": [],
        "failure": None,
    }
    try:
        if args.genai_report:
            result["genai"] = assess_genai_report(
                read_json(args.genai_report), args.backend, args.genai_min_percent
            )
        for repetition in range(1, args.repetitions + 1):
            entry = {
                "repetition": repetition,
                "protocol": run_protocol_gate(args, run, repetition),
                "coding": [
                    run_coding_gate(args, run, repetition, profile)
                    for profile in args.schema_profiles
                ],
            }
            entry["qualified"] = entry["protocol"]["qualified"] and all(
                item["qualified"] for item in entry["coding"]
            )
            result["runs"].append(entry)
        result["qualified"] = (
            all(item["qualified"] for item in result["runs"])
            and (result["genai"] is None or result["genai"]["qualified"])
        )
        if not result["qualified"]:
            result["failure"] = "qualification_checks_failed"
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        result["failure"] = type(error).__name__
        (run / "failure-detail.txt").write_text(str(error), encoding="utf-8")
    result["completed_at_epoch_seconds"] = int(time.time())
    (run / "qualification.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({**result, "runs": [
        {
            "repetition": item["repetition"],
            "qualified": item["qualified"],
            "protocol": item["protocol"]["qualified"],
            "coding": {run["profile"]: run["qualified"] for run in item["coding"]},
        }
        for item in result["runs"]
    ]}, indent=2))
    return 0 if result["qualified"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
