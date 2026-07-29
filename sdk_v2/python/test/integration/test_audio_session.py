# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Direct ``AudioSession`` tests — bypasses the OpenAI / LiveAudio wrappers.

Mirrors the C++ ``StreamingAudioFixture.StreamingCallbackReceivesTokens``
test in ``sdk_v2/cpp/test/sdk_api/streaming_audio_test.cc``: streams PCM
chunks into a session-borrowed ``ItemQueue`` while simultaneously
consuming ``SpeechSegmentItem``s via ``process_streaming_request``.
Verifies streamed-in + streamed-out plumbing through the typed Item API.
"""
from __future__ import annotations

from pathlib import Path

import pytest

from foundry_local_sdk import (
    AudioItem,
    AudioSession,
    BytesItem,
    ItemQueue,
    Request,
    SpeechResultItem,
    SpeechSegmentItem,
)


_RECORDING_PATH = Path(__file__).resolve().parents[3] / "testdata" / "Recording.pcm"

# 100 ms at 16 kHz mono s16le = 16000 * 0.1 * 2 bytes/sample.
_CHUNK_SIZE = 3200

_KEY_PHRASES = [
    "give people",
    "more than one link",
    "live concert",
    "behind the scenes",
    "photo gallery",
    "album to purchase",
]


def _split_into_chunks(data: bytes, chunk_size: int) -> list[bytes]:
    return [data[i:i + chunk_size] for i in range(0, len(data), chunk_size)]


def test_stream_pcm_and_consume_speech_segments(streaming_audio_model):
    """Streamed input + streamed output against the live-audio model.

    Pushes PCM chunks into the session's ``ItemQueue`` while consuming
    per-token ``SpeechSegmentItem``s via ``process_streaming_request``,
    then verifies the terminal ``SpeechResultItem`` aggregates the same
    segments. Skipped when ``testdata/Recording.pcm`` is missing.
    """
    if not _RECORDING_PATH.is_file():
        pytest.skip(f"testdata/Recording.pcm not found at {_RECORDING_PATH}")

    pcm = _RECORDING_PATH.read_bytes()
    assert len(pcm) > 0, "Recording.pcm is empty"
    chunks = _split_into_chunks(pcm, _CHUNK_SIZE)
    assert len(chunks) > 1

    # Format descriptor — no initial data, the actual bytes arrive via the queue.
    # Mirrors the C++ Item::AudioFromData("pcm", nullptr, 0, 16000, 1) shape.
    audio = AudioItem.create_format_descriptor("pcm", sample_rate=16000, channels=1)
    queue = ItemQueue()

    request = Request()
    request.add_item(audio, transfer_ownership=True)
    request.add_item(queue, transfer_ownership=False)

    with AudioSession(streaming_audio_model) as session:
        session.set_streaming(True)

        # The native session runs on a worker thread and consumes the audio
        # queue concurrently. ItemQueue is unbounded, so we can push every
        # chunk synchronously up-front — emitted segments accumulate on the
        # session's internal stream queue ready for the iterator below.
        for chunk in chunks:
            queue.push(BytesItem(chunk))
        queue.mark_finished()

        segment_count = 0
        streamed_text_parts: list[str] = []

        with session.process_streaming_request(request) as stream:
            for item in stream:
                # AudioSession streaming only emits SpeechSegment items.
                assert isinstance(item, SpeechSegmentItem), (
                    f"unexpected streamed item type: {type(item).__name__}"
                )
                segment_count += 1
                streamed_text_parts.append(item.text)
                # Release native handle eagerly; the result aggregate
                # produced on the terminal Response owns its own copies.
                item._close()

            assert segment_count > 0, "streaming callback should fire at least once"
            streamed_text = "".join(streamed_text_parts)
            assert streamed_text, "streamed transcription should not be empty"

            lower = streamed_text.lower()
            for phrase in _KEY_PHRASES:
                assert phrase in lower, (
                    f"Expected streamed transcription to contain '{phrase}'. Got: {streamed_text}"
                )

            with stream.final_response as final:
                # The terminal Response carries an aggregated SpeechResultItem
                # built from the streamed segments. Validate it matches what
                # we accumulated from the per-segment callbacks.
                result = next(
                    (it for it in final if isinstance(it, SpeechResultItem)),
                    None,
                )
                assert result is not None, "expected a SpeechResultItem on the terminal Response"
                assert len(result.segments) == segment_count
                assert result.text == streamed_text
