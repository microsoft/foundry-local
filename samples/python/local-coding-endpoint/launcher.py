from __future__ import annotations

import argparse
import re
import signal
import threading
from pathlib import Path

from foundry_local_sdk import (
    CatalogType,
    Configuration,
    FoundryLocalManager,
    IModel,
    ModelInfoBuilder,
)

DEFAULT_MODEL_ID = "local-coding-cuda:1"
MODEL_ID_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*:[1-9][0-9]*$")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Serve a local ONNX Runtime GenAI coding model through Foundry Local."
    )
    parser.add_argument(
        "-m",
        "--model",
        type=Path,
        required=True,
        help="Model directory containing genai_config.json.",
    )
    parser.add_argument(
        "--model-id",
        default=DEFAULT_MODEL_ID,
        help="Local catalog ID in <name>:<version> form.",
    )
    parser.add_argument(
        "--host",
        default="127.0.0.1",
        help="Web-service bind host (default: 127.0.0.1).",
    )
    parser.add_argument(
        "--port", type=int, default=5272, help="Web-service bind port (default: 5272)."
    )
    parser.add_argument(
        "--skip-cuda-ep",
        action="store_true",
        help="Do not download and register CUDAExecutionProvider.",
    )
    args = parser.parse_args()

    args.model = args.model.expanduser().resolve()
    if not args.model.is_dir():
        parser.error(f"--model must be an existing directory: {args.model}")
    if not (args.model / "genai_config.json").is_file():
        parser.error(f"--model must contain genai_config.json: {args.model}")
    if not MODEL_ID_PATTERN.fullmatch(args.model_id):
        parser.error("--model-id must use <name>:<positive-integer-version> form")
    if (
        not args.host
        or ":" in args.host
        or any(character.isspace() for character in args.host)
    ):
        parser.error("--host must be a non-empty hostname or IPv4 address")
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    return args


def get_or_register_model(
    manager: FoundryLocalManager,
    model_path: Path,
    model_id: str,
) -> tuple[IModel, bool]:
    """Return the requested local model and whether this process registered it."""
    local_catalog = manager.get_catalog(CatalogType.LOCAL)
    model = local_catalog.get_model_variant(model_id)
    if model is not None:
        registered_path = Path(model.get_path()).resolve()
        if registered_path != model_path:
            raise RuntimeError(
                f"{model_id} is already registered for {registered_path}, not {model_path}"
            )
        return model, False

    with ModelInfoBuilder() as metadata:
        metadata.set_string_property("task", "chat-completion")
        metadata.set_string_property("device_type", "GPU")
        metadata.set_string_property("execution_provider", "CUDAExecutionProvider")
        metadata.set_int_property("supports_tool_calling", 1)
        metadata.set_int_property("supports_reasoning", 1)
        model = local_catalog.register_model(model_path, model_id, metadata)
    return model, True


def run(args: argparse.Namespace) -> None:
    endpoint_url = f"http://{args.host}:{args.port}"
    FoundryLocalManager.initialize(
        Configuration(
            app_name="LocalCodingEndpoint",
            web=Configuration.WebService(urls=endpoint_url),
        )
    )
    manager = FoundryLocalManager.instance
    if manager is None:
        raise RuntimeError("Foundry Local manager initialization returned no instance")

    model: IModel | None = None
    registered_here = False
    loaded_here = False
    service_started = False
    try:
        if args.skip_cuda_ep:
            print("Skipping CUDA execution-provider registration.")
        else:
            print("Downloading/registering CUDAExecutionProvider...")
            result = manager.download_and_register_eps(["CUDAExecutionProvider"])
            if not result.success:
                raise RuntimeError(
                    f"CUDA execution-provider registration failed: {result.status}"
                )

        model, registered_here = get_or_register_model(
            manager, args.model, args.model_id
        )
        if not model.is_loaded:
            model.load()
            loaded_here = True

        manager.start_web_service()
        service_started = True
        if not manager.urls:
            raise RuntimeError("The web service started without reporting a bound URL")

        base_url = manager.urls[0].rstrip("/") + "/v1"
        print(f"Model ID: {model.id}")
        print(f"OpenAI-compatible URL: {base_url}")
        print("Press Ctrl+C to stop.")

        stop_event = threading.Event()

        def request_stop(_signum: int, _frame: object) -> None:
            stop_event.set()

        previous_sigterm = signal.signal(signal.SIGTERM, request_stop)
        try:
            try:
                stop_event.wait()
            except KeyboardInterrupt:
                pass
        finally:
            signal.signal(signal.SIGTERM, previous_sigterm)
    finally:
        # Release dependants before their borrowed model/catalog/manager handles.
        try:
            if service_started:
                manager.stop_web_service()
        finally:
            try:
                if model is not None and loaded_here:
                    model.unload()
            finally:
                try:
                    if registered_here:
                        manager.get_catalog(CatalogType.LOCAL).unregister_model(
                            args.model_id
                        )
                finally:
                    manager.close()


def main() -> None:
    run(parse_args())


if __name__ == "__main__":
    main()
