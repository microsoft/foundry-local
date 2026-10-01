# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Focused native-fake tests for ranking and typed-decision sessions."""

from __future__ import annotations

import json
import sys
import threading
from types import SimpleNamespace

import pytest

from foundry_local_sdk import (
    DecisionRequest,
    DecisionSession,
    FoundryLocalException,
    RankedCandidate,
    RankingRequest,
    RankingSession,
    TextItemType,
)

_SESSION = object()
_REQUEST = object()
_INPUT = object()
_RESPONSE = object()
_OUTPUT = object()


class _FakeFfi:
    NULL = None

    @staticmethod
    def new(cdecl, initializer=None):
        if cdecl.endswith("**"):
            return [None]
        if cdecl == "char[]":
            return initializer
        return SimpleNamespace(version=0, text=None, type=0)

    @staticmethod
    def string(value):
        return bytes(value).split(b"\x00", 1)[0]


ffi = _FakeFfi()


def _fake_session(cls):
    session = cls.__new__(cls)
    session._closed = False
    session._ptr = _SESSION
    session._manager = None
    session._operation_lock = threading.RLock()
    session._native_call_state = threading.local()
    session._stream_thread = None
    session._stream_request = None
    return session


def _install_native(monkeypatch: pytest.MonkeyPatch, response_json: object):
    calls: list[object] = []
    strings: list[object] = []

    def create_item(item_type, out):
        assert int(item_type) == 20
        out[0] = _INPUT
        calls.append("create-item")
        return ffi.NULL

    def set_text(ptr, data):
        assert ptr == _INPUT
        calls.append(("input", ffi.string(data.text).decode(), int(data.type)))
        return ffi.NULL

    def create_request(out):
        out[0] = _REQUEST
        calls.append("create-request")
        return ffi.NULL

    def add_item(request, item, transfer):
        assert (request, item, bool(transfer)) == (_REQUEST, _INPUT, True)
        calls.append("add-item")
        return ffi.NULL

    def process(session, request, out):
        assert (session, request) == (_SESSION, _REQUEST)
        out[0] = _RESPONSE
        calls.append("process")
        return ffi.NULL

    def get_item(_response, _index, out):
        out[0] = _OUTPUT
        return ffi.NULL

    def get_text(ptr, data):
        assert ptr == _OUTPUT
        encoded = json.dumps(response_json).encode()
        strings.append(ffi.new("char[]", encoded + b"\x00"))
        data.text = strings[-1]
        data.type = int(TextItemType.OPENAI_JSON)
        return ffi.NULL

    fake_api = SimpleNamespace(
        item=SimpleNamespace(
            Create=create_item,
            SetText=set_text,
            GetType=lambda ptr: 20 if ptr == _OUTPUT else 0,
            GetText=get_text,
            Item_Release=lambda ptr: calls.append(("item-release", ptr)),
        ),
        inference=SimpleNamespace(
            Request_Create=create_request,
            Request_AddItem=add_item,
            Request_Release=lambda ptr: calls.append(("request-release", ptr)),
            Session_ProcessRequest=process,
            Response_GetItemCount=lambda _ptr: 1,
            Response_GetItem=get_item,
            Response_Release=lambda ptr: calls.append(("response-release", ptr)),
            Session_Release=lambda ptr: calls.append(("session-release", ptr)),
        ),
        check_status=lambda status: assert_null(status),
    )
    monkeypatch.setitem(sys.modules, "foundry_local_sdk._native", SimpleNamespace(ffi=ffi))
    monkeypatch.setitem(sys.modules, "foundry_local_sdk._native.api", SimpleNamespace(api=fake_api))
    return calls


def test_ranking_session_uses_endpoint_json_contract_and_releases_handles(monkeypatch):
    calls = _install_native(
        monkeypatch,
        {
            "model": "clm-package:1",
            "ranked": [{"rank": 1, "candidate": "inside", "prob": 0.75}],
        },
    )
    session = _fake_session(RankingSession)

    result = session.rank(
        RankingRequest(
            context={"weather": "rain"},
            question="Where?",
            answers=["outside", "inside"],
            model="clm",
            temperature=2,
        )
    )
    payload = next(call for call in calls if isinstance(call, tuple) and call[0] == "input")

    assert json.loads(payload[1]) == {
        "model": "clm",
        "context": {"weather": "rain"},
        "question": "Where?",
        "answers": ["outside", "inside"],
        "temperature": 2,
    }
    assert payload[2] == TextItemType.OPENAI_JSON
    assert result.model == "clm-package:1"
    assert result.ranked == [RankedCandidate(rank=1, candidate="inside", prob=0.75)]
    assert ("request-release", _REQUEST) in calls
    assert ("response-release", _RESPONSE) in calls


def test_decision_session_parses_typed_response_and_closes_once(monkeypatch):
    calls = _install_native(
        monkeypatch,
        {
            "model": "kev-package:1",
            "answers": {"umbrella": {"type": "noul", "noul": 0.9}},
            "usage": {"billing_units": 0},
        },
    )
    session = _fake_session(DecisionSession)

    result = session.decide(
        DecisionRequest(
            state={"weather": "rain"},
            questions={"umbrella": {"type": "noul", "instructions": "Take one?"}},
        )
    )
    session._close()
    session._close()

    assert result.answers["umbrella"]["noul"] == 0.9
    assert result.usage == {"billing_units": 0}
    assert calls.count(("session-release", _SESSION)) == 1
    with pytest.raises(FoundryLocalException, match="closed"):
        session.decide(DecisionRequest(questions={"q": {"type": "noul"}}))


@pytest.mark.parametrize(
    ("session_type", "task", "required_task"),
    [
        (RankingSession, "typed-decision", "text-ranking"),
        (DecisionSession, "text-ranking", "typed-decision"),
    ],
)
def test_typed_session_rejects_wrong_model_task_before_native(session_type, task, required_task):
    model = SimpleNamespace(info=SimpleNamespace(task=task))
    with pytest.raises(ValueError, match=required_task):
        session_type(model)


def test_invalid_response_shape_still_releases_request_and_response(monkeypatch):
    calls = _install_native(monkeypatch, ["not", "an", "object"])

    with pytest.raises(FoundryLocalException, match="not an object"):
        _fake_session(RankingSession).rank(RankingRequest(answers=["candidate"]))

    assert ("request-release", _REQUEST) in calls
    assert ("response-release", _RESPONSE) in calls


@pytest.mark.parametrize(
    ("session_type", "request_value", "message"),
    [
        (RankingSession, RankingRequest(answers=[]), "answers"),
        (
            DecisionSession,
            DecisionRequest(questions={"q": {"type": "unknown"}}),  # type: ignore[typeddict-item]
            "type",
        ),
        (
            DecisionSession,
            DecisionRequest(questions={"q": {"type": "noul"}}, temperature=float("nan")),
            "temperature",
        ),
    ],
)
def test_non_generative_request_validation_happens_before_native(session_type, request_value, message):
    with pytest.raises(ValueError, match=message):
        (
            _fake_session(session_type).rank(request_value)
            if session_type is RankingSession
            else _fake_session(session_type).decide(request_value)
        )


def assert_null(status) -> None:
    assert status == ffi.NULL
