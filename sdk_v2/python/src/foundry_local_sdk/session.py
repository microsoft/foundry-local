# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
from __future__ import annotations

import abc
import enum
import queue
import threading
from collections.abc import Callable
from typing import TYPE_CHECKING, Iterator, cast

if TYPE_CHECKING:
    from foundry_local_sdk.imodel import IModel
    from foundry_local_sdk.items import Item
    from foundry_local_sdk.request import Request
    from foundry_local_sdk.response import Response
    from foundry_local_sdk.session_types import RequestOptions

_API_VERSION = 1  # FOUNDRY_LOCAL_API_VERSION

# Sentinel placed on the stream queue by the background thread when inference finishes.
_DONE = object()

# A Request_Cancel issued before ProcessRequest enters native code is an idle
# no-op. Retrying at this cadence closes that scheduling window without spinning.
_CANCEL_RETRY_INTERVAL_SECONDS = 0.05


class _StreamError:
    """Wraps an exception propagated from the background inference thread."""

    def __init__(self, exc: BaseException) -> None:
        self.exc = exc


class _State(enum.Enum):
    # NEW → ITERATING → {DONE, CANCELLED}. DONE includes worker-error completion (see _error).
    NEW = "new"
    ITERATING = "iterating"
    DONE = "done"
    CANCELLED = "cancelled"


class _SessionState(enum.Enum):
    OPEN = "open"
    CLOSING = "closing"
    CLOSED = "closed"


class StreamingResponse:
    """Result of :meth:`Session.process_streaming_request`.

    Iterable over streamed ``Item`` objects yielded as the model produces
    them. After the iterator drains, :attr:`final_response` exposes the
    terminal ``Response`` carrying ``finish_reason``, ``get_usage()``,
    and any aggregated items (e.g. ``AudioSession``'s full transcript
    ``TextItem``).

    Use as a context manager so the terminal Response is always released::

        with session.process_streaming_request(req) as stream:
            for item in stream:
                ...  # incremental
            with stream.final_response as final:
                print(final.finish_reason, final.get_usage())

    The wrapper can be iterated at most once; a second ``iter()`` raises.
    """

    def __init__(
        self,
        session: "Session",
        request: "Request",
        session_ptr: object,
        release_call: Callable[[], None],
    ) -> None:
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        self._session = session
        self._request = request
        self._queue: queue.Queue = queue.Queue()
        # The consumer checks the final response and error after dequeuing _DONE.
        self._final_response: "Response | None" = None
        self._error: BaseException | None = None
        self._state: _State = _State.NEW
        self._final_consumed = False
        self._closed = False
        self._lock_released = False
        self._thread: threading.Thread | None = None

        session._stream_queue = self._queue

        def _run() -> None:
            try:
                out = ffi.new("flResponse**")
                api.check_status(
                    api.inference.Session_ProcessRequest(session_ptr, request._ptr, out)
                )
                from foundry_local_sdk.response import Response

                # Response takes ownership of out[0].
                self._final_response = Response(out[0])
            except Exception as exc:
                self._error = exc
                self._queue.put(_StreamError(exc))
            finally:
                session._stream_queue = None
                try:
                    self._queue.put(_DONE)
                finally:
                    release_call()

        t = threading.Thread(target=_run, daemon=True)
        session._stream_thread = t
        session._stream_request = request
        self._thread = t
        try:
            t.start()
        except BaseException:
            self._closed = True
            session._stream_thread = None
            session._stream_request = None
            session._stream_queue = None
            if t.ident is not None:
                try:
                    request.cancel()
                finally:
                    t.join()
            raise

    def _release_lock(self) -> None:
        if self._lock_released:
            return
        self._lock_released = True
        self._session._stream_thread = None
        self._session._stream_request = None
        try:
            self._session._streaming_in_flight.release()
        except RuntimeError:
            pass

    def _cancel_and_drain_and_join(self) -> None:
        """Cancel until the worker settles, then release its queued items."""
        while self._thread is not None and self._thread.is_alive():
            try:
                self._request.cancel()
            except Exception:
                pass

            # A timed join leaves cancellation opportunities after the worker
            # crosses from Python scheduling into its native invocation.
            self._thread.join(_CANCEL_RETRY_INTERVAL_SECONDS)

        while True:
            msg = self._queue.get()
            if msg is _DONE:
                break
            # Dropping an Item releases its native handle.
            del msg

    def __iter__(self) -> Iterator["Item"]:
        from foundry_local_sdk.exception import FoundryLocalException

        if self._state is not _State.NEW:
            raise FoundryLocalException("StreamingResponse can only be iterated once.")
        self._state = _State.ITERATING

        try:
            while True:
                msg = self._queue.get()
                if msg is _DONE:
                    self._state = _State.DONE
                    if self._thread is not None:
                        self._thread.join()
                    self._release_lock()
                    return
                if isinstance(msg, _StreamError):
                    # Consume _DONE before joining the worker.
                    while self._queue.get() is not _DONE:
                        pass
                    self._state = _State.DONE
                    if self._thread is not None:
                        self._thread.join()
                    self._release_lock()
                    raise msg.exc
                yield msg
        finally:
            if self._state is _State.ITERATING:
                # Cancel and join when iteration stops early.
                self._cancel_and_drain_and_join()
                self._state = _State.CANCELLED
                # A cancelled stream has no usable final response.
                if self._final_response is not None:
                    try:
                        self._final_response._close()
                    except Exception:
                        pass
                    self._final_response = None
                self._release_lock()

    @property
    def final_response(self) -> "Response":
        """The terminal :class:`Response`. Available after the iterator completes.

        Raises:
            FoundryLocalException: If accessed before iteration completes,
                or if the stream was cancelled.
            Exception: Re-raises any error encountered by the worker.

        The caller owns the returned ``Response`` and must close it (use ``with``).
        """
        from foundry_local_sdk.exception import FoundryLocalException

        if self._state in (_State.NEW, _State.ITERATING):
            raise FoundryLocalException(
                "final_response is not available until the stream has been fully consumed."
            )
        if self._error is not None:
            raise self._error
        if self._state is _State.CANCELLED:
            raise FoundryLocalException("Stream was cancelled.")
        if self._final_response is None:
            raise FoundryLocalException("final_response is unavailable.")
        self._final_consumed = True
        return self._final_response

    def __enter__(self) -> "StreamingResponse":
        return self

    def __exit__(self, *exc) -> None:
        if self._closed:
            return
        self._closed = True
        try:
            if self._state in (_State.NEW, _State.ITERATING):
                # Cancel if iteration never started or stopped early.
                self._cancel_and_drain_and_join()
                self._state = _State.CANCELLED
            if self._final_response is not None and not self._final_consumed:
                try:
                    self._final_response._close()
                except Exception:
                    pass
                self._final_response = None
        finally:
            self._release_lock()

    def __del__(self) -> None:
        # Use a context manager for explicit cleanup.
        try:
            self.__exit__(None, None, None)
        except Exception:
            pass


class Session(abc.ABC):
    """Base inference session wrapping a native flSession*.

    Provides synchronous request processing, session-level options, and
    synchronous streaming via ``set_streaming`` + ``process_streaming_request``.
    """

    def __init__(self, model: "IModel") -> None:
        # Set cleanup fields before session creation can fail.
        self._closed = True
        self._ptr = None
        self._stream_thread = None
        self._stream_request = None
        self._state_condition = threading.Condition()
        self._session_state = _SessionState.CLOSED
        self._active_native_calls = 0
        # Avoid imports from __del__ during interpreter shutdown.
        self._release_session: Callable[[object], object] | None = None
        self._cancel_session: Callable[[object], object] | None = None
        self._check_status: Callable[[object], None] | None = None

        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api
        from foundry_local_sdk.imodel import _ModelImpl

        if not isinstance(model, _ModelImpl):
            raise TypeError("model must be a native IModel instance")

        out = ffi.new("flSession**")
        api.check_status(api.inference.Session_Create(model._ptr, out))
        self._ptr = out[0]
        self._release_session = cast(Callable[[object], object], api.inference.Session_Release)
        self._cancel_session = cast(Callable[[object], object], api.inference.Session_Cancel)
        self._check_status = cast(Callable[[object], None], api.check_status)
        self._session_state = _SessionState.OPEN
        self._closed = False

        self._streaming_enabled = False
        self._streaming_callback = None
        self._stream_queue: queue.Queue | None = None

        # Reject concurrent streams because they share one callback and queue.
        self._streaming_in_flight = threading.Lock()

    def _check_open(self) -> None:
        from foundry_local_sdk.exception import FoundryLocalException

        with self._state_condition:
            is_open = self._session_state is _SessionState.OPEN
        if not is_open:
            raise FoundryLocalException(
                f"{type(self).__name__} has been closed and can no longer be used."
            )

    def _acquire_call(self) -> object:
        from foundry_local_sdk.exception import FoundryLocalException

        with self._state_condition:
            if self._session_state is not _SessionState.OPEN or self._ptr is None:
                raise FoundryLocalException(
                    f"{type(self).__name__} has been closed and can no longer be used."
                )
            self._active_native_calls += 1
            return self._ptr

    def _release_call(self) -> None:
        with self._state_condition:
            self._active_native_calls -= 1
            if self._active_native_calls == 0:
                self._state_condition.notify_all()

    def set_options(self, options: "RequestOptions") -> "Session":
        """Set session-level inference options. Applies to all subsequent process_request calls."""
        self._check_open()
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        native_options = options.to_native_options()
        kvp_out = ffi.new("flKeyValuePairs**")
        api.root.CreateKeyValuePairs(kvp_out)
        kvp = kvp_out[0]
        try:
            for key, value in native_options.items():
                api.root.AddKeyValuePair(kvp, key.encode("utf-8"), value.encode("utf-8"))
            api.check_status(api.inference.Session_SetOptions(self._ptr, kvp))
        finally:
            api.root.KeyValuePairs_Release(kvp)
        return self

    def set_streaming(self, enabled: bool) -> "Session":
        """Install or remove the native streaming callback on this session.

        Must be called with ``True`` before ``process_streaming_request``.
        The callback remains installed across requests until disabled or the
        session is closed.

        Returns:
            self (fluent).
        """
        self._check_open()
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api
        from foundry_local_sdk.items import Item

        if enabled and not self._streaming_enabled:
            def _cb(data, user_data):
                q = self._stream_queue
                if q is None:
                    return 0

                try:
                    if data.item_queue != ffi.NULL:
                        item_out = ffi.new("flItem**")
                        while api.item.ItemQueue_TryPop(data.item_queue, item_out):
                            # Item owns the popped native handle.
                            q.put(Item.from_native(item_out[0], owns=True))
                except Exception as exc:
                    q.put(_StreamError(exc))
                    return 1

                return 0

            # Keep the callback alive while native code stores its function pointer.
            self._streaming_callback = ffi.callback("flStreamingCallback", _cb)
            self._streaming_enabled = True
            api.check_status(
                api.inference.Session_SetStreamingCallback(
                    self._ptr, self._streaming_callback, ffi.NULL
                )
            )

        elif not enabled and self._streaming_enabled:
            # A null function pointer removes the callback.
            api.check_status(
                api.inference.Session_SetStreamingCallback(
                    self._ptr, ffi.cast("flStreamingCallback", 0), ffi.NULL
                )
            )
            self._streaming_callback = None
            self._streaming_enabled = False

        return self

    def process_streaming_request(
        self,
        request: "Request",
    ) -> "StreamingResponse":
        """Run a request and stream the items produced by the model.

        Returns a :class:`StreamingResponse` wrapper that is iterable over
        ``Item`` objects in production order. After the iterator drains,
        :attr:`StreamingResponse.final_response` exposes the terminal
        :class:`Response` carrying ``finish_reason``, ``get_usage()``, and
        any aggregated items the session produces on completion.

        AudioSession streams per-token items via the streaming callback
        (some are interim hypotheses, some final), and *additionally*
        produces an aggregated transcript ``TextItem`` on the terminal
        Response. Other sessions (chat, embeddings) typically don't add
        non-streamed items to the final Response, but ``final_response``
        is still useful for ``finish_reason`` and ``get_usage()``.

        Runs ``Session_ProcessRequest`` in a background thread. The native
        streaming callback populates an unbounded ``queue.Queue``; the
        iterator drains it synchronously.

        Abandoning the iterator (``break`` / exception / generator close)
        automatically calls ``request.cancel()`` and joins the worker so
        the session is ready for the next request. Use ``with`` on the
        returned wrapper to guarantee cleanup of the terminal Response.

        Requires ``set_streaming(True)`` to have been called first.

        Raises:
            FoundryLocalException: If streaming was not enabled, another
                streaming request is already in flight on this session,
                or the worker encounters a native error.
        """
        from foundry_local_sdk.exception import FoundryLocalException

        session_ptr = self._acquire_call()
        release_lock = threading.Lock()
        released = False

        def release_call() -> None:
            nonlocal released
            with release_lock:
                if released:
                    return
                released = True
            self._release_call()

        lease_transferred = False
        try:
            if not self._streaming_enabled:
                raise FoundryLocalException(
                    "Streaming not enabled. Call set_streaming(True) before process_streaming_request."
                )

            if not self._streaming_in_flight.acquire(blocking=False):
                raise FoundryLocalException(
                    "Concurrent streaming requests on the same session are not supported. "
                    "Drain or cancel the in-flight stream before starting another."
                )

            try:
                response = StreamingResponse(self, request, session_ptr, release_call)
            except BaseException:
                self._streaming_in_flight.release()
                raise
            lease_transferred = True
            return response
        finally:
            if not lease_transferred:
                release_call()

    def process_request(self, request: "Request", timeout: "float | None" = None) -> "Response":
        """Run the request synchronously and return the complete response.

        Args:
            request: The request to run.
            timeout: If provided, set ``request``'s timeout in seconds before processing.
                The setting remains on ``request``.
        """
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api
        from foundry_local_sdk.response import Response

        session_ptr = self._acquire_call()
        try:
            if timeout is not None:
                request.set_timeout(timeout)

            out = ffi.new("flResponse**")
            api.check_status(api.inference.Session_ProcessRequest(session_ptr, request._ptr, out))
            return Response(out[0])
        finally:
            self._release_call()

    def cancel(self) -> None:
        """Permanently cancel this session.

        Active and queued requests are cancelled. Later request processing fails.
        Thread-safe and idempotent.
        """
        from foundry_local_sdk._native.api import api

        session_ptr = self._acquire_call()
        try:
            api.check_status(api.inference.Session_Cancel(session_ptr))
        finally:
            self._release_call()

    def _close(self) -> None:
        # Subclasses may fail before super().__init__, leaving cleanup fields unset.
        condition = getattr(self, "_state_condition", None)
        if condition is None:
            return

        with condition:
            if self._session_state is _SessionState.CLOSED:
                return
            if self._session_state is _SessionState.CLOSING:
                while self._session_state is _SessionState.CLOSING:
                    condition.wait()
                return

            self._session_state = _SessionState.CLOSING
            self._closed = True
            ptr = self._ptr
            cancel_session = self._cancel_session
            check_status = self._check_status

        # Cancel at Session scope so streaming and non-streaming calls can unwind.
        # Do not hold the condition because Session_Cancel may block.
        try:
            if ptr is not None and cancel_session is not None:
                status = cancel_session(ptr)
                if check_status is not None:
                    check_status(status)
        except Exception:
            pass

        with condition:
            # The native Session must outlive every native call.
            while self._active_native_calls:
                condition.wait()
            ptr = self._ptr
            release_session = self._release_session

        try:
            if ptr is not None and release_session is not None:
                release_session(ptr)
        except Exception:
            pass
        finally:
            with condition:
                self._ptr = None
                self._session_state = _SessionState.CLOSED
                condition.notify_all()

    def _finalize(self) -> None:
        """Release only if the condition is free and the session is open and idle."""
        condition = getattr(self, "_state_condition", None)
        if condition is None:
            return

        try:
            if not condition.acquire(blocking=False):
                return
            try:
                if (
                    self._session_state is not _SessionState.OPEN
                    or self._active_native_calls
                ):
                    return
                self._closed = True
                ptr = self._ptr
                release_session = self._release_session
                self._ptr = None
                self._session_state = _SessionState.CLOSED
                condition.notify_all()
            finally:
                condition.release()

            try:
                if ptr is not None and release_session is not None:
                    release_session(ptr)
            except Exception:
                pass
        except Exception:
            pass

    def __enter__(self) -> "Session":
        return self

    def __exit__(self, *_) -> None:
        self._close()

    def __del__(self) -> None:
        self._finalize()


class ChatSession(Session):
    """Inference session for chat-completion and vision-language-chat models.

    Validates the model task at construction time. Supports tool definitions,
    turn count inspection, and turn undo.
    """

    _SUPPORTED_TASKS = frozenset({"chat-completion", "vision-language-chat"})

    def __init__(self, model: "IModel") -> None:
        task = model.info.task
        if task not in self._SUPPORTED_TASKS:
            raise ValueError(
                f"ChatSession requires a model with task 'chat-completion' or "
                f"'vision-language-chat', but got {task!r}."
            )
        super().__init__(model)

    def add_tool_definition(self, name: str, description: str, json_schema: str) -> "ChatSession":
        """Register a tool so the model can request tool calls. Returns self (fluent)."""
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        # Keep cffi temporaries as named locals so they outlive the native call.
        c_name = ffi.new("char[]", name.encode("utf-8") + b"\x00")
        c_desc = ffi.new("char[]", description.encode("utf-8") + b"\x00")
        c_schema = ffi.new("char[]", json_schema.encode("utf-8") + b"\x00")

        tool_def = ffi.new("flToolDefinition*")
        tool_def.version = _API_VERSION
        tool_def.name = c_name
        tool_def.description = c_desc
        tool_def.json_schema = c_schema

        api.check_status(api.inference.Session_AddToolDefinition(self._ptr, tool_def))
        return self

    def remove_tool_definition(self, name: str) -> bool:
        """Remove a previously-added tool definition by name.

        Returns True if a matching tool was found and removed, False if no tool with that
        name was registered. Useful when the available tool set changes mid-conversation.
        """
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        c_name = ffi.new("char[]", name.encode("utf-8") + b"\x00")
        out_removed = ffi.new("bool*")
        api.check_status(api.inference.Session_RemoveToolDefinition(self._ptr, c_name, out_removed))
        return bool(out_removed[0])

    @property
    def turn_count(self) -> int:
        """Number of completed turns accumulated in this session."""
        from foundry_local_sdk._native.api import api

        return int(api.inference.Session_GetTurnCount(self._ptr))

    def undo_turns(self, count: int) -> None:
        """Remove the last `count` turns from session history."""
        from foundry_local_sdk._native.api import api

        api.check_status(api.inference.Session_UndoTurns(self._ptr, count))


class AudioSession(Session):
    """Inference session for automatic-speech-recognition models.

    Accepts ``AudioItem`` input and produces ``TextItem`` output.
    Validates the model task at construction time.
    """

    def __init__(self, model: "IModel") -> None:
        task = model.info.task
        if task != "automatic-speech-recognition":
            raise ValueError(
                f"AudioSession requires a model with task 'automatic-speech-recognition', "
                f"but got {task!r}."
            )
        super().__init__(model)


class EmbeddingsSession(Session):
    """Inference session for text-embedding models.

    Accepts ``TextItem`` inputs and produces one ``TensorItem`` per input
    containing the embedding vector. Stateless — multiple requests can be
    processed concurrently against the same loaded model.
    Validates the model task at construction time.
    """

    def __init__(self, model: "IModel") -> None:
        task = model.info.task
        if task != "embeddings":
            raise ValueError(
                f"EmbeddingsSession requires a model with task 'embeddings', "
                f"but got {task!r}."
            )
        super().__init__(model)
