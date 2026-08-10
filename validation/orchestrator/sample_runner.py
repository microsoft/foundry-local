"""Sample-backed feature runners for the release-validation harness.

The runner copies an existing sample into the cell workspace, pins it to the RC package,
installs/builds against the isolated feed, runs it with the cell model, and asserts on
both process success and an expected output marker.
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import time

import feeds
from runners import PKG_NAMES, RC_VERSIONS, _assert, _pkg

HERE = os.path.dirname(os.path.abspath(__file__))
VALIDATION_ROOT = os.path.dirname(HERE)
MAP_PATH = os.path.join(VALIDATION_ROOT, "manifests", "sample_map.json")


def run_chat(cell, ctx):
    return run_sample(cell, ctx)


def run_tool_calling(cell, ctx):
    return run_sample(cell, ctx)


def run_embeddings(cell, ctx):
    return run_sample(cell, ctx)


def run_audio_file(cell, ctx):
    return run_sample(cell, ctx)


def run_audio_stream(cell, ctx):
    return run_sample(cell, ctx)


def run_vision(cell, ctx):
    return run_sample(cell, ctx)


def run_web_server(cell, ctx):
    return run_sample(cell, ctx)


def run_integrations(cell, ctx):
    return run_sample(cell, ctx)


def run_model_mgmt(cell, ctx):
    return run_sample(cell, ctx)


def run_sample(cell, ctx):
    t0 = time.time()
    sdk = cell["sdk"]
    log_path = None
    try:
        spec = _sample_spec(cell)
        if not spec or not spec.get("applicable", False):
            note = spec.get("note") if spec else "no sample mapping for this sdk/feature"
            return _result("n-a", sdk, [], note, t0, None)

        var = feeds.missing_feed(sdk)
        if var:
            note = f"feed not configured: set ${var} (see VALIDATION.md) to run {sdk} sample cells"
            return _result("blocked", sdk, [], note, t0, None)

        ws = ctx["cell_workspace"]
        os.makedirs(ws, exist_ok=True)
        src = os.path.join(ctx["repo_root"], spec["path"])
        if not os.path.isdir(src):
            return _result("blocked", sdk, [], f"sample path does not exist: {spec['path']}", t0, None)

        sample_dir = os.path.join(ws, "sample")
        if os.path.exists(sample_dir):
            shutil.rmtree(sample_dir)
        shutil.copytree(src, sample_dir)
        _copy_support_files(sdk, src, ws)

        log_path = os.path.join(ws, "sample-run.log")
        with open(log_path, "w", encoding="utf-8") as log:
            log.write(f"cell={cell.get('cell_id')} sdk={sdk} feature={cell.get('feature')}\n")
            log.write(f"sample={spec['path']} model={_model_for_run(cell)}\n")
            _rewrite_model_aliases(sample_dir, cell)
            install_rc = _install_or_build(spec, sdk, sample_dir, ws, ctx, log)
            if install_rc != 0:
                assertions = [_assert("install/build exits 0", False, f"exit code {install_rc}")]
                return _result("fail", sdk, assertions, "sample install/build failed", t0, log_path)

            cmd = _resolve_cmd(spec.get("run", []), cell, sample_dir, ws)
            rc = _exec(cmd, sample_dir, log, int(ctx["cell_timeout"]), _run_env(sample_dir, ws))

        log_text = _read(log_path)
        marker = spec.get("expect_marker", "")
        exit_ok = rc == 0
        marker_ok = bool(marker and marker in log_text)
        semantic_ok, semantic_detail = _semantic_assertion(cell["feature"], log_text, marker_ok)
        assertions = [
            _assert("sample exits 0", exit_ok, f"exit code {rc}"),
            _assert(f"output contains marker: {marker}", marker_ok, None if marker_ok else "marker not found"),
            _assert("feature output sanity", semantic_ok, semantic_detail),
        ]
        ok = exit_ok and marker_ok and semantic_ok
        return _result("pass" if ok else "fail", sdk, assertions, "sample-backed run completed", t0, log_path)
    except Exception as e:
        return _result("blocked", sdk, [_assert("runner executed without error", False, repr(e))],
                       f"sample runner raised: {e!r}", t0, log_path)


def _sample_spec(cell):
    with open(MAP_PATH, encoding="utf-8") as f:
        mapping = json.load(f)
    feature = cell["feature"]
    spec = mapping.get(feature, {}).get(cell["sdk"])
    if spec is None and feature == "chat-large":
        spec = mapping.get("chat", {}).get(cell["sdk"])
    return spec


def _install_or_build(spec, sdk, sample_dir, ws, ctx, log):
    kind = spec["kind"]
    if kind == "python":
        return _setup_python(sample_dir, ws, ctx, log)
    if kind == "node":
        return _setup_node(sample_dir, ctx, log)
    if kind == "dotnet":
        return _setup_dotnet(sample_dir, ws, ctx, log)
    if kind == "cmake":
        return _setup_cmake(sample_dir, ws, ctx, log)
    log.write(f"unknown sample kind: {kind}\n")
    return 1


def _setup_python(sample_dir, ws, ctx, log):
    import sys

    venv = os.path.join(ws, "venv")
    rc = _exec([sys.executable, "-m", "venv", venv], sample_dir, log, 120, None)
    if rc != 0:
        return rc
    py = _python_exe(venv)
    _pin_python_requirements(sample_dir)
    pip = feeds.pip_install_args(ws)
    req = os.path.join(sample_dir, "requirements.txt")
    cmd = [py, "-m", "pip", "install", "--no-input", "--cache-dir", pip["cache_dir"], "--index-url", pip["index_url"]]
    cmd += ["-r", req] if os.path.exists(req) else [f"{PKG_NAMES['python']}=={RC_VERSIONS['python']}"]
    return _exec(cmd, sample_dir, log, int(ctx["install_timeout"]), None)


def _setup_node(sample_dir, ctx, log):
    feeds.write_npmrc(sample_dir)
    pkg_path = os.path.join(sample_dir, "package.json")
    if os.path.exists(pkg_path):
        with open(pkg_path, encoding="utf-8") as f:
            pkg = json.load(f)
        deps = pkg.setdefault("dependencies", {})
        if PKG_NAMES["js"] in deps:
            deps[PKG_NAMES["js"]] = RC_VERSIONS["js"]
        with open(pkg_path, "w", encoding="utf-8") as f:
            json.dump(pkg, f, indent=2)
            f.write("\n")
    return _exec(["npm", "install", "--no-audit", "--no-fund"], sample_dir, log, int(ctx["install_timeout"]), None)


def _setup_dotnet(sample_dir, ws, ctx, log):
    feeds.write_nuget_config(ws)
    shutil.copy(os.path.join(ws, "nuget.config"), os.path.join(sample_dir, "nuget.config"))
    _pin_dotnet_projects(ws, sample_dir)
    rc = _exec(["dotnet", "restore", sample_dir], sample_dir, log, int(ctx["install_timeout"]), None)
    if rc != 0:
        return rc
    return _exec(["dotnet", "build", sample_dir, "-c", "Release", "--no-restore"],
                 sample_dir, log, int(ctx["install_timeout"]), None)


def _setup_cmake(sample_dir, ws, ctx, log):
    feeds.write_nuget_config(ws)
    pkg_root = os.path.join(ws, "cpp-runtime")
    os.makedirs(pkg_root, exist_ok=True)
    csproj = os.path.join(pkg_root, "restore.csproj")
    with open(csproj, "w", encoding="utf-8") as f:
        f.write('<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><TargetFramework>net9.0</TargetFramework>')
        f.write('<RestorePackagesPath>packages</RestorePackagesPath></PropertyGroup><ItemGroup>')
        f.write(f'<PackageReference Include="{PKG_NAMES["cpp"]}" Version="{RC_VERSIONS["cpp"]}" />')
        f.write("</ItemGroup></Project>\n")
    shutil.copy(os.path.join(ws, "nuget.config"), os.path.join(pkg_root, "nuget.config"))
    rc = _exec(["dotnet", "restore", csproj], pkg_root, log, int(ctx["install_timeout"]), None)
    if rc != 0:
        return rc
    packages = os.path.join(pkg_root, "packages")
    header = _find_file(packages, "foundry_local.h")
    lib = _find_library(packages)
    native = _find_native(packages)
    if not header or not (lib or native):
        log.write(f"C++ package assets missing: header={header} lib={lib} native={native}\n")
        return 1
    _write_cmakelists(sample_dir, os.path.dirname(header), lib or native)
    build_dir = os.path.join(ws, "cmake-build")
    os.makedirs(build_dir, exist_ok=True)
    rc = _exec(["cmake", "-S", sample_dir, "-B", build_dir], sample_dir, log, int(ctx["install_timeout"]), None)
    if rc != 0:
        return rc
    rc = _exec(["cmake", "--build", build_dir, "--config", "Release"],
               sample_dir, log, int(ctx["install_timeout"]), None)
    if rc == 0:
        _copy_native_libs(packages, build_dir)
    return rc


def _copy_support_files(sdk, src, ws):
    if sdk != "cs":
        return
    root = os.path.dirname(src)
    for name in ("Shared", "Directory.Build.props", "Directory.Packages.props"):
        path = os.path.join(root, name)
        dst = os.path.join(ws, name)
        if os.path.isdir(path):
            if os.path.exists(dst):
                shutil.rmtree(dst)
            shutil.copytree(path, dst)
        elif os.path.exists(path):
            shutil.copy(path, dst)


def _pin_python_requirements(sample_dir):
    req = os.path.join(sample_dir, "requirements.txt")
    if not os.path.exists(req):
        return
    out = []
    seen = False
    for line in _read(req).splitlines():
        if line.strip().startswith(PKG_NAMES["python"]):
            out.append(f"{PKG_NAMES['python']}=={RC_VERSIONS['python']}")
            seen = True
        else:
            out.append(line)
    if not seen:
        out.insert(0, f"{PKG_NAMES['python']}=={RC_VERSIONS['python']}")
    with open(req, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")


def _pin_dotnet_projects(ws, sample_dir):
    props = os.path.join(ws, "Directory.Packages.props")
    central_versions = os.path.exists(props)
    if os.path.exists(props):
        text = _read(props)
        text = _replace_attr_version(text, PKG_NAMES["cs"], RC_VERSIONS["cs"], "PackageVersion")
        with open(props, "w", encoding="utf-8") as f:
            f.write(text)
    if central_versions:
        return
    for base, _dirs, files in os.walk(sample_dir):
        for name in files:
            if name.endswith(".csproj"):
                path = os.path.join(base, name)
                text = _read(path)
                text = _replace_attr_version(text, PKG_NAMES["cs"], RC_VERSIONS["cs"], "PackageReference")
                with open(path, "w", encoding="utf-8") as f:
                    f.write(text)


def _replace_attr_version(text, package, version, element):
    needle = f'<{element} Include="{package}"'
    pos = 0
    while True:
        start = text.find(needle, pos)
        if start < 0:
            return text
        end = text.find(">", start)
        if end < 0:
            return text
        tag = text[start:end]
        if " Version=" in tag:
            vpos = tag.find(" Version=")
            quote = tag[vpos + 9]
            vend = tag.find(quote, vpos + 10)
            tag = tag[:vpos] + f' Version="{version}"' + tag[vend + 1:]
        else:
            tag += f' Version="{version}"'
        text = text[:start] + tag + text[end:]
        pos = start + len(tag)


def _rewrite_model_aliases(sample_dir, cell):
    model = (cell.get("model") or {}).get("alias")
    if not model:
        return
    aliases = [
        "qwen2.5-0.5b",
        "deepseek-r1-distill-qwen-14b",
        "qwen3-embedding-0.6b",
        "whisper-tiny",
        "nemotron-speech-streaming-en-0.6b",
        "Qwen2.5-VL-7B-Instruct",
    ]
    for base, _dirs, files in os.walk(sample_dir):
        for name in files:
            if not name.endswith((".py", ".js", ".ts", ".cs", ".cpp", ".cc", ".h")):
                continue
            path = os.path.join(base, name)
            text = _read(path)
            new_text = text
            for alias in aliases:
                new_text = new_text.replace(alias, model)
            if new_text != text:
                with open(path, "w", encoding="utf-8") as f:
                    f.write(new_text)


def _resolve_cmd(run, cell, sample_dir, ws):
    model = _model_for_run(cell)
    exe = _cmake_exe(ws)
    py = _python_exe(os.path.join(ws, "venv"))
    return [x.replace("{model}", model).replace("{python}", py).replace("{exe}", exe) for x in run]


def _model_for_run(cell):
    model = cell.get("model") or {}
    return model.get("variant") or model.get("alias") or ""


def _run_env(sample_dir, ws):
    env = os.environ.copy()
    env["FOUNDRY_VALIDATION_SAMPLE_DIR"] = sample_dir
    env["FOUNDRY_VALIDATION_WORKSPACE"] = ws
    if os.name != "nt":
        lib_path = os.pathsep.join([os.path.join(ws, "cmake-build"), env.get("LD_LIBRARY_PATH", "")])
        env["LD_LIBRARY_PATH"] = lib_path
        env["DYLD_LIBRARY_PATH"] = os.pathsep.join([os.path.join(ws, "cmake-build"), env.get("DYLD_LIBRARY_PATH", "")])
    return env


def _semantic_assertion(feature, log_text, fallback):
    # Only features whose output is free text can be checked from the captured log with a
    # one-argument assertion. Structured checks (assert_tool_call/assert_embedding/
    # assert_transcription) need parsed objects/vectors/reference text that are not
    # recoverable from a plain run log, so those features rely on the exit-code + marker
    # assertions (fallback) here; wire richer structured capture per-SDK to strengthen them.
    text_checks = {
        "chat": "assert_chat_nonempty",
        "chat-large": "assert_chat_nonempty",
        "vision": "assert_chat_nonempty",
        "web-server": "assert_chat_nonempty",
        "integrations": "assert_chat_nonempty",
        "model-mgmt": "assert_chat_nonempty",
    }
    fn_name = text_checks.get(feature)
    if not fn_name:
        return fallback, "structured feature; used exit-code + marker assertion"
    try:
        import assertions
    except ImportError:
        return fallback, "assertions.py unavailable; used marker assertion"
    fn = getattr(assertions, fn_name, None)
    if not fn:
        return fallback, "no specialized assertion; used marker assertion"
    try:
        out = fn(log_text)
        if isinstance(out, tuple):
            return bool(out[0]), str(out[1]) if len(out) > 1 else None
        if isinstance(out, dict):
            return bool(out.get("ok")), out.get("detail")
        return bool(out), None
    except Exception as e:
        return False, f"{fn_name} raised: {e!r}"


def _exec(cmd, cwd, log, timeout, env):
    log.write(f"\n$ {' '.join(cmd)}  (cwd={cwd})\n")
    log.flush()
    try:
        p = subprocess.run(cmd, cwd=cwd, timeout=timeout, env=env, capture_output=True, text=True)
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


def _result(result, sdk, assertions, notes, t0, log_path):
    return {
        "result": result,
        "assertions": assertions,
        "notes": notes,
        "package": _pkg(sdk, feeds.feed_env().get(_feed_key(sdk))),
        "duration_seconds": round(time.time() - t0, 1),
        "log_path": log_path,
    }


def _feed_key(sdk):
    return {"python": "pip", "js": "npm", "cs": "nuget", "cpp": "nuget"}[sdk]


def _python_exe(venv):
    if os.name == "nt":
        return os.path.join(venv, "Scripts", "python.exe")
    return os.path.join(venv, "bin", "python")


def _cmake_exe(ws):
    name = "live-audio-transcription-example.exe" if os.name == "nt" else "live-audio-transcription-example"
    for base, _dirs, files in os.walk(os.path.join(ws, "cmake-build")):
        if name in files:
            return os.path.join(base, name)
    return os.path.join(ws, "cmake-build", name)


def _write_cmakelists(sample_dir, include_dir, lib):
    with open(os.path.join(sample_dir, "CMakeLists.txt"), "w", encoding="utf-8") as f:
        f.write("cmake_minimum_required(VERSION 3.20)\n")
        f.write("project(foundry_local_live_audio_sample LANGUAGES CXX)\n")
        f.write("set(CMAKE_CXX_STANDARD 20)\n")
        f.write("add_executable(live-audio-transcription-example main.cpp)\n")
        f.write(f'target_include_directories(live-audio-transcription-example PRIVATE "{include_dir}")\n')
        f.write(f'target_link_libraries(live-audio-transcription-example PRIVATE "{lib}")\n')


def _find_file(root, filename):
    for base, _dirs, files in os.walk(root):
        if filename in files:
            return os.path.join(base, filename)
    return None


def _find_library(root):
    endings = (".lib", ".a", ".dylib", ".so")
    for base, _dirs, files in os.walk(root):
        for name in files:
            low = name.lower()
            if low.startswith("foundry_local") and low.endswith(endings):
                return os.path.join(base, name)
    return None


def _find_native(root):
    endings = (".dll", ".so", ".dylib")
    for base, _dirs, files in os.walk(root):
        for name in files:
            low = name.lower()
            if low.startswith("foundry_local") and low.endswith(endings):
                return os.path.join(base, name)
    return None


def _copy_native_libs(src_root, dst):
    endings = (".dll", ".so", ".dylib")
    for base, _dirs, files in os.walk(src_root):
        for name in files:
            if name.lower().endswith(endings):
                shutil.copy(os.path.join(base, name), os.path.join(dst, name))


def _read(path):
    try:
        with open(path, encoding="utf-8") as f:
            return f.read()
    except OSError:
        return ""
