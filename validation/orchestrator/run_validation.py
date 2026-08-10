#!/usr/bin/env python3
"""Foundry Local 2.0.0 — cross-platform validation orchestrator.

Runs the SAME suite on any Windows/Linux/macOS agent. It:
  1. Fingerprints the machine and maps it to a platform in the manifest.
  2. Expands the coverage-cell catalog into concrete {sdk x feature x accelerator} cells.
  3. Filters to what this machine supports (unsupported accelerators -> n-a per manifest).
  4. Runs each selected cell (or simulates), emitting one schema-valid record per cell.
  5. Writes a per-machine results JSON + markdown summary into validation/results/.

Stdlib-only. Example:
  python run_validation.py --list
  python run_validation.py --simulate
  python run_validation.py --sdk python,js --feature install-smoke,chat
  python run_validation.py --sdk cs --feature install-smoke
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import uuid
from typing import Any, Dict, List

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import envinfo  # noqa: E402
import results as R  # noqa: E402
import runners  # noqa: E402

VALIDATION_ROOT = os.path.dirname(HERE)
MANIFEST_DIR = os.path.join(VALIDATION_ROOT, "manifests")
DEFAULT_RESULTS_DIR = os.path.join(VALIDATION_ROOT, "results")
REPO_ROOT = os.path.dirname(VALIDATION_ROOT)


def load_json(path: str) -> Dict[str, Any]:
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def load_manifests() -> Dict[str, Any]:
    return {
        "platform": load_json(os.path.join(MANIFEST_DIR, "platform_manifest.json")),
        "availability": load_json(os.path.join(MANIFEST_DIR, "model_ep_availability.json")),
        "cells": load_json(os.path.join(MANIFEST_DIR, "coverage_cells.json")),
    }


def platform_entry(manifest: Dict[str, Any], platform_id: str) -> Dict[str, Any] | None:
    for p in manifest["platform"]["platforms"]:
        if p["id"] == platform_id:
            return p
    return None


def model_for_task(manifest: Dict[str, Any], task: str | None, accel: str) -> Dict[str, Any] | None:
    if not task:
        return None
    avail = manifest["availability"]
    suffix = avail["variant_suffixes"].get(accel)
    for m in avail["models"]:
        if m["task"] == task:
            variant = f"{m['alias']}-{suffix}" if suffix else None
            expected = m.get("expected", {}).get(accel, False)
            return {"task": task, "alias": m["alias"], "variant": variant, "expected": expected}
    return {"task": task, "alias": None, "variant": None, "expected": False}


def applicable_sdks(feature: Dict[str, Any], all_sdks: List[str], overrides: Dict[str, Any]) -> List[str]:
    ov = overrides.get(feature["id"])
    return ov if ov else all_sdks


def expand_cells(manifest: Dict[str, Any], platform_id: str, available_accels: List[str]) -> List[Dict[str, Any]]:
    """Produce every candidate cell for this platform with a provisional result.

    - accelerator applies to platform but not physically present -> 'blocked' (needs hardware)
    - accelerator not applicable to platform at all               -> 'n-a'
    - otherwise                                                    -> to-run (result decided by runner)
    """
    cells_cfg = manifest["cells"]
    all_sdks = cells_cfg["sdks"]
    overrides = cells_cfg.get("feature_sdk_applicability", {})
    pentry = platform_entry(manifest, platform_id)
    platform_accels = set(pentry["accelerators"]) if pentry else set(available_accels)

    out: List[Dict[str, Any]] = []
    for feat in cells_cfg["features"]:
        sdks = applicable_sdks(feat, all_sdks, overrides)
        for sdk in sdks:
            for accel in feat["accelerators"]:
                provisional = None
                if accel not in platform_accels:
                    provisional = "n-a"           # not part of this platform's support surface
                elif accel not in available_accels:
                    provisional = "blocked"        # supported on platform but no hardware here
                model = model_for_task(manifest, feat.get("model_task"), accel)
                # Missing-but-expected variant would be a failure; the runner confirms at run time.
                out.append({
                    "cell_id": f"{sdk}__{feat['id']}__{platform_id}__{accel}",
                    "sdk": sdk,
                    "feature": feat["id"],
                    "runner": feat["runner"],
                    "category": feat["category"],
                    "blocking": feat["blocking"],
                    "accelerator": accel,
                    "model": model,
                    "provisional": provisional,
                    "desc": feat["desc"],
                })
    return out


def csv_set(val: str | None) -> set[str] | None:
    if not val:
        return None
    return {x.strip() for x in val.split(",") if x.strip()}


def main() -> int:
    ap = argparse.ArgumentParser(description="Foundry Local 2.0.0 validation orchestrator")
    ap.add_argument("--sdk", help="comma list: cpp,cs,js,python")
    ap.add_argument("--feature", help="comma list of feature ids (see coverage_cells.json)")
    ap.add_argument("--accelerator", help="comma list: cpu,cuda,winml-dml,npu-winml,coreml-metal,webgpu")
    ap.add_argument("--list", action="store_true", help="list selected cells and exit")
    ap.add_argument("--simulate", action="store_true", help="do not install/run; emit synthetic pass records")
    ap.add_argument("--output-dir", default=DEFAULT_RESULTS_DIR)
    ap.add_argument("--run-id", default=None)
    ap.add_argument("--install-timeout", type=int, default=1800)
    ap.add_argument("--cell-timeout", type=int, default=2400)
    ap.add_argument("--force-platform", help="override detected platform id (for dry runs)")
    args = ap.parse_args()

    env = envinfo.fingerprint()
    platform_id = args.force_platform or env["platform_id"]
    available_accels = env["available_accelerators"]
    run_id = args.run_id or __import__("datetime").datetime.now().strftime("%Y%m%d-%H%M%S")

    manifest = load_manifests()
    if not platform_entry(manifest, platform_id):
        print(f"WARNING: platform '{platform_id}' not in manifest; treating detected accelerators as support surface.",
              file=sys.stderr)

    cells = expand_cells(manifest, platform_id, available_accels)

    sel_sdk = csv_set(args.sdk)
    sel_feat = csv_set(args.feature)
    sel_accel = csv_set(args.accelerator)

    def selected(c: Dict[str, Any]) -> bool:
        if sel_sdk and c["sdk"] not in sel_sdk:
            return False
        if sel_feat and c["feature"] not in sel_feat:
            return False
        if sel_accel and c["accelerator"] not in sel_accel:
            return False
        return True

    cells = [c for c in cells if selected(c)]

    if args.list:
        print(f"Platform: {platform_id}  accelerators-present: {available_accels}")
        print(f"{'CELL':<52} {'CAT':<12} {'BLOCK':<6} PROVISIONAL")
        for c in cells:
            print(f"{c['cell_id']:<52} {c['category']:<12} "
                  f"{'yes' if c['blocking'] else 'no':<6} {c['provisional'] or '-'}")
        print(f"\n{len(cells)} cells selected.")
        return 0

    ctx = {
        "repo_root": REPO_ROOT,
        "validation_root": VALIDATION_ROOT,
        "run_id": run_id,
        "install_timeout": args.install_timeout,
        "cell_timeout": args.cell_timeout,
        "simulate": args.simulate,
    }

    records: List[Dict[str, Any]] = []
    ws_root = os.path.join(args.output_dir, f"work-{env['hostname']}-{run_id}")

    for c in cells:
        base = dict(
            cell_id=c["cell_id"], run_id=run_id, sdk=c["sdk"], feature=c["feature"],
            category=c["category"], blocking=c["blocking"], accelerator=c["accelerator"],
            env=env, model=c["model"],
        )
        if c["provisional"] in ("n-a", "blocked"):
            note = ("accelerator not part of this platform's support surface" if c["provisional"] == "n-a"
                    else "accelerator supported on platform but not present on this machine")
            rec = R.make_record(result=c["provisional"], notes=note,
                                package={"name": runners.PKG_NAMES[c["sdk"]],
                                         "version": runners.RC_VERSIONS[c["sdk"]]},
                                **base)
        elif args.simulate:
            rec = R.make_record(result="pass", notes="SIMULATED (no install/run performed)",
                                assertions=[{"name": "simulated", "ok": True, "detail": None}],
                                package={"name": runners.PKG_NAMES[c["sdk"]],
                                         "version": runners.RC_VERSIONS[c["sdk"]]},
                                **base)
        else:
            ctx["cell_workspace"] = os.path.join(ws_root, c["cell_id"])
            out = runners.dispatch(c["runner"], c, ctx)
            rec = R.make_record(result=out["result"], notes=out.get("notes"),
                                assertions=out.get("assertions", []),
                                package=out.get("package"),
                                duration_seconds=out.get("duration_seconds"),
                                log_path=out.get("log_path"), **base)
        errs = R.validate_record(rec)
        if errs:
            print(f"SCHEMA ERROR for {c['cell_id']}: {errs}", file=sys.stderr)
        records.append(rec)
        print(f"[{rec['result']:>7}] {c['cell_id']}"
              + (f"  ({rec['notes']})" if rec.get("notes") else ""))

    json_path = R.write_results(records, args.output_dir, run_id, env["hostname"])
    md_path = R.write_markdown_summary(records, args.output_dir, run_id, env["hostname"])
    counts = R.summarize(records)
    print("\n=== SUMMARY ===")
    for k in ["pass", "fail", "blocked", "waived", "n-a", "skipped"]:
        print(f"  {k:<8} {counts.get(k, 0)}")
    print(f"\nResults : {json_path}")
    print(f"Summary : {md_path}")

    bf = R.blocking_failures(records)
    if bf:
        print(f"\n❌ {len(bf)} BLOCKING FAILURE(S) — go/no-go blocker until triaged/waived.")
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
