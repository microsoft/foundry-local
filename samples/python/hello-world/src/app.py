#!/usr/bin/env python3
"""End-to-end native Foundry Local chat sample."""

from __future__ import annotations

from foundry_local_sdk import (
    ChatSession,
    Configuration,
    FoundryLocalManager,
    MessageItem,
    Request,
    RequestOptions,
    SearchOptions,
    TextItem,
    TextItemType,
)

APP_NAME = "foundry_local_samples"
MODEL_ALIAS = "qwen2.5-0.5b"


def print_download_progress(percent: float) -> None:
    print(f"\rDownloading model: {percent:5.1f}%", end="", flush=True)


def main() -> None:
    # Manager owns catalogs and coordinates the lifetime of models and sessions.
    manager = FoundryLocalManager(Configuration(app_name=APP_NAME))
    model = None

    try:
        print("Discovering execution providers:")
        execution_providers = manager.discover_eps()
        for execution_provider in execution_providers:
            state = "registered" if execution_provider.is_registered else "available"
            print(f"  {execution_provider.name}: {state}")

        print("Downloading and registering execution providers...")
        result = manager.download_and_register_eps()
        if not result.success:
            raise RuntimeError(
                f"Execution provider registration failed: {result.failed_eps}"
            )

        catalog = manager.catalog
        model = catalog.get_model(MODEL_ALIAS)
        if model is None:
            raise RuntimeError(f"Model {MODEL_ALIAS!r} was not found in the catalog.")

        print(f"\nSelected {model.alias} ({model.id})")
        if model.is_cached:
            print("Model is already cached; skipping download.")
        else:
            model.download(progress_callback=print_download_progress)
            print()

        model.load()
        print("Model loaded.")

        with ChatSession(model) as session:
            session.set_streaming(True)
            session.set_options(
                RequestOptions(
                    search=SearchOptions(temperature=0.0, max_output_tokens=128)
                )
            )
            with Request().add_item(
                MessageItem.user("Why is the sky blue? Answer in two sentences.")
            ) as request:
                print("\nAssistant: ", end="", flush=True)
                with session.process_streaming_request(request) as stream:
                    for item in stream:
                        if (
                            isinstance(item, TextItem)
                            and item.type is TextItemType.DEFAULT
                        ):
                            print(item.text, end="", flush=True)
                print()
    finally:
        if model is not None and model.is_loaded:
            model.unload()
            print("Model unloaded.")
        manager.close()
        print("Manager closed.")


if __name__ == "__main__":
    main()
