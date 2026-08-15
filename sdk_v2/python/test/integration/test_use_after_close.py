# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Regression tests for the ``_check_open()`` guards on Request / Response / Session.

After ``_close()`` the native handle has been released and ``self._ptr`` is None. Calling any public method that
passes ``self._ptr`` to the native layer would at best assert in cffi and at worst trigger UB. These tests
verify each class raises ``FoundryLocalException`` with a clear message instead.
"""
from __future__ import annotations

import pytest

from foundry_local_sdk import (
    ChatSession,
    MessageItem,
    Request,
    RequestOptions,
    SearchOptions,
)
from foundry_local_sdk.exception import FoundryLocalException


class TestRequestUseAfterClose:
    def test_methods_raise_after_close(self):
        req = Request()
        req._close()
        with pytest.raises(FoundryLocalException, match="closed"):
            _ = req.item_count
        with pytest.raises(FoundryLocalException, match="closed"):
            req.cancel()
        with pytest.raises(FoundryLocalException, match="closed"):
            req.set_options(RequestOptions(search=SearchOptions(temperature=0)))


class TestSessionUseAfterClose:
    def test_methods_raise_after_close(self, chat_model):
        s = ChatSession(chat_model)
        s._close()
        with pytest.raises(FoundryLocalException, match="closed"):
            s.set_streaming(True)
        with pytest.raises(FoundryLocalException, match="closed"):
            s.process_request(Request())
        with pytest.raises(FoundryLocalException, match="closed"):
            s.add_tool_definition("tool", "description", "{}")
        with pytest.raises(FoundryLocalException, match="closed"):
            s.remove_tool_definition("tool")
        with pytest.raises(FoundryLocalException, match="closed"):
            _ = s.turn_count
        with pytest.raises(FoundryLocalException, match="closed"):
            s.undo_turns(1)


class TestResponseUseAfterClose:
    def test_methods_raise_after_close(self, chat_model):
        with ChatSession(chat_model) as s:
            s.set_options(RequestOptions(search=SearchOptions(temperature=0, max_output_tokens=4)))
            resp = s.process_request(Request().add_item(MessageItem.user("hi")))
            resp._close()
            with pytest.raises(FoundryLocalException, match="closed"):
                _ = resp.item_count
            with pytest.raises(FoundryLocalException, match="closed"):
                resp.get_item(0)
