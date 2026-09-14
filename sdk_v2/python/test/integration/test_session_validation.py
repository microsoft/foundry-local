# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Session task-validation tests.

These do not run inference — they assert that the Python ``Session``
subclasses reject mismatched models at construction time. Each test
needs a model of the *wrong* task to prove the rejection fires.
"""
from __future__ import annotations

import pytest

from foundry_local_sdk import AudioSession, ChatSession, EmbeddingsSession


def _find_first_model_for_task(manager, task: str):
    for model in manager.catalog.list_models():
        if model.info.task == task:
            return model
        try:
            for v in model.variants:
                if v.info.task == task:
                    model.select_variant(v)
                    return model
        except Exception:
            pass
    return None


class TestChatSessionValidation:
    def test_rejects_embeddings_model(self, manager):
        m = _find_first_model_for_task(manager, "embeddings")
        if m is None:
            pytest.fail("No embeddings model in catalog to use as wrong-task input.")
        with pytest.raises(ValueError, match="ChatSession requires"):
            ChatSession(m)


class TestAudioSessionValidation:
    def test_rejects_chat_model(self, manager):
        m = _find_first_model_for_task(manager, "chat-completion")
        if m is None:
            pytest.fail("No chat-completion model in catalog.")
        with pytest.raises(ValueError, match="AudioSession requires"):
            AudioSession(m)


class TestEmbeddingsSessionValidation:
    def test_rejects_chat_model(self, manager):
        m = _find_first_model_for_task(manager, "chat-completion")
        if m is None:
            pytest.fail("No chat-completion model in catalog.")
        with pytest.raises(ValueError, match="EmbeddingsSession requires"):
            EmbeddingsSession(m)


class TestSessionRequiresNativeModel:
    def test_python_object_rejected(self):
        class Bogus:
            pass

        with pytest.raises(TypeError):
            # Session is abstract via ABC, so go through ChatSession which
            # would otherwise hit task validation first — but we want to
            # confirm the type guard is in the base. ChatSession reads
            # model.info.task before super().__init__, so use the same trick:
            # construct via __new__ + manual call to base __init__.
            s = ChatSession.__new__(ChatSession)
            from foundry_local_sdk.session import Session as Base
            Base.__init__(s, Bogus())
