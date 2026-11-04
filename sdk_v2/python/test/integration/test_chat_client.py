# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""End-to-end ChatClient tests — non-streaming and streaming.

The bulk of inference correctness is covered by the C++ test suite.
These tests focus on Python-specific concerns:

- The OpenAI-format request JSON is built and accepted by the native layer.
- The response JSON is parsed into a typed ``ChatCompletion`` (no shape drift).
- The streaming generator yields ``ChatCompletionChunk`` objects in order
  and produces equivalent visible content to the non-streaming call.
- ``finish_reason`` and ``usage`` survive the round-trip.
"""
from __future__ import annotations
from foundry_local_sdk.openai.chat_client import ChatClient
from openai.types.chat.chat_completion_chunk import ChatCompletionChunk
from openai.types.chat import ChatCompletion

import pytest

pytest.importorskip("openai")


@pytest.fixture
def chat_client(chat_model) -> ChatClient:
    """Function-scoped ChatClient.

    ``get_chat_client()`` is a thin Python-side wrapper over the model handle;
    each call to ``complete_chat`` builds its own native ``ChatSession`` internally.
    Function scope keeps tests isolated and signals that the client is cheap to create.
    """
    client = chat_model.get_chat_client()
    # Deterministic, short, fast — but high enough that reasoning models
    # (e.g. qwen3) can finish their <think> block and still emit visible text.
    client.settings.temperature = 0.0
    client.settings.max_tokens = 4096
    # Surface the selected model in CI logs (pytest captures stdout per-test and
    # emits it on failure) so we can confirm which catalog entry the test ran against.
    print(
        f"[chat_client] alias={chat_model.alias!r} "
        f"model_id={client.model_id!r} "
        f"task={chat_model.info.task!r} "
        f"size_mb={chat_model.info.file_size_mb}"
    )
    return client


# `/no_think` is the qwen3 documented opt-out for the <think> block. Non-reasoning
# models ignore it. This keeps the streaming content assertions deterministic regardless
# of how much reasoning budget the selected model would otherwise consume.
PROMPT = [{"role": "user", "content": "Reply with the single word: hello /no_think"}]


class TestNonStreaming:
    @pytest.mark.parametrize("cancel_before_start", [False, True])
    def test_cancellation_event_stops_inference_and_allows_subsequent_call(
        self, chat_client, cancel_before_start, cancel_on_native_processing
    ):
        from foundry_local_sdk.exception import FoundryLocalException

        chat_client.settings.max_tokens = 256
        event = cancel_on_native_processing
        if cancel_before_start:
            event.set()
        with pytest.raises(FoundryLocalException) as error:
            chat_client.complete_chat(
                [{"role": "user", "content": "Count from 1 to 10000, writing every integer on its own line."}],
                cancel_event=event,
            )
        assert error.value.error_code == 5

        chat_client.settings.max_tokens = 8
        assert chat_client.complete_chat(PROMPT).choices

    def test_returns_typed_completion(self, chat_client):
        resp = chat_client.complete_chat(PROMPT)
        assert isinstance(resp, ChatCompletion)

    def test_response_has_content(self, chat_client):
        resp = chat_client.complete_chat(PROMPT)
        assert resp.choices, "Response must contain at least one choice"
        msg = resp.choices[0].message
        assert msg.content is not None
        assert msg.content.strip(), "Assistant content must not be empty"

    def test_response_has_finish_reason(self, chat_client):
        resp = chat_client.complete_chat(PROMPT)
        assert resp.choices[0].finish_reason in {"stop", "length"}

    def test_response_has_usage_with_positive_counts(self, chat_client):
        resp = chat_client.complete_chat(PROMPT)
        assert resp.usage is not None
        assert resp.usage.prompt_tokens > 0
        assert resp.usage.completion_tokens > 0
        assert resp.usage.total_tokens == (
            resp.usage.prompt_tokens + resp.usage.completion_tokens
        )

    def test_response_model_field_matches_loaded_model(self, chat_client):
        resp = chat_client.complete_chat(PROMPT)
        # Some models echo a normalized id; just confirm it is present.
        assert resp.model

    def test_invalid_messages_rejected_before_native_call(self, chat_client):
        with pytest.raises(ValueError):
            chat_client.complete_chat([])


class TestStreaming:
    def test_yields_chunks(self, chat_client):
        chunks = list(chat_client.complete_streaming_chat(PROMPT))
        assert chunks, "Streaming should yield at least one chunk"
        for c in chunks:
            assert isinstance(c, ChatCompletionChunk)

    def test_concatenated_content_is_non_empty(self, chat_client):
        chunks = list(chat_client.complete_streaming_chat(PROMPT))
        # Concatenate visible content. Foundry Local may emit assistant text
        # under either delta.content (per-token) or as a single message-level
        # payload that the client normalises into a delta.
        parts: list[str] = []
        for c in chunks:
            if not c.choices:
                continue
            delta = c.choices[0].delta
            if delta.content:
                parts.append(delta.content)
        text = "".join(parts)
        assert text.strip(), (
            f"Streamed content was empty. Chunks: {len(chunks)}; "
            f"all chunks: ["
            + ", ".join(c.model_dump_json(exclude_none=True) for c in chunks)
            + "]"
        )

    def test_final_chunk_has_finish_reason(self, chat_client):
        chunks = list(chat_client.complete_streaming_chat(PROMPT))
        finish = None
        for c in chunks:
            if c.choices and c.choices[0].finish_reason:
                finish = c.choices[0].finish_reason
        assert finish in {"stop", "length"}

    def test_break_mid_stream_does_not_crash(self, chat_client):
        gen = chat_client.complete_streaming_chat(PROMPT)
        # Pull one chunk, then abandon — the finally-block in the session
        # must cancel the request and join the background thread cleanly.
        first = next(gen)
        assert isinstance(first, ChatCompletionChunk)
        gen.close()

        # Subsequent calls must still work.
        resp = chat_client.complete_chat(PROMPT)
        assert isinstance(resp, ChatCompletion)
