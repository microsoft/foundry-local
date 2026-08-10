"""Per-SDK cell runners.

Each runner receives a `cell` dict and a `ctx` dict and returns a result dict:
    { "result": <class>, "assertions": [...], "notes": str, "package": {...},
      "duration_seconds": float, "log_path": str|None }

Runners do real work against the ORT-Nightly feed in an isolated workspace. When the
feed env vars are not configured, or a runner has no automation on this agent yet, the
runner returns a non-fatal result ("blocked"/"skipped") with a clear note pointing at
the manual procedure in validation/VALIDATION.md — the harness itself never crashes.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import time
from typing import Any, Dict, List

import feeds

RC_VERSIONS = {
    "python": "2.0.0rc1",
    "js": "2.0.0-rc1",
    "cs": "2.0.0-rc1",
    "cpp": "2.0.0-rc1",
}
PKG_NAMES = {
    "python": "foundry-local-sdk",
    "js": "foundry-local-sdk",
    "cs": "Microsoft.AI.Foundry.Local",
    "cpp": "Microsoft.AI.Foundry.Local.Runtime",
}


def _assert(name: str, ok: bool, detail: str | None = None) -> Dict[str, Any]:
    return {"name": name, "ok": bool(ok), "detail": detail}


def _pkg(sdk: str, resolved_source: str | None = None) -> Dict[str, Any]:
    return {
        "name": PKG_NAMES[sdk],
        "version": RC_VERSIONS[sdk],
        "resolved_source": resolved_source,
        "sha256": None,
        "native_lib_versions": {},
    }


def _exec(cmd: List[str], cwd: str, log, timeout: int, env: Dict[str, str] | None = None) -> int:
    log.write(f"\n$ {' '.join(cmd)}  (cwd={cwd})\n")
    log.flush()
    try:
        p = subprocess.run(cmd, cwd=cwd, timeout=timeout, env=env,
                           capture_output=True, text=True)
        log.write(p.stdout or "")
        log.write(p.stderr or "")
        log.flush()
        return p.returncode
    except subprocess.TimeoutExpired:
        log.write(f"\n[TIMEOUT after {timeout}s]\n")
        return 124
    except FileNotFoundError as e:
        log.write(f"\n[TOOL NOT FOUND] {e}\n")
        return 127


def _blocked_feed(sdk: str, var: str) -> Dict[str, Any]:
    return {
        "result": "blocked",
        "assertions": [],
        "notes": f"feed not configured: set ${var} (see VALIDATION.md) to run {sdk} cells",
        "package": _pkg(sdk),
        "duration_seconds": 0.0,
        "log_path": None,
    }


# --------------------------------------------------------------------------------------
# install-smoke: install RC package into a fresh isolated project and assert version.
# --------------------------------------------------------------------------------------

def run_install_smoke(cell: Dict[str, Any], ctx: Dict[str, Any]) -> Dict[str, Any]:
    sdk = cell["sdk"]
    var = feeds.missing_feed(sdk)
    if var:
        return _blocked_feed(sdk, var)
    ws = ctx["cell_workspace"]
    os.makedirs(ws, exist_ok=True)
    log_path = os.path.join(ws, "install-smoke.log")
    t0 = time.time()
    with open(log_path, "w", encoding="utf-8") as log:
        if sdk == "python":
            ok, notes = _install_smoke_python(ws, log, ctx)
        elif sdk == "js":
            ok, notes = _install_smoke_js(ws, log, ctx)
        elif sdk == "cs":
            ok, notes = _install_smoke_cs(ws, log, ctx)
        elif sdk == "cpp":
            ok, notes = _install_smoke_cpp(ws, log, ctx)
        else:
            ok, notes = False, f"unknown sdk {sdk}"
    return {
        "result": "pass" if ok else "fail",
        "assertions": [_assert("package installs and reports RC version", ok, notes)],
        "notes": notes,
        "package": _pkg(sdk, feeds.feed_env().get(_feed_key(sdk))),
        "duration_seconds": round(time.time() - t0, 1),
        "log_path": log_path,
    }


def _feed_key(sdk: str) -> str:
    return {"python": "pip", "js": "npm", "cs": "nuget", "cpp": "nuget"}[sdk]


def _install_smoke_python(ws: str, log, ctx: Dict[str, Any]) -> tuple[bool, str]:
    import sys
    venv = os.path.join(ws, "venv")
    rc = _exec([sys.executable, "-m", "venv", venv], ws, log, 120)
    if rc != 0:
        return False, "venv creation failed"
    py = os.path.join(venv, "Scripts", "python.exe") if os.name == "nt" else os.path.join(venv, "bin", "python")
    pip = feeds.pip_install_args(ws)
    rc = _exec([py, "-m", "pip", "install", "--no-input", "--cache-dir", pip["cache_dir"],
                "--index-url", pip["index_url"],
                f"{PKG_NAMES['python']}=={RC_VERSIONS['python']}"], ws, log, ctx["install_timeout"])
    if rc != 0:
        return False, "pip install failed"
    check = ("import importlib.metadata as m;"
             "import foundry_local_sdk as f;"
             "print('VERSION=' + m.version('foundry-local-sdk'))")
    out_path = os.path.join(ws, "pyver.txt")
    rc = _exec([py, "-c", check + f";open(r'{out_path}','w').write(m.version('foundry-local-sdk'))"],
               ws, log, 60)
    if rc != 0:
        return False, "import foundry_local_sdk failed (native load?)"
    ver = _read(out_path)
    ok = ver.startswith("2.0.0rc1") or ver.startswith("2.0.0-rc1")
    return ok, f"installed version={ver}"


def _install_smoke_js(ws: str, log, ctx: Dict[str, Any]) -> tuple[bool, str]:
    feeds.write_npmrc(ws)
    with open(os.path.join(ws, "package.json"), "w", encoding="utf-8") as f:
        f.write('{"name":"fl-smoke","version":"1.0.0","type":"module","private":true}\n')
    rc = _exec(["npm", "install", "--no-audit", "--no-fund",
                f"{PKG_NAMES['js']}@{RC_VERSIONS['js']}"], ws, log, ctx["install_timeout"])
    if rc != 0:
        return False, "npm install failed"
    script = ("import {createRequire} from 'module';const r=createRequire(import.meta.url);"
              "const p=r('foundry-local-sdk/package.json');console.log('VERSION='+p.version);")
    js = os.path.join(ws, "smoke.mjs")
    with open(js, "w", encoding="utf-8") as f:
        f.write(script)
    out_path = os.path.join(ws, "jsver.txt")
    rc = _exec(["node", "-e",
                f"const{{createRequire}}=require('module');const r=createRequire('{ws}/x');"
                f"const p=r('foundry-local-sdk/package.json');require('fs').writeFileSync('{out_path}',p.version);"],
               ws, log, 60)
    if rc != 0:
        return False, "resolving foundry-local-sdk failed"
    ver = _read(out_path)
    return ver.startswith("2.0.0-rc1"), f"installed version={ver}"


def _install_smoke_cs(ws: str, log, ctx: Dict[str, Any]) -> tuple[bool, str]:
    feeds.write_nuget_config(ws)
    proj = os.path.join(ws, "smoke")
    os.makedirs(proj, exist_ok=True)
    rc = _exec(["dotnet", "new", "console", "-o", proj, "--force"], ws, log, 180)
    if rc != 0:
        return False, "dotnet new failed"
    shutil.copy(os.path.join(ws, "nuget.config"), os.path.join(proj, "nuget.config"))
    rc = _exec(["dotnet", "add", proj, "package", PKG_NAMES["cs"],
                "--version", RC_VERSIONS["cs"]], ws, log, ctx["install_timeout"])
    if rc != 0:
        return False, "dotnet add package failed"
    rc = _exec(["dotnet", "build", proj, "-c", "Release"], ws, log, ctx["install_timeout"])
    if rc != 0:
        return False, "dotnet build failed"
    return True, f"restored+built {PKG_NAMES['cs']} {RC_VERSIONS['cs']}"


def _install_smoke_cpp(ws: str, log, ctx: Dict[str, Any]) -> tuple[bool, str]:
    feeds.write_nuget_config(ws)
    proj = os.path.join(ws, "cpp-smoke")
    os.makedirs(proj, exist_ok=True)
    # Restore the runtime NuGet package and assert the native library is present.
    csproj = os.path.join(proj, "restore.csproj")
    with open(csproj, "w", encoding="utf-8") as f:
        f.write(
            '<Project Sdk="Microsoft.NET.Sdk">\n'
            '  <PropertyGroup><TargetFramework>net9.0</TargetFramework>'
            '<RestorePackagesPath>packages</RestorePackagesPath></PropertyGroup>\n'
            '  <ItemGroup><PackageReference Include="' + PKG_NAMES["cpp"] +
            '" Version="' + RC_VERSIONS["cpp"] + '" /></ItemGroup>\n'
            '</Project>\n')
    shutil.copy(os.path.join(ws, "nuget.config"), os.path.join(proj, "nuget.config"))
    rc = _exec(["dotnet", "restore", csproj], proj, log, ctx["install_timeout"])
    if rc != 0:
        return False, "nuget restore of runtime package failed"
    # Look for a native lib (foundry_local.{dll,so,dylib}) in the restored package tree.
    found = _find_native(os.path.join(proj, "packages"))
    return bool(found), (f"native lib present: {found}" if found else "native library not found in package")


def _find_native(root: str) -> str | None:
    for base, _dirs, files in os.walk(root):
        for fn in files:
            low = fn.lower()
            if low.startswith("foundry_local") and low.endswith((".dll", ".so", ".dylib")):
                return os.path.join(base, fn)
    return None


def _read(path: str) -> str:
    try:
        with open(path, encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return ""


# --------------------------------------------------------------------------------------
# Feature runners routed through existing sample projects (chat, embeddings, ...).
# Fully automating each is per-agent work; until a sample-backed runner is wired for a
# given (sdk, feature) we return a non-fatal 'skipped' with a pointer to the manual
# procedure. This keeps the matrix honest without crashing or falsely reporting pass.
# --------------------------------------------------------------------------------------

def run_pending(cell: Dict[str, Any], ctx: Dict[str, Any]) -> Dict[str, Any]:
    return {
        "result": "skipped",
        "assertions": [],
        "notes": (f"automated runner for feature '{cell['feature']}' not yet wired on this agent; "
                  f"follow the manual procedure in VALIDATION.md and record the result"),
        "package": _pkg(cell["sdk"]),
        "duration_seconds": 0.0,
        "log_path": None,
    }


# --------------------------------------------------------------------------------------
# pkg-inspect: download the RC artifact from the feed and inspect its contents/identity.
# --------------------------------------------------------------------------------------

def run_pkg_inspect(cell: Dict[str, Any], ctx: Dict[str, Any]) -> Dict[str, Any]:
    """Download the RC package artifact and inspect contents/identity/provenance.

    Fetching the raw artifact differs per ecosystem (nuget/npm/pip). Rather than couple to
    each feed's download API, we reuse the isolated install performed by install-smoke to
    populate the local cache, then locate and inspect the downloaded artifact with
    pkg_inspect. If the feed is not configured we return a non-fatal 'blocked'.
    """
    import pkg_inspect  # local module, stdlib-only
    sdk = cell["sdk"]
    var = feeds.missing_feed(sdk)
    if var:
        return _blocked_feed(sdk, var)
    ws = ctx["cell_workspace"]
    os.makedirs(ws, exist_ok=True)
    log_path = os.path.join(ws, "pkg-inspect.log")
    t0 = time.time()
    with open(log_path, "w", encoding="utf-8") as log:
        # Reuse install-smoke to fetch the artifact into the isolated cache.
        smoke = run_install_smoke(cell, {**ctx, "cell_workspace": ws})
        artifact = _find_artifact(sdk, ws)
        log.write(f"\nlocated artifact: {artifact}\n")
    if not artifact:
        return {
            "result": "blocked",
            "assertions": [_assert("artifact located for inspection", False,
                                   "could not find downloaded .nupkg/.whl/.tgz in isolated cache")],
            "notes": "install step did not yield an inspectable artifact (see log)",
            "package": _pkg(sdk),
            "duration_seconds": round(time.time() - t0, 1),
            "log_path": log_path,
        }
    report = pkg_inspect.inspect(artifact)
    asserts = pkg_inspect.to_assertions(report, PKG_NAMES[sdk], RC_VERSIONS[sdk])
    ok = all(a["ok"] for a in asserts)
    pkg = _pkg(sdk, feeds.feed_env().get(_feed_key(sdk)))
    pkg["sha256"] = report.get("sha256")
    return {
        "result": "pass" if ok else "fail",
        "assertions": asserts,
        "notes": f"inspected {os.path.basename(artifact)}",
        "package": pkg,
        "duration_seconds": round(time.time() - t0, 1),
        "log_path": log_path,
    }


def _find_artifact(sdk: str, ws: str) -> str | None:
    exts = {"python": (".whl",), "js": (".tgz",), "cs": (".nupkg",), "cpp": (".nupkg",)}[sdk]
    token = PKG_NAMES[sdk].lower().replace(".", "").replace("-", "")
    best = None
    for base, _dirs, files in os.walk(ws):
        for fn in files:
            low = fn.lower()
            if low.endswith(exts) and token[:12] in low.replace(".", "").replace("-", "").replace("_", ""):
                best = os.path.join(base, fn)
    if best:
        return best
    # Fallback: any matching-extension file in the workspace.
    for base, _dirs, files in os.walk(ws):
        for fn in files:
            if fn.lower().endswith(exts):
                return os.path.join(base, fn)
    return None


# Import sample-backed feature runners (author: sample_runner.py). Imported lazily-safe:
# if the module is missing for any reason, fall back to run_pending so dispatch never breaks.
try:
    import sample_runner as _sr
    _SAMPLE = {
        "chat": _sr.run_chat,
        "tool_calling": _sr.run_tool_calling,
        "embeddings": _sr.run_embeddings,
        "audio_file": _sr.run_audio_file,
        "audio_stream": _sr.run_audio_stream,
        "vision": _sr.run_vision,
        "web_server": _sr.run_web_server,
        "integrations": _sr.run_integrations,
        "model_mgmt": _sr.run_model_mgmt,
    }
except Exception:  # pragma: no cover - defensive
    _SAMPLE = {}


# Dispatch table: feature runner name -> callable. install-smoke and pkg-inspect are fully
# automated; feature cells route to sample-backed runners; the remaining runtime/non-
# functional cells (ep_bootstrap, model_mgmt_fail, crosscutting, soak, compat) are staged
# hooks the platform agents extend, and stay honest (skipped) until wired.
RUNNERS = {
    "install_smoke": run_install_smoke,
    "pkg_inspect": run_pkg_inspect,
    "chat": _SAMPLE.get("chat", run_pending),
    "tool_calling": _SAMPLE.get("tool_calling", run_pending),
    "embeddings": _SAMPLE.get("embeddings", run_pending),
    "audio_file": _SAMPLE.get("audio_file", run_pending),
    "audio_stream": _SAMPLE.get("audio_stream", run_pending),
    "vision": _SAMPLE.get("vision", run_pending),
    "web_server": _SAMPLE.get("web_server", run_pending),
    "integrations": _SAMPLE.get("integrations", run_pending),
    "model_mgmt": _SAMPLE.get("model_mgmt", run_pending),
    "model_mgmt_fail": run_pending,
    "ep_bootstrap": run_pending,
    "crosscutting": run_pending,
    "soak_resource": run_pending,
    "compat_upgrade": run_pending,
}


def dispatch(runner_name: str, cell: Dict[str, Any], ctx: Dict[str, Any]) -> Dict[str, Any]:
    fn = RUNNERS.get(runner_name, run_pending)
    try:
        return fn(cell, ctx)
    except Exception as e:  # a runner bug must not sink the whole run
        return {
            "result": "blocked",
            "assertions": [_assert("runner executed without error", False, repr(e))],
            "notes": f"runner raised: {e!r}",
            "package": _pkg(cell["sdk"]),
            "duration_seconds": 0.0,
            "log_path": None,
        }
