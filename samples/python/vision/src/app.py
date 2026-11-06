#!/usr/bin/env python3
"""Describe a local image using the native ChatSession API."""

import argparse
from pathlib import Path

from foundry_local_sdk import (
    ChatSession,
    Configuration,
    FoundryLocalManager,
    ImageItem,
    MessageItem,
    Request,
    TextItem,
    TextItemType,
)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model_alias", help="a vision-language-chat model alias")
    parser.add_argument("image_path", type=Path)
    args = parser.parse_args()

    manager = FoundryLocalManager(Configuration(app_name="foundry_local_samples"))
    model = None
    try:
        result = manager.download_and_register_eps()
        if not result.success:
            raise RuntimeError(
                f"Execution provider registration failed: {result.failed_eps}"
            )
        model = manager.catalog.get_model(args.model_alias)
        if model is None:
            raise RuntimeError(f"Model {args.model_alias!r} was not found.")
        if not model.is_cached:
            model.download()
        model.load()

        with ChatSession(model) as session, Request() as request:
            request.add_item(
                MessageItem.user(
                    [
                        TextItem("Describe this image in one short sentence."),
                        ImageItem.from_uri(str(args.image_path.resolve())),
                    ]
                )
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
        if model is not None and model.is_loaded:
            model.unload()
        manager.close()


if __name__ == "__main__":
    main()
