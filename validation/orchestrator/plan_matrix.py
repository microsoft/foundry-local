#!/usr/bin/env python3
"""Generate the cross-platform validation tracking matrix.

Unlike ``aggregate.py`` (which reports only what has already been executed), this produces the
*planned* go/no-go matrix for every target platform in ``platform_manifest.json`` up front, so a
release lead has a single tracking surface before any machine has run. If per-machine result
files exist under ``results/``, their outcomes are overlaid so the same doc doubles as a live
tracker as coverage lands.

Status legend:
  NOT RUN  - planned cell, no result recorded yet (the default before execution)
  pass/fail/blocked/waived/n-a/skipped - taken from a committed result record for that cell

Usage:  python3 orchestrator/plan_matrix.py [--out ../STATUS_MATRIX.md]
Writes the matrix (default: ``validation/STATUS_MATRIX.md``) and prints planned-coverage counts.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
from typing import Any, Dict, List

import run_validation as rv

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RESULTS_DIR = os.path.join(ROOT, "results")
DEFAULT_OUT = os.path.join(ROOT, "STATUS_MATRIX.md")


def load_recorded_results() -> Dict[str, Dict[str, Any]]:
    """Map cell_id -> most-recent recorded result across all committed result files."""
    recorded: Dict[str, Dict[str, Any]] = {}
    for path in sorted(glob.glob(os.path.join(RESULTS_DIR, "*.json"))):
        try:
            records = json.load(open(path, encoding="utf-8"))
        except (ValueError, OSError):
            continue
        for rec in records if isinstance(records, list) else []:
            cid = rec.get("cell_id")
            if cid:
                recorded[cid] = {"result": rec.get("result"), "source": os.path.basename(path)}
    return recorded


def planned_cells(manifest: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Every planned cell across all target platforms (on-surface accelerators only)."""
    cells: List[Dict[str, Any]] = []
    for pentry in manifest["platform"]["platforms"]:
        pid = pentry["id"]
        accels = pentry["accelerators"]
        for cell in rv.expand_cells(manifest, pid, accels):
            cell["platform_id"] = pid
            cells.append(cell)
    return cells


def status_for(cell: Dict[str, Any], recorded: Dict[str, Dict[str, Any]]) -> str:
    rec = recorded.get(cell["cell_id"])
    if rec and rec.get("result"):
        return rec["result"]
    if cell.get("provisional") == "n-a":
        return "n-a"
    return "NOT RUN"


def scope_matrix(manifest: Dict[str, Any]) -> str:
    """Compact feature x SDK applicability table."""
    cfg = manifest["cells"]
    sdks = cfg["sdks"]
    overrides = cfg.get("feature_sdk_applicability", {})
    lines = ["| feature | blocking | " + " | ".join(sdks) + " |",
             "|---|---|" + "|".join(["---"] * len(sdks)) + "|"]
    for feat in cfg["features"]:
        applic = rv.applicable_sdks(feat, sdks, overrides)
        cols = ["yes" if s in applic else "n-a" for s in sdks]
        block = "**yes**" if feat["blocking"] else "no"
        lines.append(f"| {feat['id']} | {block} | " + " | ".join(cols) + " |")
    return "\n".join(lines)


def platform_table(manifest: Dict[str, Any]) -> str:
    lines = ["| platform_id | os | arch | accelerators (support surface) |",
             "|---|---|---|---|"]
    for p in manifest["platform"]["platforms"]:
        lines.append(f"| {p['id']} | {p['os']} | {p['arch']} | {', '.join(p['accelerators'])} |")
    return "\n".join(lines)


def tracker_table(cells: List[Dict[str, Any]], recorded: Dict[str, Dict[str, Any]]) -> str:
    """Per-cell go/no-go tracker, runnable cells only (n-a rows omitted for signal)."""
    lines = ["| platform | sdk | feature | accelerator | blocking | status |",
             "|---|---|---|---|---|---|"]
    for c in sorted(cells, key=lambda x: (x["platform_id"], x["feature"], x["sdk"], x["accelerator"])):
        if c.get("provisional") == "n-a":
            continue
        st = status_for(c, recorded)
        block = "**yes**" if c["blocking"] else "no"
        lines.append(f"| {c['platform_id']} | {c['sdk']} | {c['feature']} | {c['accelerator']} "
                     f"| {block} | {st} |")
    return "\n".join(lines)


def summarize(cells: List[Dict[str, Any]], recorded: Dict[str, Dict[str, Any]]) -> Dict[str, int]:
    counts = {"planned_runnable": 0, "n-a": 0, "NOT RUN": 0,
              "pass": 0, "fail": 0, "blocked": 0, "waived": 0, "skipped": 0}
    for c in cells:
        if c.get("provisional") == "n-a":
            counts["n-a"] += 1
            continue
        counts["planned_runnable"] += 1
        counts[status_for(c, recorded)] = counts.get(status_for(c, recorded), 0) + 1
    return counts


def render(manifest: Dict[str, Any]) -> str:
    recorded = load_recorded_results()
    cells = planned_cells(manifest)
    counts = summarize(cells, recorded)
    executed = counts["pass"] + counts["fail"] + counts["blocked"] + counts["waived"] + counts["skipped"]
    rel = manifest["platform"]["release"]

    out: List[str] = []
    out.append("# Foundry Local 2.0.0-rc1 — Validation Tracking Matrix")
    out.append("")
    out.append("_Generated by `orchestrator/plan_matrix.py`. This is the **planned** go/no-go matrix "
               "across all target platforms; recorded results are overlaid as they are committed._")
    out.append("")
    out.append(f"- Release: **{rel['version']}**  ·  branch `{rel['branch']}`  ·  feed: {rel['feed']}")
    out.append(f"- Planned runnable cells: **{counts['planned_runnable']}**  ·  "
               f"n-a (off support-surface): {counts['n-a']}")
    out.append(f"- Executed so far: **{executed}** / {counts['planned_runnable']}  "
               f"(pass {counts['pass']} · fail {counts['fail']} · blocked {counts['blocked']} · "
               f"waived {counts['waived']} · skipped {counts['skipped']})")
    out.append(f"- **NOT RUN: {counts['NOT RUN']}**")
    out.append("")
    if executed == 0:
        out.append("> ⚠️ **No cells have been executed yet.** Nothing has passed or failed. Execution is "
                   "blocked on (a) ADO ORT-Nightly feed credentials and (b) provisioned Windows/Linux/"
                   "macOS + GPU/NPU agents. Run `run.sh` / `run.ps1` on each machine, commit the "
                   "`results/*.json` file, then regenerate this matrix and `COMBINED_CHECKLIST.md`.")
        out.append("")
    out.append("## Feature × SDK scope")
    out.append("")
    out.append(scope_matrix(manifest))
    out.append("")
    out.append("## Target platforms & accelerator support surface")
    out.append("")
    out.append(platform_table(manifest))
    out.append("")
    out.append("## Go / No-Go cell tracker")
    out.append("")
    out.append("One row per planned runnable cell. `status` is `NOT RUN` until a result record for that "
               "cell is committed under `results/`, then it reflects the recorded outcome.")
    out.append("")
    out.append(tracker_table(cells, recorded))
    out.append("")
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate the cross-platform validation tracking matrix.")
    ap.add_argument("--out", default=DEFAULT_OUT, help="output markdown path")
    args = ap.parse_args()

    manifest = rv.load_manifests()
    doc = render(manifest)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(doc)

    recorded = load_recorded_results()
    counts = summarize(planned_cells(manifest), recorded)
    print(f"Wrote {args.out}")
    print(f"planned runnable cells : {counts['planned_runnable']}")
    print(f"n-a (off-surface)      : {counts['n-a']}")
    print(f"NOT RUN                : {counts['NOT RUN']}")
    print(f"executed               : "
          f"{counts['pass'] + counts['fail'] + counts['blocked'] + counts['waived'] + counts['skipped']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
