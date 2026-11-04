#!/usr/bin/env python3
"""Handle a function tool call with the native ChatSession API."""

from __future__ import annotations

import json
from collections.abc import Iterable, Iterator

from foundry_local_sdk import (
    ChatSession,
    Configuration,
    FoundryLocalManager,
    Item,
    MessageItem,
    Request,
    RequestOptions,
    SearchOptions,
    TextItem,
    ToolCallItem,
    ToolChoice,
    ToolResultItem,
)

APP_NAME = "foundry_local_samples"
MODEL_ALIAS = "qwen2.5-0.5b"
WEATHER_SCHEMA = json.dumps(
    {
        "type": "object",
        "properties": {
            "city": {"type": "string"},
        },
        "required": ["city"],
        "additionalProperties": False,
    }
)


def nested_items(items: Iterable[Item]) -> Iterator[Item]:
    for item in items:
        yield item
        if isinstance(item, MessageItem):
            yield from nested_items(item.parts)


def print_text(items: Iterable[Item]) -> None:
    for item in nested_items(items):
        if isinstance(item, TextItem):
            print(item.text, end="")
    print()


def main() -> None:
    manager = FoundryLocalManager(Configuration(app_name=APP_NAME))
    model = None

    try:
        ep_result = manager.download_and_register_eps()
        if not ep_result.success:
            raise RuntimeError(
                f"Execution provider registration failed: {ep_result.failed_eps}"
            )

        model = manager.catalog.get_model(MODEL_ALIAS)
        if model is None:
            raise RuntimeError(f"Model {MODEL_ALIAS!r} was not found in the catalog.")
        if model.supports_tool_calling is not True:
            raise RuntimeError(
                f"The selected {MODEL_ALIAS} variant does not support tool calling."
            )
        print(f"Selected tool-capable model: {model.alias} ({model.id})")

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

        session = ChatSession(model)
        with session:
            session.add_tool_definition(
                name="get_weather",
                description="Get the current weather for a city.",
                json_schema=WEATHER_SCHEMA,
            )

            options = RequestOptions(
                search=SearchOptions(temperature=0.0, max_output_tokens=256),
                tool_choice=ToolChoice.REQUIRED,
            )
            with (
                Request()
                .add_item(MessageItem.user("What is the weather in Paris?"))
                .set_options(options) as request,
                session.process_request(request) as response,
            ):
                calls = [
                    item
                    for item in nested_items(response)
                    if isinstance(item, ToolCallItem)
                ]
                if not calls:
                    raise RuntimeError("The model did not produce a tool call.")
                call = calls[0]
                call_id = call.call_id
                print(f"Tool call: {call.name}({call.arguments})")

            weather = {
                "temperature": 22,
                "unit": "celsius",
                "condition": "sunny",
            }
            with (
                Request()
                .add_item(ToolResultItem(call_id=call_id, result=json.dumps(weather)))
                .set_options(
                    RequestOptions(
                        search=SearchOptions(temperature=0.0, max_output_tokens=256),
                        tool_choice=ToolChoice.NONE,
                    )
                ) as follow_up,
                session.process_request(follow_up) as final_response,
            ):
                print("Assistant: ", end="")
                print_text(final_response)
    finally:
        if model is not None and model.is_loaded:
            model.unload()
        manager.close()


if __name__ == "__main__":
    main()
