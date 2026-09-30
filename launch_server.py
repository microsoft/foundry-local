#!/usr/bin/env python3
"""Serve a local CUDA BYOM model through Foundry Local's OpenAI-compatible API."""

import argparse
import importlib.util
import json
import os
import platform
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parent
EVAL_DIR = ROOT / "sdk_v2" / "cpp" / "build" / "latest-main-eval"
DEFAULT_NATIVE_LIB_DIR = (
    EVAL_DIR / "foundry-src" / "sdk_v2" / "cpp" / "build" / "Windows" /
    "RelWithDebInfo" / "bin" / "RelWithDebInfo"
)
DEFAULT_MODEL_ID = "qwen-3.8-27b-cuda-gpu:1"
DEFAULT_PORT = 5272


def use_sdk_python() -> None:
    if importlib.util.find_spec("foundry_local_sdk") is not None:
        return
    python = EVAL_DIR / "python-env" / "Scripts" / "python.exe"
    if not python.is_file():
        raise RuntimeError(f"Foundry Local SDK is not installed in {sys.executable}; "
                           f"the isolated Python environment is missing at {python}")
    raise SystemExit(subprocess.call([str(python), str(ROOT / "launch_server.py"), *sys.argv[1:]]))


def configure_cudnn(cudnn_bin_dir: Path | None) -> object | None:
    if os.name != "nt":
        return None

    if cudnn_bin_dir is None:
        cuda_home = os.environ.get("CUDA_PATH")
        if not cuda_home:
            raise RuntimeError("CUDA_PATH is not set; specify --cudnn-bin-dir for a custom cuDNN installation")
        arch = "arm64" if platform.machine().lower() in ("arm64", "aarch64") else "x64"
        root = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "NVIDIA" / "CUDNN"
        cuda_version = Path(cuda_home).name.removeprefix("v")
        candidates = list(root.glob(f"v*/bin/{cuda_version}/{arch}/cudnn64_9.dll"))
        if not candidates:
            raise FileNotFoundError(f"No cuDNN 9 runtime for CUDA {cuda_version}/{arch} under {root}; "
                                    "specify --cudnn-bin-dir")
        cudnn_bin_dir = max(candidates, key=lambda dll: tuple(
            int(part) for part in dll.parents[3].name.removeprefix("v").split(".")
        )).parent
    else:
        cudnn_bin_dir = cudnn_bin_dir.resolve(strict=True)
        if not (cudnn_bin_dir / "cudnn64_9.dll").is_file():
            raise FileNotFoundError(f"Missing cudnn64_9.dll in {cudnn_bin_dir}")

    os.environ["PATH"] = f"{cudnn_bin_dir}{os.pathsep}{os.environ.get('PATH', '')}"
    return os.add_dll_directory(str(cudnn_bin_dir))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-m", "--model-path", type=Path, required=True, help="Directory containing genai_config.json")
    parser.add_argument("--model-id", default=DEFAULT_MODEL_ID, help="BYOM ID in name:version format")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="Loopback HTTP port (0 selects a free port)")
    parser.add_argument("--native-lib-dir", type=Path, default=DEFAULT_NATIVE_LIB_DIR,
                        help="Directory containing the matching Foundry/ORT/CUDA DLLs")
    parser.add_argument("--cudnn-bin-dir", type=Path, help="Override auto-detected cuDNN 9 DLL directory")
    parser.add_argument("--app-data-dir", type=Path, help="Optional isolated Foundry application data directory")
    args = parser.parse_args()

    if not 0 <= args.port <= 65535:
        parser.error("--port must be between 0 and 65535")
    model_path = args.model_path.resolve(strict=True)
    config_path = model_path / "genai_config.json"
    if not config_path.is_file():
        parser.error(f"Model directory is missing {config_path}")
    model_config = json.loads(config_path.read_text(encoding="utf-8"))

    native_dir = args.native_lib_dir.resolve()
    for name in ("foundry_local.dll", "onnxruntime.dll", "onnxruntime-genai.dll",
                 "onnxruntime-genai-cuda.dll", "onnxruntime_providers_cuda.dll"):
        if not (native_dir / name).is_file():
            parser.error(f"Native library is missing {native_dir / name}; specify --native-lib-dir")
    os.environ["FOUNDRY_LOCAL_LIB_DIR"] = str(native_dir)
    os.environ["FOUNDRY_LOCAL_CUDA_EP_LIBRARY"] = str(native_dir / "onnxruntime_providers_cuda.dll")

    cudnn_dll_directory = configure_cudnn(args.cudnn_bin_dir)

    from foundry_local_sdk import CatalogType, Configuration, FoundryLocalManager, ModelInfoBuilder

    config = Configuration(
        app_name="LocalByomServer",
        app_data_dir=str(args.app_data_dir.resolve()) if args.app_data_dir else None,
        web=Configuration.WebService(urls=f"http://127.0.0.1:{args.port}"),
        disable_nonessential_telemetry=True,
    )
    FoundryLocalManager.initialize(config)
    manager = FoundryLocalManager.instance
    try:
        manager.download_and_register_eps(["CUDAExecutionProvider"])
        if not any(ep.name == "CUDAExecutionProvider" and ep.is_registered for ep in manager.discover_eps()):
            raise RuntimeError("CUDA plugin EP was not registered")

        catalog = manager.get_catalog(CatalogType.LOCAL)
        model = catalog.get_model_variant(args.model_id)
        if model is not None:
            cached_path = model.get_path()
            registered_path = Path(cached_path).resolve() if cached_path else None
            if registered_path != model_path and (registered_path is None or not registered_path.exists()):
                print(f"Replacing stale registration for {args.model_id}", flush=True)
                catalog.unregister_model(args.model_id)
                model = None
        if model is None:
            with ModelInfoBuilder() as metadata:
                metadata.set_string_property("task", "chat-completion")
                metadata.set_string_property("device_type", "GPU")
                metadata.set_string_property("execution_provider", "CUDAExecutionProvider")
                metadata.set_int_property("supports_tool_calling", 1)
                metadata.set_int_property("supports_reasoning", 1)
                metadata.set_int_property("context_length", model_config["model"]["context_length"])
                model = catalog.register_model(model_path, args.model_id, metadata)
        elif (registered_path != model_path or
              model.get_int_property("supports_tool_calling", -1) != 1 or
              model.get_int_property("supports_reasoning", -1) != 1):
            raise RuntimeError(f"{args.model_id} is already registered at {registered_path} with different "
                               f"path or capabilities; requested {model_path}")

        model.load()
        try:
            print(f"Loaded model: {model.id}", flush=True)
            manager.start_web_service()
            try:
                if not manager.urls:
                    raise RuntimeError("Web service started without a bound URL")
                for url in manager.urls:
                    print(f"Copilot BYOK OpenAI base URL: {url}/catalogs/local/v1", flush=True)
                    print(f"Chat completions URL: {url}/catalogs/local/v1/chat/completions", flush=True)
                print("Tool calling and reasoning enabled.", flush=True)
                print("Listening on loopback; press Ctrl+C to stop.", flush=True)
                while True:
                    time.sleep(3600)
            except KeyboardInterrupt:
                pass
            finally:
                manager.stop_web_service()
        finally:
            model.unload()
    finally:
        manager.shutdown()
        if cudnn_dll_directory is not None:
            cudnn_dll_directory.close()


if __name__ == "__main__":
    use_sdk_python()
    main()
