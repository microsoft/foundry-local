# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Unit tests for tolerant tool-call parsing in the SDK v2 ChatClient.

These cover the pure sanitization helpers used by ``complete_chat`` /
``complete_streaming_chat`` and do not require a native session or loaded model.
"""
from __future__ import annotations

import json

import pytest

pytest.importorskip("openai")

from openai.types.chat import ChatCompletion  # noqa: E402
from openai.types.chat.chat_completion_chunk import ChatCompletionChunk  # noqa: E402

from foundry_local_sdk.openai.chat_client import (  # noqa: E402
    _drop_malformed_tool_calls,
    _tool_call_is_valid,
)


def _completion_with_tool_calls(tool_calls: list[dict]) -> dict:
    return {
        "id": "chatcmpl-x",
        "object": "chat.completion",
        "created": 0,
        "model": "smollm3-3b",
        "choices": [
            {
                "index": 0,
                "finish_reason": "tool_calls",
                "message": {
                    "role": "assistant",
                    "content": None,
                    "tool_calls": tool_calls,
                },
            }
        ],
    }


class TestToolCallValidation:
    """Tests for ``_tool_call_is_valid``."""

    def test_function_call_with_name_is_valid(self):
        assert _tool_call_is_valid(
            {"type": "function", "function": {"name": "f", "arguments": "{}"}}
        )

    def test_function_call_without_name_is_invalid(self):
        # Exactly what smollm3-3b emitted: no name, arguments == "null".
        assert not _tool_call_is_valid(
            {"type": "function", "function": {"arguments": "null"}}
        )

    def test_function_call_with_extra_custom_key_is_still_valid(self):
        # Dispatch on the explicit "type" discriminator: a valid function call must
        # not be dropped merely because it also carries an extra "custom" key.
        assert _tool_call_is_valid(
            {"type": "function", "custom": None,
             "function": {"name": "f", "arguments": "{}"}}
        )

    def test_custom_call_is_valid(self):
        assert _tool_call_is_valid(
            {"type": "custom", "custom": {"name": "c", "input": "x"}}
        )

    def test_missing_discriminator_is_invalid(self):
        # No "type" — the discriminator is missing, so the entry is malformed.
        assert not _tool_call_is_valid(
            {"function": {"name": "f", "arguments": "{}"}}
        )

    def test_unknown_discriminator_is_invalid(self):
        assert not _tool_call_is_valid(
            {"type": "mystery", "function": {"name": "f", "arguments": "{}"}}
        )

    def test_non_dict_is_invalid(self):
        assert not _tool_call_is_valid("nope")
        assert not _tool_call_is_valid(None)


class TestDropMalformedToolCalls:
    """Tests for ``_drop_malformed_tool_calls`` + downstream pydantic validation."""

    def test_drops_only_malformed_entry_and_keeps_valid(self):
        payload = _completion_with_tool_calls(
            [
                {"index": 0, "id": "a", "type": "function",
                 "function": {"name": "get_weather", "arguments": "{}"}},
                {"index": 1, "id": "b", "type": "function",
                 "function": {"arguments": "null"}},
            ]
        )

        _drop_malformed_tool_calls(payload)
        completion = ChatCompletion.model_validate(payload)  # must not raise

        tool_calls = completion.choices[0].message.tool_calls
        assert tool_calls is not None
        assert len(tool_calls) == 1
        assert tool_calls[0].function.name == "get_weather"

    def test_all_malformed_removes_tool_calls_entirely(self):
        payload = _completion_with_tool_calls(
            [{"index": 0, "id": "b", "type": "function", "function": {"arguments": "null"}}]
        )

        _drop_malformed_tool_calls(payload)
        completion = ChatCompletion.model_validate(payload)  # must not raise

        assert completion.choices[0].message.tool_calls is None

    def test_valid_response_is_untouched(self):
        payload = _completion_with_tool_calls(
            [{"index": 0, "id": "a", "type": "function",
              "function": {"name": "f", "arguments": "{}"}}]
        )
        expected = json.loads(json.dumps(payload))  # deep copy

        _drop_malformed_tool_calls(payload)

        assert payload == expected

    def test_streaming_delta_tool_calls_are_sanitized(self):
        payload = {
            "id": "chatcmpl-x",
            "object": "chat.completion.chunk",
            "created": 0,
            "model": "smollm3-3b",
            "choices": [
                {
                    "index": 0,
                    "finish_reason": "tool_calls",
                    "delta": {
                        "role": "assistant",
                        "tool_calls": [
                            {"index": 0, "id": "a", "type": "function",
                             "function": {"name": "f", "arguments": "{}"}},
                            {"index": 1, "id": "b", "type": "function",
                             "function": {"arguments": "null"}},
                        ],
                    },
                }
            ],
        }

        _drop_malformed_tool_calls(payload)
        chunk = ChatCompletionChunk.model_validate(payload)  # must not raise

        remaining = chunk.choices[0].delta.tool_calls
        assert remaining is not None
        assert len(remaining) == 1
        assert remaining[0].function.name == "f"
