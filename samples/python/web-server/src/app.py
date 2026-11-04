#!/usr/bin/env python3
"""Call the built-in Foundry Local OpenAI-compatible web service."""

from __future__ import annotations

from openai import OpenAI

from foundry_local_sdk import Configuration, FoundryLocalManager

APP_NAME = "foundry_local_samples"
MODEL_ALIAS = "qwen2.5-0.5b"


def main() -> None:
    manager = FoundryLocalManager(Configuration(app_name=APP_NAME))
    ep_result = manager.download_and_register_eps()
    if not ep_result.success:
        manager.close()
        raise RuntimeError(
            f"Execution provider registration failed: {ep_result.failed_eps}"
        )

    model = manager.catalog.get_model(MODEL_ALIAS)
    if model is None:
        manager.close()
        raise RuntimeError(f"Model {MODEL_ALIAS!r} was not found in the catalog.")

    server_started = False
    try:
        if not model.is_cached:
            model.download(
                progress_callback=lambda percent: print(
                    f"\rDownloading model: {percent:5.1f}%",
                    end="",
                    flush=True,
                )
            )
            print()
        model.load()

        manager.start_web_service()
        server_started = True
        if not manager.urls:
            raise RuntimeError("The web server started without reporting a URL.")

        server_url = manager.urls[0].rstrip("/")
        print(f"Foundry Local server: {server_url}")
        client = OpenAI(base_url=f"{server_url}/v1", api_key="not-needed")
        response = client.chat.completions.create(
            model=model.id,
            messages=[
                {"role": "system", "content": "Answer clearly and briefly."},
                {"role": "user", "content": "What is the golden ratio?"},
            ],
        )
        print(response.choices[0].message.content)
    finally:
        if server_started:
            manager.stop_web_service()
            print("Web server stopped.")
        if model.is_loaded:
            model.unload()
            print("Model unloaded.")
        manager.close()


if __name__ == "__main__":
    main()
