# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Live audio transcription session integration tests.

The state-machine tests do not require a real model — they exercise the
Python wrapper's guard logic only.  The smoke test requires an ASR model
that supports streaming (e.g. a Nemotron / Whisper variant wired to the
live-audio path).  When no live-audio-capable model is available, the
smoke test skips cleanly.

TODO: pin the smoke test to a known live-audio model alias once one is
available in the test cache.
"""
from __future__ import annotations

import threading
import time
from pathlib import Path

import pytest

from foundry_local_sdk.exception import FoundryLocalException
from foundry_local_sdk.openai.live_audio_session import LiveAudioTranscriptionSession
from foundry_local_sdk.openai.live_audio_types import LiveAudioTranscriptionResponse


# Recording.pcm is raw s16le, 16 kHz, mono PCM (no RIFF header) — see the
# matching C++ test in sdk_v2/cpp/test/sdk_api/streaming_audio_test.cc.
# Distinct from Recording.wav (a real WAV used by offline transcribe() tests).
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


@pytest.fixture
def audio_client(streaming_audio_model):
    """Function-scoped AudioClient.

    ``get_audio_client()`` is a thin Python-side wrapper over the model handle;
    each call to ``transcribe`` (and each ``create_live_transcription_session()``)
    builds its own native ``AudioSession`` internally. Function scope keeps tests
    isolated and signals that the client is cheap to create.
    """
    return streaming_audio_model.get_audio_client()


def test_session_factory_returns_session(audio_client):
    session = audio_client.create_live_transcription_session()
    try:
        assert isinstance(session, LiveAudioTranscriptionSession)
    finally:
        session.close()


def test_state_machine_rejects_misordered_calls(audio_client):
    session = audio_client.create_live_transcription_session()
    try:
        # append() before start() must raise.
        with pytest.raises(FoundryLocalException):
            session.append(b"\x00\x00")

        # get_stream() is a generator — error is raised on iteration entry.
        with pytest.raises(FoundryLocalException):
            next(iter(session.get_stream()))

        # stop() before start() is a no-op.
        session.stop()
    finally:
        session.close()


def test_double_start_is_rejected(audio_client):
    session = audio_client.create_live_transcription_session()
    try:
        try:
            session.start()
        except FoundryLocalException:
            # Some models / runtimes may reject the descriptor synchronously;
            # in that case there's nothing to double-start.
            pytest.skip("Could not start a live audio session for double-start check.")
            return

        with pytest.raises(FoundryLocalException):
            session.start()
    finally:
        session.close()


def test_smoke_start_append_stop(audio_client):
    """End-to-end: start, push a quarter-second of silence, stop.

    Skipped if the model rejects the live-audio request (e.g. it's an
    offline-only ASR model that doesn't speak the streaming protocol).
    """
    session = audio_client.create_live_transcription_session()
    try:
        try:
            session.start()
        except FoundryLocalException as e:
            pytest.skip(f"Live audio not supported by this model: {e}")
            return

        # 0.25 s of 16-bit mono silence at 16 kHz.
        silence = b"\x00\x00" * (16000 // 4)
        session.append(silence)

        # Read the stream with a short timeout — some models stay silent
        # on a silence-only buffer; that's fine, we just want no crash.
        stream = session.get_stream()
        deadline = time.time() + 2.0
        first_response = None
        drain_error: list[BaseException] = []

        def _drain_with_timeout():
            nonlocal first_response
            try:
                for resp in stream:
                    first_response = resp
                    break
            except BaseException as e:  # noqa: BLE001 — surfaced via drain_error below
                drain_error.append(e)

        import threading

        t = threading.Thread(target=_drain_with_timeout, daemon=True)
        t.start()
        while time.time() < deadline and first_response is None and t.is_alive():
            time.sleep(0.05)

        # Whether or not we got a response, stop should drain cleanly.
        session.stop()
        # The drain thread must finish once stop releases the stream.
        t.join(timeout=2.0)

        # Native streaming rejected the model (e.g. Whisper instead of Nemotron) —
        # the error fires inside the drain thread, after start() has already returned.
        if drain_error and isinstance(drain_error[0], FoundryLocalException):
            pytest.skip(f"Live audio streaming not supported by this model: {drain_error[0]}")
        elif drain_error:
            raise drain_error[0]

        if first_response is not None:
            assert hasattr(first_response, "content")
            assert isinstance(first_response.content, list)
    finally:
        session.close()


def test_stream_recording_in_chunks_and_validate_transcription(audio_client):
    """Mirrors C++ StreamRecordingInChunksAndValidateTranscription.

    Streams testdata/Recording.pcm (raw s16le, 16 kHz, mono) through a live
    transcription session in 100 ms chunks while a background reader drains
    the response stream. Asserts the final transcription contains expected
    key phrases.

    Skips if testdata/Recording.pcm is missing or the audio model rejects
    the live-audio protocol.
    """
    if not _RECORDING_PATH.is_file():
        pytest.skip(f"testdata/Recording.pcm not found at {_RECORDING_PATH}")

    pcm = _RECORDING_PATH.read_bytes()
    assert len(pcm) > 0, "Recording.pcm is empty"

    chunks = _split_into_chunks(pcm, _CHUNK_SIZE)
    assert len(chunks) > 1

    session = audio_client.create_live_transcription_session()
    session.settings.language = "en"
    try:
        try:
            session.start()
        except FoundryLocalException as e:
            pytest.skip(f"Live audio not supported by this model: {e}")
            return

        responses: list[LiveAudioTranscriptionResponse] = []
        drain_error: list[BaseException] = []

        def _drain() -> None:
            # The stream iterator naturally ends once stop() finalises the
            # session and the underlying queue is drained.
            try:
                for resp in session.get_stream():
                    responses.append(resp)
            except BaseException as e:  # noqa: BLE001 — surfaced via drain_error below
                drain_error.append(e)

        drain_thread = threading.Thread(target=_drain, daemon=True)
        drain_thread.start()

        for chunk in chunks:
            session.append(chunk)

        session.stop()
        drain_thread.join(timeout=60.0)
        assert not drain_thread.is_alive(), "drain thread did not finish after stop()"

        # Native streaming rejected the model (e.g. Whisper instead of Nemotron) —
        # the error fires inside the drain thread, after start() has already returned.
        if drain_error and isinstance(drain_error[0], FoundryLocalException):
            pytest.skip(f"Live audio streaming not supported by this model: {drain_error[0]}")
        elif drain_error:
            raise drain_error[0]

        # Concatenate text from every response. `text` and `transcript` carry
        # the same value in the current native shape (see live_audio_types.py),
        # so prefer `transcript` and fall back to `text`. Each native response
        # carries a single subword token that already includes its own leading
        # whitespace, so concatenate without inserting separators.
        parts: list[str] = []
        for resp in responses:
            for part in resp.content:
                chunk_text = part.transcript or part.text
                if chunk_text:
                    parts.append(chunk_text)
        full_transcript = "".join(parts).strip()

        print(f"Streaming transcription: {full_transcript}")
        assert full_transcript, "Transcription should not be empty"

        lower = full_transcript.lower()
        for phrase in _KEY_PHRASES:
            assert phrase in lower, (
                f"Expected transcription to contain {phrase!r}.\nGot: {full_transcript}"
            )
    finally:
        session.close()
