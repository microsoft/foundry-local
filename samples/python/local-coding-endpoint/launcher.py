from __future__ import annotations

import argparse
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

MODEL_ID = "qwen38-dflash2-coding:1"
ENDPOINT_URL = "http://127.0.0.1:5272"


def parse_model_path() -> Path:
    parser = argparse.ArgumentParser(
        description="Serve the Qwen3.8 DFlash2 coding model through Foundry Local."
    )
    parser.add_argument(
        "-m",
        "--model",
        type=Path,
        required=True,
        help="Model directory containing genai_config.json.",
    )
    model_path = parser.parse_args().model.expanduser().resolve()
    if not model_path.is_dir():
        parser.error(f"--model must be an existing directory: {model_path}")
    if not (model_path / "genai_config.json").is_file():
        parser.error(f"--model must contain genai_config.json: {model_path}")
    return model_path


def get_or_register_model(
    manager: FoundryLocalManager,
    model_path: Path,
) -> tuple[IModel, bool]:
    """Return the requested local model and whether this process registered it."""
    local_catalog = manager.get_catalog(CatalogType.LOCAL)
    model = local_catalog.get_model_variant(MODEL_ID)
    if model is not None:
        registered_path = Path(model.get_path()).resolve()
        if registered_path != model_path:
            raise RuntimeError(
                f"{MODEL_ID} is already registered for {registered_path}, not {model_path}"
            )
        return model, False

    with ModelInfoBuilder() as metadata:
        metadata.set_string_property("task", "chat-completion")
        metadata.set_string_property("device_type", "GPU")
        metadata.set_string_property("execution_provider", "CUDAExecutionProvider")
        metadata.set_int_property("supports_tool_calling", 1)
        metadata.set_int_property("supports_reasoning", 1)
        model = local_catalog.register_model(model_path, MODEL_ID, metadata)
    return model, True


def run(model_path: Path) -> None:
    FoundryLocalManager.initialize(
        Configuration(
            app_name="LocalCodingEndpoint",
            web=Configuration.WebService(urls=ENDPOINT_URL),
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
        print("Downloading/registering CUDAExecutionProvider...")
        result = manager.download_and_register_eps(["CUDAExecutionProvider"])
        if not result.success:
            raise RuntimeError(
                f"CUDA execution-provider registration failed: {result.status}"
            )

        model, registered_here = get_or_register_model(manager, model_path)
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
                        manager.get_catalog(CatalogType.LOCAL).unregister_model(MODEL_ID)
                finally:
                    manager.close()


def main() -> None:
    run(parse_model_path())


if __name__ == "__main__":
    main()
