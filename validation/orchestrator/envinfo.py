"""Environment fingerprinting for the Foundry Local validation harness.

Detects OS/arch, maps to a platform id from the platform manifest, and best-effort
detects available accelerators (CUDA GPU, DirectML/WinML, CoreML/Metal, NPU) plus
runtime/toolchain versions. Pure stdlib so it runs on a fresh agent with no deps.
"""
from __future__ import annotations

import json
import platform
import shutil
import socket
import subprocess
import sys
from typing import Any, Dict, List, Optional


def _run(cmd: List[str], timeout: int = 15) -> Optional[str]:
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        if out.returncode == 0:
            return (out.stdout or out.stderr).strip()
    except Exception:
        return None
    return None


def _tool_version(exe: str, args: List[str]) -> Optional[str]:
    if not shutil.which(exe):
        return None
    return _run([exe, *args])


def detect_os_arch() -> Dict[str, str]:
    sysname = platform.system().lower()  # windows / linux / darwin
    machine = platform.machine().lower()
    arch = {
        "x86_64": "x64", "amd64": "x64", "arm64": "arm64", "aarch64": "arm64",
    }.get(machine, machine)
    return {"os": sysname, "arch": arch}


def platform_id(os_name: str, arch: str) -> str:
    return {
        ("windows", "x64"): "windows-x64",
        ("windows", "arm64"): "windows-arm64",
        ("linux", "x64"): "linux-x64",
        ("linux", "arm64"): "linux-arm64",
        ("darwin", "arm64"): "macos-arm64",
        ("darwin", "x64"): "macos-x64",
    }.get((os_name, arch), f"{os_name}-{arch}")


def detect_gpus(os_name: str) -> List[str]:
    gpus: List[str] = []
    nv = _tool_version("nvidia-smi", ["--query-gpu=name", "--format=csv,noheader"])
    if nv:
        gpus.extend([g.strip() for g in nv.splitlines() if g.strip()])
    if os_name == "darwin":
        info = _run(["system_profiler", "SPDisplaysDataType"], timeout=20)
        if info:
            for line in info.splitlines():
                line = line.strip()
                if line.startswith("Chipset Model:"):
                    gpus.append(line.split(":", 1)[1].strip())
    return gpus


def detect_cuda() -> Optional[str]:
    v = _tool_version("nvcc", ["--version"])
    if v:
        for line in v.splitlines():
            if "release" in line.lower():
                return line.strip()
    smi = _tool_version("nvidia-smi", [])
    if smi:
        for line in smi.splitlines():
            if "CUDA Version" in line:
                return line.strip()
    return None


def detect_accelerators(os_name: str, gpus: List[str]) -> List[str]:
    """Best-effort list of accelerators physically usable on this machine."""
    accels = ["cpu"]
    has_nvidia = any("nvidia" not in g.lower() for g in []) or bool(_tool_version("nvidia-smi", []))
    if has_nvidia or any("nvidia" in g.lower() for g in gpus):
        accels.append("cuda")
    if os_name == "windows":
        # DirectML/WinML available on Win with a compatible GPU; assume present, agents can override.
        accels.append("winml-dml")
        # NPU / WebGPU are hardware/feature dependent; agents confirm via ep-bootstrap.
    if os_name == "darwin":
        accels.append("coreml-metal")
    return accels


def detect_runtimes() -> Dict[str, Optional[str]]:
    return {
        "python": sys.version.split()[0],
        "dotnet": _tool_version("dotnet", ["--version"]),
        "node": _tool_version("node", ["--version"]),
        "npm": _tool_version("npm", ["--version"]),
        "cmake": _tool_version("cmake", ["--version"]),
    }


def fingerprint() -> Dict[str, Any]:
    oa = detect_os_arch()
    gpus = detect_gpus(oa["os"])
    return {
        "platform_id": platform_id(oa["os"], oa["arch"]),
        "os": oa["os"],
        "arch": oa["arch"],
        "hostname": socket.gethostname(),
        "cpu": platform.processor() or platform.machine(),
        "gpus": gpus,
        "npu": None,  # confirmed via ep-bootstrap on capable hardware
        "driver": None,
        "cuda": detect_cuda(),
        "available_accelerators": detect_accelerators(oa["os"], gpus),
        "runtimes": detect_runtimes(),
    }


if __name__ == "__main__":
    print(json.dumps(fingerprint(), indent=2))
