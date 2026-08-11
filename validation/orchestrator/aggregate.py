#!/usr/bin/env python3
"""Merge all per-machine result files in validation/results/ into one combined
go/no-go checklist and print a provisional verdict.

Cross-machine merge rule: cells are keyed by cell_id; the most severe result wins so a
failure on any machine surfaces (fail > blocked > pass > waived > n-a > skipped, with the
latest timestamp breaking ties within the same severity). Stdlib-only.
"""
from __future__ import annotations

import glob
import json
import os
import sys
from typing import Any, Dict, List

# Windows consoles default to a legacy code page (cp1252) that can't encode the ✅/❌ status
# glyphs this script prints; force UTF-8 so the verdict line doesn't crash on Windows.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

HERE = os.path.dirname(os.path.abspath(__file__))
RESULTS_DIR = os.path.join(os.path.dirname(HERE), "results")

SEVERITY = {"fail": 0, "blocked": 1, "pass": 2, "waived": 3, "n-a": 4, "skipped": 5}


def load_all() -> List[Dict[str, Any]]:
    recs: List[Dict[str, Any]] = []
    for p in sorted(glob.glob(os.path.join(RESULTS_DIR, "*.json"))):
        try:
            data = json.load(open(p, encoding="utf-8"))
        except Exception as e:
            print(f"skip {p}: {e}")
            continue
        if isinstance(data, list):
            recs.extend(data)
    return recs


def merge(recs: List[Dict[str, Any]]) -> Dict[str, Dict[str, Any]]:
    best: Dict[str, Dict[str, Any]] = {}
    for r in recs:
        cid = r["cell_id"]
        cur = best.get(cid)
        if cur is None:
            best[cid] = r
            continue
        rs, cs = SEVERITY.get(r["result"], 9), SEVERITY.get(cur["result"], 9)
        if rs < cs or (rs == cs and r.get("timestamp", "") > cur.get("timestamp", "")):
            best[cid] = r
    return best


def main() -> int:
    recs = load_all()
    if not recs:
        print(f"No result files found in {RESULTS_DIR}. Run the harness first.")
        return 0
    merged = merge(recs)
    cells = sorted(merged.values(), key=lambda x: (x["sdk"], x["feature"], x["accelerator"]))

    counts: Dict[str, int] = {}
    for c in cells:
        counts[c["result"]] = counts.get(c["result"], 0) + 1

    blocking_fail = [c for c in cells if c.get("blocking") and c["result"] == "fail"]
    blocking_blocked = [c for c in cells if c.get("blocking") and c["result"] == "blocked"]
    blocking_pending = [c for c in cells if c.get("blocking") and c["result"] in ("skipped",)]

    machines = sorted({r["env"]["hostname"] for r in recs if r.get("env", {}).get("hostname")})
    platforms = sorted({r["env"]["platform_id"] for r in recs if r.get("env", {}).get("platform_id")})

    go = not blocking_fail and not blocking_blocked and not blocking_pending
    verdict = "GO ✅" if go else "NO-GO ❌"

    lines: List[str] = []
    lines.append("# Combined validation checklist (generated)")
    lines.append("")
    lines.append(f"- Machines reporting: {', '.join(machines) or 'none'}")
    lines.append(f"- Platforms covered: {', '.join(platforms) or 'none'}")
    lines.append(f"- Cells (deduplicated): {len(cells)}")
    lines.append("")
    lines.append(f"## Provisional verdict: **{verdict}**")
    lines.append("")
    lines.append("> Provisional and mechanical: it only checks that GA-blocking cells are "
                 "pass/waived. The release lead makes the final call per ACCEPTANCE_POLICY.md, "
                 "including manifest/`n-a` justification and GA-artifact equivalence.")
    lines.append("")
    lines.append("## Totals")
    lines.append("")
    lines.append("| result | count |")
    lines.append("|--------|-------|")
    for k in ["pass", "fail", "blocked", "waived", "n-a", "skipped"]:
        lines.append(f"| {k} | {counts.get(k, 0)} |")
    lines.append("")
    if blocking_fail or blocking_blocked or blocking_pending:
        lines.append("## GA-blocking cells needing attention")
        lines.append("")
        lines.append("| cell_id | result | machine | notes |")
        lines.append("|---------|--------|---------|-------|")
        for c in blocking_fail + blocking_blocked + blocking_pending:
            host = c.get("env", {}).get("hostname", "?")
            note = (c.get("notes") or "").replace("|", "\\|")[:80]
            lines.append(f"| {c['cell_id']} | **{c['result']}** | {host} | {note} |")
        lines.append("")
    lines.append("## All cells")
    lines.append("")
    lines.append("| sdk | feature | accel | model | result | blocking | machine |")
    lines.append("|-----|---------|-------|-------|--------|----------|---------|")
    for c in cells:
        model = (c.get("model") or {}).get("alias") or "-"
        host = c.get("env", {}).get("hostname", "?")
        lines.append(f"| {c['sdk']} | {c['feature']} | {c['accelerator']} | {model} | "
                     f"**{c['result']}** | {'yes' if c.get('blocking') else 'no'} | {host} |")
    lines.append("")

    out = os.path.join(RESULTS_DIR, "COMBINED_CHECKLIST.md")
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"Wrote {out}")
    print(f"Provisional verdict: {verdict}")
    if not go:
        n = len(blocking_fail) + len(blocking_blocked) + len(blocking_pending)
        print(f"  {n} GA-blocking cell(s) not yet pass/waived.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
