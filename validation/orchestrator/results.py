"""Result record construction, minimal schema validation (stdlib-only), and
markdown summary generation for the validation harness.

We intentionally avoid the third-party `jsonschema` package so the harness runs
on a bare agent. The validator here covers the constraints we actually rely on:
required keys, enum membership, and type of a few critical fields.
"""
from __future__ import annotations

import datetime as _dt
import json
import os
from typing import Any, Dict, List

RESULT_CLASSES = {"pass", "fail", "blocked", "waived", "n-a", "skipped"}
CATEGORIES = {"packaging", "runtime", "feature", "compat", "nonfunctional"}
ACCELERATORS = {"cpu", "cuda", "winml-dml", "npu-winml", "coreml-metal", "webgpu", "none"}
SDKS = {"cpp", "cs", "js", "python"}
SCHEMA_VERSION = "1.0"


def now_iso() -> str:
    return _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0).isoformat()


def make_record(
    *,
    cell_id: str,
    run_id: str,
    sdk: str,
    feature: str,
    category: str,
    blocking: bool,
    accelerator: str,
    result: str,
    env: Dict[str, Any],
    package: Dict[str, Any],
    model: Dict[str, Any] | None = None,
    assertions: List[Dict[str, Any]] | None = None,
    duration_seconds: float | None = None,
    log_path: str | None = None,
    notes: str | None = None,
    owner: str | None = None,
    waiver: Dict[str, Any] | None = None,
) -> Dict[str, Any]:
    return {
        "schema_version": SCHEMA_VERSION,
        "cell_id": cell_id,
        "run_id": run_id,
        "sdk": sdk,
        "feature": feature,
        "category": category,
        "blocking": blocking,
        "model": model,
        "accelerator": accelerator,
        "result": result,
        "duration_seconds": duration_seconds,
        "env": {
            "platform_id": env.get("platform_id"),
            "os": env.get("os"),
            "arch": env.get("arch"),
            "hostname": env.get("hostname"),
            "cpu": env.get("cpu"),
            "gpus": env.get("gpus", []),
            "npu": env.get("npu"),
            "driver": env.get("driver"),
            "cuda": env.get("cuda"),
            "runtimes": env.get("runtimes", {}),
        },
        "package": package,
        "assertions": assertions or [],
        "waiver": waiver,
        "log_path": log_path,
        "notes": notes,
        "owner": owner,
        "timestamp": now_iso(),
    }


def validate_record(rec: Dict[str, Any]) -> List[str]:
    """Return a list of human-readable validation errors (empty == valid)."""
    errs: List[str] = []
    required = ["schema_version", "cell_id", "sdk", "feature", "result",
                "category", "blocking", "env", "package", "timestamp"]
    for k in required:
        if k not in rec:
            errs.append(f"missing required key: {k}")
    if rec.get("schema_version") != SCHEMA_VERSION:
        errs.append(f"schema_version must be {SCHEMA_VERSION}")
    if rec.get("result") not in RESULT_CLASSES:
        errs.append(f"result '{rec.get('result')}' not in {sorted(RESULT_CLASSES)}")
    if rec.get("category") not in CATEGORIES:
        errs.append(f"category '{rec.get('category')}' not in {sorted(CATEGORIES)}")
    if rec.get("sdk") not in SDKS:
        errs.append(f"sdk '{rec.get('sdk')}' not in {sorted(SDKS)}")
    if rec.get("accelerator") not in ACCELERATORS:
        errs.append(f"accelerator '{rec.get('accelerator')}' not in {sorted(ACCELERATORS)}")
    if not isinstance(rec.get("blocking"), bool):
        errs.append("blocking must be a boolean")
    env = rec.get("env") or {}
    for k in ("platform_id", "os", "arch"):
        if not env.get(k):
            errs.append(f"env.{k} is required")
    pkg = rec.get("package") or {}
    for k in ("name", "version"):
        if k not in pkg:
            errs.append(f"package.{k} is required")
    return errs


def write_results(records: List[Dict[str, Any]], out_dir: str, run_id: str, hostname: str) -> str:
    os.makedirs(out_dir, exist_ok=True)
    fname = f"{hostname}__{run_id}.json"
    path = os.path.join(out_dir, fname)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(records, f, indent=2)
    return path


def summarize(records: List[Dict[str, Any]]) -> Dict[str, int]:
    counts: Dict[str, int] = {c: 0 for c in RESULT_CLASSES}
    for r in records:
        counts[r.get("result", "skipped")] = counts.get(r.get("result", "skipped"), 0) + 1
    return counts


def blocking_failures(records: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    return [r for r in records if r.get("blocking") and r.get("result") == "fail"]


def write_markdown_summary(records: List[Dict[str, Any]], out_dir: str, run_id: str, hostname: str) -> str:
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, f"{hostname}__{run_id}.md")
    counts = summarize(records)
    env = records[0]["env"] if records else {}
    lines: List[str] = []
    lines.append(f"# Validation results — {hostname} ({run_id})")
    lines.append("")
    if env:
        lines.append(f"- Platform: `{env.get('platform_id')}` ({env.get('os')}/{env.get('arch')})")
        lines.append(f"- CPU: {env.get('cpu')}")
        lines.append(f"- GPUs: {', '.join(env.get('gpus') or []) or 'none detected'}")
        lines.append(f"- CUDA: {env.get('cuda') or 'n/a'}")
        lines.append("")
    lines.append("## Totals")
    lines.append("")
    lines.append("| result | count |")
    lines.append("|--------|-------|")
    for k in ["pass", "fail", "blocked", "waived", "n-a", "skipped"]:
        lines.append(f"| {k} | {counts.get(k, 0)} |")
    lines.append("")
    lines.append("## Cells")
    lines.append("")
    lines.append("| sdk | feature | accel | model | result | blocking | notes |")
    lines.append("|-----|---------|-------|-------|--------|----------|-------|")
    for r in sorted(records, key=lambda x: (x["sdk"], x["feature"], x["accelerator"])):
        model = (r.get("model") or {}).get("alias") or "-"
        note = (r.get("notes") or "").replace("|", "\\|")[:80]
        lines.append(
            f"| {r['sdk']} | {r['feature']} | {r['accelerator']} | {model} | "
            f"**{r['result']}** | {'yes' if r['blocking'] else 'no'} | {note} |"
        )
    lines.append("")
    bf = blocking_failures(records)
    if bf:
        lines.append("## ❌ Blocking failures (go/no-go blockers)")
        lines.append("")
        for r in bf:
            lines.append(f"- `{r['cell_id']}` — {r.get('notes') or 'see log'}")
        lines.append("")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    return path
