# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Unit tests for the deprecated live audio transcription adapter."""
from __future__ import annotations

import threading
from unittest.mock import MagicMock

import pytest

from foundry_local_sdk import item_queue, items, request, session
from foundry_local_sdk.openai.live_audio_session import LiveAudioTranscriptionSession


def test_start_propagates_snapshotted_language_to_request_options(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    native_request = MagicMock()
    monkeypatch.setattr(request, "Request", MagicMock(return_value=native_request))
    monkeypatch.setattr(item_queue, "ItemQueue", MagicMock(return_value=MagicMock()))
    monkeypatch.setattr(
        items.AudioItem,
        "create_format_descriptor",
        MagicMock(return_value=MagicMock()),
    )
    monkeypatch.setattr(session, "AudioSession", MagicMock(return_value=MagicMock()))
    monkeypatch.setattr(threading, "Thread", MagicMock(return_value=MagicMock()))

    with pytest.warns(DeprecationWarning):
        live_session = LiveAudioTranscriptionSession("model", MagicMock())
    live_session.settings.language = "en-US"

    live_session.start()
    live_session.settings.language = "fr"

    options = native_request.set_options.call_args.args[0]
    assert options.additional_options == {"language": "en-US"}
    live_session.close()
