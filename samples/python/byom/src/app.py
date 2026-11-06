#!/usr/bin/env python3
"""Register a compatible local chat model and run native inference."""

import argparse
from pathlib import Path

from foundry_local_sdk import (
    CatalogType,
    ChatSession,
    Configuration,
    FoundryLocalManager,
    MessageItem,
    ModelInfoBuilder,
    Request,
    TextItem,
    TextItemType,
)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model_directory", type=Path)
    args = parser.parse_args()

    manager = FoundryLocalManager(Configuration(app_name="foundry_local_samples"))
    model = None
    try:
        result = manager.download_and_register_eps()
        if not result.success:
            raise RuntimeError(
                f"Execution provider registration failed: {result.failed_eps}"
            )
        catalog = manager.get_catalog(CatalogType.LOCAL)
        with ModelInfoBuilder() as metadata:
            metadata.set_string_property("task", "chat-completion")
            model = catalog.register_model(
                args.model_directory.resolve(), "sample-byom:1", metadata
            )
        model.load()

        with ChatSession(model) as session, Request() as request:
            request.add_item(
                MessageItem.user("Why is the sky blue? Answer in two sentences.")
            )
            with session.process_request(request) as response:
                for item in response:
                    if isinstance(item, MessageItem):
                        text = "".join(
                            part.text
                            for part in item.parts
                            if isinstance(part, TextItem)
                            and part.type is TextItemType.DEFAULT
                        )
                        print(f"Assistant: {text}")
    finally:
        try:
            if model is not None:
                if model.is_loaded:
                    model.unload()
                catalog.unregister_model("sample-byom:1")
        finally:
            manager.close()


if __name__ == "__main__":
    main()
