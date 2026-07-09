# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Live audio transcription streaming session.

Provides :class:`LiveAudioTranscriptionSession` — a push-based streaming
session for real-time audio-to-text transcription via ONNX Runtime GenAI.

Usage
-----
Push PCM audio chunks via :meth:`LiveAudioTranscriptionSession.append` and
consume transcription results via :meth:`LiveAudioTranscriptionSession.get_stream`.
Use the session as a context manager (or call :meth:`close`) to release
native handles deterministically.

Error handling
--------------
All session operations raise :class:`FoundryLocalException` on failure.
Common failure modes:

- **Lifecycle errors** — raised by :meth:`start`, :meth:`append`,
  :meth:`get_stream`, and :meth:`stop` when called in an invalid state
  (e.g. ``append()`` before ``start()``, or any call after :meth:`close`).
  Message identifies the current state and what was expected.
- **Native session errors** — raised when the native ``AudioSession``
  fails to start or process the request. These surface synchronously from
  :meth:`start` if setup fails, or from inside :meth:`get_stream` if the
  background worker thread observes a fatal native error.

Once the worker thread raises, the session is terminated and must be
re-created.
"""
from __future__ import annotations

import queue
import threading
from enum import IntEnum
from typing import TYPE_CHECKING, Iterator

from foundry_local_sdk.exception import FoundryLocalException
from foundry_local_sdk.openai.live_audio_types import (
    LiveAudioTranscriptionOptions,
    LiveAudioTranscriptionResponse,
    TranscriptionContentPart,
)

if TYPE_CHECKING:
    from foundry_local_sdk.imodel import IModel
    from foundry_local_sdk.items import Item


# Sentinel placed on the response queue when the background thread finishes.
_DONE = object()


class _StreamError:
    """Wraps an exception propagated from the background inference thread."""

    def __init__(self, exc: BaseException) -> None:
        self.exc = exc


class _State(IntEnum):
    CREATED = 0
    STARTED = 1
    STOPPED = 2
    DISPOSED = 3


class LiveAudioTranscriptionSession:
    """Session for real-time audio streaming ASR (Automatic Speech Recognition).

    Audio data from a microphone (or other source) is pushed in as PCM
    chunks via :meth:`append`, and transcription results are returned as a
    synchronous iterator via :meth:`get_stream`.

    Created via :meth:`AudioClient.create_live_transcription_session`.

    State machine: ``CREATED → STARTED → STOPPED → DISPOSED``.

    Thread safety
    -------------
    :meth:`append` can be called from any thread (including high-frequency
    audio device callbacks). Audio chunks are handed to the native input
    queue, which serializes them in push order. The native queue is
    unbounded, so callers that produce audio faster than the model can
    consume it are responsible for their own pacing or backpressure.

    Example::

        with audio_client.create_live_transcription_session() as session:
            session.settings.sample_rate = 16000
            session.settings.channels = 1
            session.settings.language = "en"

            session.start()

            # Push audio from a microphone callback (thread-safe)
            session.append(pcm_bytes)

            # Read results as they arrive
            for result in session.get_stream():
                print(result.content[0].text, end="", flush=True)

            session.stop()
    """

    def __init__(self, model_id: str, model: "IModel") -> None:
        self.model_id = model_id
        # Hold the IModel reference so the underlying native model pointer
        # cannot be released out from under us while this session is alive.
        self._model = model
        self.settings = LiveAudioTranscriptionOptions()

        self._state = _State.CREATED
        self._active_settings: LiveAudioTranscriptionOptions | None = None
        self._queue = None  # ItemQueue (input audio queue, owned by us)
        self._audio_session = None  # AudioSession wrapping flSession*
        self._request = None  # Request
        self._response_queue: queue.Queue | None = None
        self._thread: threading.Thread | None = None

    # ------------------------------------------------------------------
    # Lifecycle
    # ------------------------------------------------------------------

    def start(self) -> None:
        """Start the streaming session.

        Must be called before :meth:`append` or :meth:`get_stream`.
        Settings are snapshot-frozen by this call — subsequent mutation of
        ``self.settings`` has no effect on the running session.

        Raises:
            FoundryLocalException: If the session is not in the ``CREATED``
                state (already started, already stopped, or disposed), or
                if the native ``AudioSession`` fails to start.
        """
        if self._state == _State.DISPOSED:
            raise FoundryLocalException("Session is disposed.")
        if self._state != _State.CREATED:
            raise FoundryLocalException(
                f"Session can only be started once (was {self._state.name})."
            )

        from foundry_local_sdk.item_queue import ItemQueue
        from foundry_local_sdk.items import AudioItem
        from foundry_local_sdk.request import Request
        from foundry_local_sdk.session import AudioSession

        # Snapshot settings — mutation after start() has no effect.
        active = self.settings.snapshot()
        self._active_settings = active

        response_queue: queue.Queue = queue.Queue()
        self._response_queue = response_queue

        # Create the input queue first; if anything later fails we close it.
        item_queue = ItemQueue()

        format_descriptor = AudioItem.create_format_descriptor(
            "pcm", active.sample_rate, active.channels
        )

        audio_session: "AudioSession | None" = None
        request: "Request | None" = None
        try:
            audio_session = AudioSession(self._model)
            audio_session.set_streaming(True)

            request = Request()
            # format_descriptor: ownership transfers to the request.
            request.add_item(format_descriptor)
            # item_queue: keep ownership — append() pushes into it.
            request.add_item(item_queue, transfer_ownership=False)
        except Exception:
            if request is not None:
                request._close()
            if audio_session is not None:
                audio_session._close()
            item_queue._close()
            self._response_queue = None
            raise

        self._queue = item_queue
        self._audio_session = audio_session
        self._request = request

        def _run() -> None:
            try:
                # AudioSession.process_streaming_request runs Session_ProcessRequest in its own
                # background thread and returns a StreamingResponse iterable; we translate each
                # streamed item into a LiveAudioTranscriptionResponse and push it onto the
                # consumer-facing response queue. The terminal Response carries the consolidated
                # transcript, which we emit as a separate aggregated response after the stream
                # drains.
                with audio_session.process_streaming_request(request) as stream:
                    for item in stream:
                        response = self._translate(item)
                        if response is not None:
                            response_queue.put(response)

                    from foundry_local_sdk.items import SpeechResultItem

                    final_text: str | None = None
                    with stream.final_response as final:
                        for it in final:
                            if isinstance(it, SpeechResultItem) and it.text:
                                # The result's text is the canonical concatenated transcript.
                                final_text = it.text
                                break

                if final_text:
                    response_queue.put(
                        LiveAudioTranscriptionResponse(
                            content=[TranscriptionContentPart(text=final_text, transcript=final_text)],
                            is_final=True,
                        )
                    )
            except Exception as exc:
                response_queue.put(_StreamError(exc))
            finally:
                response_queue.put(_DONE)

        self._thread = threading.Thread(target=_run, daemon=True)
        self._thread.start()
        self._state = _State.STARTED

    def append(self, pcm_data: bytes | bytearray | memoryview) -> None:
        """Push a chunk of raw PCM audio into the streaming session.

        Safe to call from any thread, including audio device callbacks.
        Chunks are wrapped in a native item and pushed onto the input
        queue in FIFO order. The native queue is unbounded — this call
        does not block on a slow consumer, so callers producing audio
        faster than real-time are responsible for their own pacing.

        Args:
            pcm_data: Raw PCM audio bytes matching the configured format
                (sample rate, channels, bits-per-sample). The buffer is
                consumed immediately; the caller may reuse it on return.

        Raises:
            FoundryLocalException: If the session is not in the ``STARTED``
                state (not started, already stopped, or disposed).
        """
        if self._state == _State.DISPOSED:
            raise FoundryLocalException("Session is disposed.")
        if self._state != _State.STARTED:
            raise FoundryLocalException(
                f"Session must be Started to append audio (was {self._state.name})."
            )

        from foundry_local_sdk.items import BytesItem

        bytes_item = BytesItem(pcm_data)
        # Push transfers ownership of bytes_item into the native queue.
        self._queue.push(bytes_item)

    def get_stream(self) -> Iterator[LiveAudioTranscriptionResponse]:
        """Yield transcription results as they arrive from the native session.

        The iterator completes when :meth:`stop` is called and all
        remaining audio has been drained, or when the background worker
        thread terminates (cleanly or with an error).

        Yields:
            ``LiveAudioTranscriptionResponse`` objects as the native ASR
            engine produces them.

        Raises:
            FoundryLocalException: If the session is not in the ``STARTED``
                state. Also re-raised from inside the iterator if the
                background worker observed a fatal native error — once
                this happens the session is terminated and must be
                re-created.
        """
        if self._state == _State.DISPOSED:
            raise FoundryLocalException("Session is disposed.")
        if self._state != _State.STARTED:
            raise FoundryLocalException(
                f"Session must be Started to read stream (was {self._state.name})."
            )

        q = self._response_queue
        assert q is not None  # state-machine invariant

        while True:
            msg = q.get()
            if msg is _DONE:
                break
            if isinstance(msg, _StreamError):
                raise msg.exc
            yield msg

    def stop(self) -> None:
        """Signal end-of-audio and wait for the background thread to drain.

        Marking the input queue as finished tells the native session that
        no more audio will arrive; the model finishes processing whatever
        is already queued and the worker exits naturally. Any final
        results are delivered through :meth:`get_stream` before its
        iterator completes.

        Idempotent on a never-started or already-stopped session — both
        are no-ops. Raises on a disposed session.

        Raises:
            FoundryLocalException: If :meth:`close` has already been
                called.
        """
        if self._state == _State.DISPOSED:
            raise FoundryLocalException("Session is disposed.")
        if self._state != _State.STARTED:
            # CREATED (never started) or already STOPPED — no-op.
            return

        # Tell the native side no more items will be pushed. The model drains naturally.
        self._queue.mark_finished()

        if self._thread is not None:
            self._thread.join()

        self._state = _State.STOPPED

    def close(self) -> None:
        """Release all native handles. Idempotent."""
        if self._state == _State.DISPOSED:
            return

        if self._state == _State.STARTED:
            try:
                self.stop()
            except Exception:
                pass

        # Drain any remaining items from the response queue so their native
        # handles are released by the Item GC path rather than leaking.
        if self._response_queue is not None:
            try:
                while True:
                    msg = self._response_queue.get_nowait()
                    if isinstance(msg, _StreamError):
                        # Errors carry no native handle; nothing to release.
                        continue
                    # _DONE sentinel and LiveAudioTranscriptionResponse are
                    # plain Python objects; nothing further to do.
                    _ = msg
            except queue.Empty:
                pass
            self._response_queue = None

        if self._queue is not None:
            self._queue._close()
            self._queue = None

        if self._request is not None:
            self._request._close()
            self._request = None

        if self._audio_session is not None:
            self._audio_session._close()
            self._audio_session = None

        self._state = _State.DISPOSED

    # ------------------------------------------------------------------
    # Internals
    # ------------------------------------------------------------------

    def _translate(self, item: "Item") -> "LiveAudioTranscriptionResponse | None":
        """Translate a streaming Item into a LiveAudioTranscriptionResponse, or None to drop it."""
        from foundry_local_sdk.items import SpeechSegmentItem

        # is_final stays False here: SpeechSegmentKind.FINAL on intermediate segments
        # marks the end of an utterance, not the end of the stream. The trailing
        # is_final=True event comes from the final-Response drain.
        if isinstance(item, SpeechSegmentItem) and item.text:
            return LiveAudioTranscriptionResponse(
                content=[TranscriptionContentPart(text=item.text, transcript=item.text)],
                is_final=False,
            )
        return None

    def __enter__(self) -> "LiveAudioTranscriptionSession":
        return self

    def __exit__(self, *_) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass
