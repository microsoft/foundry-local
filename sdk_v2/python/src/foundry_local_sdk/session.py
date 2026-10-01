# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
from __future__ import annotations

import abc
import enum
import json
import math
import queue
import threading
from collections.abc import Iterator
from contextlib import contextmanager
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from foundry_local_sdk.imodel import IModel
    from foundry_local_sdk.items import Item
    from foundry_local_sdk.request import Request
    from foundry_local_sdk.response import Response
    from foundry_local_sdk.session_types import (
        DecisionRequest,
        DecisionResult,
        RankingRequest,
        RankingResult,
        RequestOptions,
        RequestPreflightResult,
    )

# Stamped on every versioned struct this module builds. Must match the version requested from
# FoundryLocalGetApi (see _native/api.py): a tool definition carrying `kind` is only read as such
# by a runtime that supports version 2.
_API_VERSION = 2  # FOUNDRY_LOCAL_API_VERSION

# flToolKind values.
_TOOL_KIND_FUNCTION = 0
_TOOL_KIND_CUSTOM = 1

# Sentinel placed on the stream queue by the background thread when inference finishes.
_DONE = object()


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

    def __init__(self, session: "Session", request: "Request") -> None:
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        self._session = session
        self._request = request
        self._queue: queue.Queue = queue.Queue()
        # Worker-thread results — visible to the consumer only after _DONE drains the queue.
        self._final_response: "Response | None" = None
        self._error: BaseException | None = None
        # Iterator lifecycle.
        self._state: _State = _State.NEW
        # Idempotence latches: caller took the final Response; __exit__ ran; lock released.
        self._final_consumed = False
        self._closed = False
        self._lock_released = False
        self._thread: threading.Thread | None = None

        session._stream_queue = self._queue
        worker_started = threading.Event()

        def _run() -> None:
            try:
                with session._native_lifetime() as ptr:
                    worker_started.set()
                    out = ffi.new("flResponse**")
                    api.check_status(api.inference.Session_ProcessRequest(ptr, request._ptr, out))
                from foundry_local_sdk.response import Response

                # Response takes ownership of out[0]; wrapper releases it.
                self._final_response = Response(out[0])
            except Exception as exc:
                worker_started.set()
                self._error = exc
                self._queue.put(_StreamError(exc))
            finally:
                session._stream_queue = None
                self._queue.put(_DONE)

        t = threading.Thread(target=_run, daemon=True)
        session._stream_thread = t
        session._stream_request = request
        self._thread = t
        t.start()
        worker_started.wait()

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

    def _cancel_drain_and_join(self) -> None:
        # Cancellation is an idle no-op, so retry while the worker crosses native admission. Timed joins leave the
        # worker and callback threads free to make progress while ensuring early close cannot miss that boundary.
        while self._thread is not None and self._thread.is_alive():
            self._request.cancel()
            self._thread.join(timeout=0.01)

        # Drain any pending items so their native handles are released.
        while True:
            msg = self._queue.get()
            if msg is _DONE:
                break
            # _StreamError / Item: drop reference; Item handles release via __del__.
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
                    # Worker has already posted _DONE (or is about to). Drain it
                    # so the queue is empty and the worker is fully reaped.
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
                # Caller broke / errored out of the loop. Cancel the in-flight
                # request, drain the queue, join the worker, and release the lock.
                self._cancel_drain_and_join()
                self._state = _State.CANCELLED
                # Cancelled streams produce an undefined final Response — discard it.
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
            raise FoundryLocalException("final_response is not available until the stream has been fully consumed.")
        if self._error is not None:
            raise self._error
        if self._state is _State.CANCELLED:
            raise FoundryLocalException("Stream was cancelled.")
        if self._final_response is None:
            # Defensive — should not happen after a clean completion.
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
                # Iterator was never run, or abandoned without entering its finally.
                self._cancel_drain_and_join()
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
        # Safety net only — callers should use `with`.
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
        # Initialise lifecycle flags FIRST so that if anything below raises,
        # __del__ sees a fully-constructed (but already-closed) object and
        # cleanly no-ops instead of AttributeError'ing inside the GC.
        self._closed = True
        self._ptr = None
        self._manager = None
        self._operation_lock = threading.RLock()
        self._native_call_state = threading.local()
        self._stream_thread = None
        self._stream_request = None

        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api
        from foundry_local_sdk.imodel import _ModelImpl

        if not isinstance(model, _ModelImpl):
            raise TypeError("model must be a native IModel instance")

        manager = getattr(getattr(model, "_parent", None), "_parent", None)
        with model._manager_lifetime():
            out = ffi.new("flSession**")
            api.check_status(api.inference.Session_Create(model._ptr, out))
            try:
                self._ptr = out[0]
                self._manager = manager
                self._closed = False
                if manager is not None:
                    manager._register_session(self)
            except BaseException:
                self._ptr = None
                self._manager = None
                self._closed = True
                api.inference.Session_Release(out[0])
                raise

        # Streaming state — populated by set_streaming(True).
        self._streaming_enabled = False
        self._streaming_callback = None  # cffi callback object; held to prevent GC
        self._stream_queue: queue.Queue | None = None

        # Non-blocking gate used to detect (not serialize) concurrent streaming requests on the same session.
        # The native session has a single _stream_queue / callback path that cannot multiplex two in-flight streams;
        # the second caller must fail fast rather than silently cross-pollinate items into the first caller's iterator.
        self._streaming_in_flight = threading.Lock()

    def _check_open(self) -> None:
        from foundry_local_sdk.exception import FoundryLocalException

        if self._closed:
            raise FoundryLocalException(f"{type(self).__name__} has been closed and can no longer be used.")

    @contextmanager
    def _native_lifetime(self) -> Iterator[object]:
        with self._operation_lock:
            self._check_open()
            state = self._native_call_state
            state.depth = getattr(state, "depth", 0) + 1
            try:
                manager = self._manager
                if manager is None:
                    yield self._ptr
                    return
                with manager._native_call():
                    yield self._ptr
            finally:
                state.depth -= 1

    def set_options(self, options: "RequestOptions") -> "Session":
        """Set session-level inference options. Applies to all subsequent process_request calls."""
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        with self._native_lifetime() as ptr:
            native_options = options.to_native_options()
            kvp_out = ffi.new("flKeyValuePairs**")
            api.root.CreateKeyValuePairs(kvp_out)
            kvp = kvp_out[0]
            try:
                for key, value in native_options.items():
                    api.root.AddKeyValuePair(kvp, key.encode("utf-8"), value.encode("utf-8"))
                api.check_status(api.inference.Session_SetOptions(ptr, kvp))
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
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api
        from foundry_local_sdk.items import Item

        with self._native_lifetime() as ptr:
            if enabled and not self._streaming_enabled:
                # The object is stored on self so native never observes a collected callback.
                def _cb(data, user_data):
                    q = self._stream_queue
                    if q is None:
                        return 0

                    try:
                        if data.item_queue != ffi.NULL:
                            item_out = ffi.new("flItem**")
                            while api.item.ItemQueue_TryPop(data.item_queue, item_out):
                                # Ownership transferred to the Python Item wrapper.
                                q.put(Item.from_native(item_out[0], owns=True))
                    except Exception as exc:
                        q.put(_StreamError(exc))
                        return 1

                    return 0

                self._streaming_callback = ffi.callback("flStreamingCallback", _cb)
                self._streaming_enabled = True
                api.check_status(api.inference.Session_SetStreamingCallback(ptr, self._streaming_callback, ffi.NULL))

            elif not enabled and self._streaming_enabled:
                # Passing a NULL function pointer uninstalls the callback.
                api.check_status(
                    api.inference.Session_SetStreamingCallback(ptr, ffi.cast("flStreamingCallback", 0), ffi.NULL)
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
        self._check_open()
        from foundry_local_sdk.exception import FoundryLocalException

        if not self._streaming_enabled:
            raise FoundryLocalException(
                "Streaming not enabled. Call set_streaming(True) before process_streaming_request."
            )

        # Detect concurrent streaming on the same session — there is exactly one native callback / _stream_queue slot,
        # so a second caller would have its items interleaved into the first caller's iterator.
        # The lock is released by StreamingResponse's terminal cleanup (iterator drain, __exit__, or GC).
        if not self._streaming_in_flight.acquire(blocking=False):
            raise FoundryLocalException(
                "Concurrent streaming requests on the same session are not supported. "
                "Drain or cancel the in-flight stream before starting another."
            )

        try:
            return StreamingResponse(self, request)
        except BaseException:
            self._streaming_in_flight.release()
            raise

    def process_request(self, request: "Request") -> "Response":
        """Run the request synchronously and return the complete response."""
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api
        from foundry_local_sdk.response import Response

        with self._native_lifetime() as ptr:
            out = ffi.new("flResponse**")
            api.check_status(api.inference.Session_ProcessRequest(ptr, request._ptr, out))
            return Response(out[0])

    def _close(self) -> None:
        # Defensive: subclasses (ChatSession, AudioSession, EmbeddingsSession) validate
        # the model task BEFORE calling super().__init__(), so a validation failure leaves
        # a partially-constructed object that the GC will still try to finalise. Use
        # getattr so __del__ -> _close() no-ops cleanly instead of AttributeError'ing.
        if getattr(self, "_closed", True) or getattr(self, "_ptr", None) is None:
            return
        if getattr(getattr(self, "_native_call_state", None), "depth", 0) > 0:
            raise RuntimeError("Cannot close Session during an active native call")

        # If a streaming request is in flight, wind it down before Session_Release —
        # releasing while the worker is inside Session_ProcessRequest is a native
        # use-after-free.
        t = getattr(self, "_stream_thread", None)
        if t is not None and t.is_alive():
            req = getattr(self, "_stream_request", None)
            if req is not None:
                try:
                    req.cancel()
                except Exception:
                    pass
            # Session_Release and the manager lease must wait until native processing exits.
            t.join()

        with self._operation_lock:
            if self._closed or self._ptr is None:
                return
            ptr = self._ptr
            manager = self._manager
            try:
                if manager is None:
                    from foundry_local_sdk._native.api import api

                    api.inference.Session_Release(ptr)
                else:
                    manager._release_session(self, ptr)
            except Exception:
                pass
            finally:
                self._ptr = None
                self._closed = True
                self._manager = None

    def __enter__(self) -> "Session":
        return self

    def __exit__(self, *_) -> None:
        self._close()

    def __del__(self) -> None:
        try:
            self._close()
        except Exception:
            pass


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
                f"ChatSession requires a model with task 'chat-completion' or 'vision-language-chat', but got {task!r}."
            )
        super().__init__(model)

    def preflight_request(self, request: "Request") -> "RequestPreflightResult":
        """Synchronously return the native token budget for a request."""

        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api
        from foundry_local_sdk.session_types import RequestPreflightResult

        out_preflight = ffi.new("flRequestPreflight**")
        with self._native_lifetime() as session_ptr:
            with request._native_lifetime() as request_ptr:
                api.check_status(api.inference.Session_CreateRequestPreflight(session_ptr, request_ptr, out_preflight))
            preflight = out_preflight[0]
            try:
                result = ffi.new("flRequestPreflightResult*")
                result.version = _API_VERSION
                api.check_status(api.inference.RequestPreflight_Execute(preflight, result))
                return RequestPreflightResult(
                    prompt_tokens=int(result.prompt_tokens),
                    output_reserve_tokens=int(result.output_reserve_tokens),
                    required_tokens=int(result.required_tokens),
                    context_limit_tokens=int(result.context_limit_tokens),
                    fits=bool(result.fits),
                    deficit_tokens=int(result.deficit_tokens),
                )
            finally:
                api.inference.RequestPreflight_Release(preflight)

    def add_tool_definition(self, name: str, description: str, json_schema: str) -> "ChatSession":
        """Register a function tool so the model can request tool calls. Returns self (fluent).

        ``json_schema`` is required and must be valid JSON. Names are case-sensitive and must be
        unique within the session across kinds. All string arguments must not contain embedded NUL
        characters.
        """
        return self._add_tool_definition(name, description, json_schema, _TOOL_KIND_FUNCTION)

    def add_custom_tool_definition(self, name: str, description: str) -> "ChatSession":
        """Register a custom tool: one whose arguments are a single free-form text payload.

        The schema the model is prompted with is synthesized natively, so none is supplied here, and
        the ``arguments`` of a generated tool call carry the raw text the model produced rather than
        a JSON object. The name and description must not contain embedded NUL characters. Returns
        self (fluent).
        """
        return self._add_tool_definition(name, description, "", _TOOL_KIND_CUSTOM)

    def _add_tool_definition(self, name: str, description: str, json_schema: str, kind: int) -> "ChatSession":
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        self._validate_native_string(name, "name")
        self._validate_native_string(description, "description")
        self._validate_native_string(json_schema, "json_schema")

        # Keep cffi temporaries as named locals so they outlive the native call.
        c_name = ffi.new("char[]", name.encode("utf-8") + b"\x00")
        c_desc = ffi.new("char[]", description.encode("utf-8") + b"\x00")
        c_schema = ffi.new("char[]", json_schema.encode("utf-8") + b"\x00")

        tool_def = ffi.new("flToolDefinition*")
        tool_def.version = _API_VERSION
        tool_def.name = c_name
        tool_def.description = c_desc
        tool_def.json_schema = c_schema
        tool_def.kind = kind

        with self._native_lifetime() as ptr:
            api.check_status(api.inference.Session_AddToolDefinition(ptr, tool_def))
        return self

    def remove_tool_definition(self, name: str) -> bool:
        """Remove a previously-added tool definition by name.

        Returns True if a matching tool was found and removed, False if no tool with that
        name was registered. Useful when the available tool set changes mid-conversation. The name
        must not contain embedded NUL characters.
        """
        from foundry_local_sdk._native import ffi
        from foundry_local_sdk._native.api import api

        self._validate_native_string(name, "name")
        c_name = ffi.new("char[]", name.encode("utf-8") + b"\x00")
        out_removed = ffi.new("bool*")
        with self._native_lifetime() as ptr:
            api.check_status(api.inference.Session_RemoveToolDefinition(ptr, c_name, out_removed))
            return bool(out_removed[0])

    @staticmethod
    def _validate_native_string(value: str, argument_name: str) -> None:
        if not isinstance(value, str):
            raise TypeError(f"{argument_name} must be a string")
        if "\x00" in value:
            raise ValueError(f"{argument_name} must not contain an embedded NUL character")

    @property
    def turn_count(self) -> int:
        """Number of completed turns accumulated in this session."""
        from foundry_local_sdk._native.api import api

        with self._native_lifetime() as ptr:
            return int(api.inference.Session_GetTurnCount(ptr))

    def undo_turns(self, count: int) -> None:
        """Remove the last `count` turns from session history."""
        from foundry_local_sdk._native.api import api

        with self._native_lifetime() as ptr:
            api.check_status(api.inference.Session_UndoTurns(ptr, count))


class AudioSession(Session):
    """Inference session for automatic-speech-recognition models.

    Accepts ``AudioItem`` input and produces ``TextItem`` output.
    Validates the model task at construction time.
    """

    def __init__(self, model: "IModel") -> None:
        task = model.info.task
        if task != "automatic-speech-recognition":
            raise ValueError(
                f"AudioSession requires a model with task 'automatic-speech-recognition', but got {task!r}."
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
            raise ValueError(f"EmbeddingsSession requires a model with task 'embeddings', but got {task!r}.")
        super().__init__(model)


def _validate_json_value(value: object, name: str) -> None:
    if value is None or isinstance(value, (bool, str)):
        return
    if isinstance(value, int):
        if value < -(2**63) or value > 2**63 - 1:
            raise ValueError(f"{name} integers must fit in signed 64 bits")
        return
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ValueError(f"{name} numbers must be finite")
        return
    if isinstance(value, list):
        for index, item in enumerate(value):
            _validate_json_value(item, f"{name}[{index}]")
        return
    if isinstance(value, dict):
        for key, item in value.items():
            if not isinstance(key, str):
                raise TypeError(f"{name} object keys must be strings")
            _validate_json_value(item, f"{name}.{key}")
        return
    raise TypeError(f"{name} must contain only JSON-compatible values")


def _validate_temperature(value: object) -> None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise TypeError("temperature must be a number")
    if value <= 0 or value > 100 or not math.isfinite(value):
        raise ValueError("temperature must be in (0, 100]")


def _validate_model(value: object) -> None:
    if not isinstance(value, str):
        raise TypeError("model must be a string")
    if not value:
        raise ValueError("model must not be empty")


def _is_finite_number(value: object) -> bool:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return False
    try:
        return math.isfinite(value)
    except OverflowError:
        return False


def _process_non_generative_json(session: Session, payload: dict[str, object]) -> dict[str, object]:
    from foundry_local_sdk.exception import FoundryLocalException
    from foundry_local_sdk.items import TextItem, TextItemType
    from foundry_local_sdk.request import Request

    content = json.dumps(payload, separators=(",", ":"), allow_nan=False)
    with Request().add_item(TextItem(content, TextItemType.OPENAI_JSON)) as request:
        with session.process_request(request) as response:
            if response.item_count != 1:
                raise FoundryLocalException("Non-generative session returned an invalid response item count.")
            item = response.get_item(0)
            if not isinstance(item, TextItem) or item.type is not TextItemType.OPENAI_JSON:
                raise FoundryLocalException("Non-generative session returned a non-JSON text response.")
            response_text = item.text

    try:
        parsed = json.loads(response_text)
    except (TypeError, json.JSONDecodeError) as exc:
        raise FoundryLocalException("Non-generative session returned invalid JSON.") from exc
    if not isinstance(parsed, dict):
        raise FoundryLocalException("Non-generative session returned a JSON value that is not an object.")
    return parsed


class RankingSession(Session):
    """Typed wrapper for a native ``text-ranking`` model session."""

    def __init__(self, model: "IModel") -> None:
        task = model.info.task
        if task != "text-ranking":
            raise ValueError(f"RankingSession requires a model with task 'text-ranking', but got {task!r}.")
        super().__init__(model)

    def rank(self, request: "RankingRequest") -> "RankingResult":
        """Rank candidates using the same contract as ``POST /v1/rank``."""
        from foundry_local_sdk.session_types import RankedCandidate, RankingRequest, RankingResult

        if not isinstance(request, RankingRequest):
            raise TypeError("request must be a RankingRequest")
        if not request.answers or any(not isinstance(answer, str) or not answer for answer in request.answers):
            raise ValueError("answers must be a non-empty list of non-empty strings")
        if not isinstance(request.question, str):
            raise TypeError("question must be a string")
        _validate_json_value(request.context, "context")
        _validate_model(request.model)
        _validate_temperature(request.temperature)

        result = _process_non_generative_json(
            self,
            {
                "model": request.model,
                "context": request.context,
                "question": request.question,
                "answers": request.answers,
                "temperature": request.temperature,
            },
        )
        model = result.get("model")
        ranked = result.get("ranked")
        if not isinstance(model, str) or not isinstance(ranked, list):
            raise _invalid_non_generative_response()
        values: list[RankedCandidate] = []
        for item in ranked:
            if not isinstance(item, dict):
                raise _invalid_non_generative_response()
            rank, candidate, probability = (
                item.get("rank"),
                item.get("candidate"),
                item.get("prob"),
            )
            if (
                isinstance(rank, bool)
                or not isinstance(rank, int)
                or not isinstance(candidate, str)
                or not _is_finite_number(probability)
            ):
                raise _invalid_non_generative_response()
            values.append(RankedCandidate(rank, candidate, float(probability)))
        return RankingResult(model, values)


class DecisionSession(Session):
    """Typed wrapper for a native ``typed-decision`` model session."""

    def __init__(self, model: "IModel") -> None:
        task = model.info.task
        if task != "typed-decision":
            raise ValueError(f"DecisionSession requires a model with task 'typed-decision', but got {task!r}.")
        super().__init__(model)

    def decide(self, request: "DecisionRequest") -> "DecisionResult":
        """Answer typed questions using the same contract as ``POST /v1/systemone``."""
        from typing import cast

        from foundry_local_sdk.session_types import (
            DecisionAnswer,
            DecisionRequest,
            DecisionResult,
            DecisionUsage,
        )

        if not isinstance(request, DecisionRequest):
            raise TypeError("request must be a DecisionRequest")
        if not isinstance(request.questions, dict) or not request.questions:
            raise ValueError("questions must be a non-empty dictionary")
        for question_id, question in request.questions.items():
            if not isinstance(question_id, str) or not question_id:
                raise ValueError("question names must be non-empty strings")
            if not isinstance(question, dict):
                raise TypeError(f"questions.{question_id} must be a dictionary")
            question_type = question.get("type")
            if question_type not in {"noul", "choice", "score"}:
                raise ValueError(f"questions.{question_id}.type must be 'noul', 'choice', or 'score'")
            for field in ("criteria", "instructions"):
                if field in question:
                    _validate_json_value(question[field], f"questions.{question_id}.{field}")
        _validate_json_value(request.state, "state")
        _validate_model(request.model)
        _validate_temperature(request.temperature)

        result = _process_non_generative_json(
            self,
            {
                "model": request.model,
                "state": request.state,
                "questions": request.questions,
                "temperature": request.temperature,
            },
        )
        model, answers, usage = result.get("model"), result.get("answers"), result.get("usage")
        if (
            not isinstance(model, str)
            or not isinstance(answers, dict)
            or not isinstance(usage, dict)
            or isinstance(usage.get("billing_units"), bool)
            or not isinstance(usage.get("billing_units"), int)
        ):
            raise _invalid_non_generative_response()
        for question_id, answer in answers.items():
            if (
                not isinstance(question_id, str)
                or not isinstance(answer, dict)
                or not isinstance(answer.get("type"), str)
            ):
                raise _invalid_non_generative_response()
            try:
                _validate_json_value(answer, f"answers.{question_id}")
            except (TypeError, ValueError) as exc:
                raise _invalid_non_generative_response() from exc
        return DecisionResult(
            model,
            cast(dict[str, DecisionAnswer], answers),
            cast(DecisionUsage, usage),
        )


def _invalid_non_generative_response() -> Exception:
    from foundry_local_sdk.exception import FoundryLocalException

    return FoundryLocalException("Non-generative session returned an invalid response.")
