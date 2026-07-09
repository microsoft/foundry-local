# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Smoke tests asserting the public import surface stays intact."""
from __future__ import annotations

import importlib

import pytest


PUBLIC_NAMES = [
    "FoundryLocalException",
    "__version__",
    "LogLevel",
    "Configuration",
    "FoundryLocalManager",
    "Catalog",
    "IModel",
    "Model",
    "ModelInfo",
    "PromptTemplate",
    "Runtime",
    "Parameter",
    "ModelSettings",
    "DeviceType",
    "EpInfo",
    "EpDownloadResult",
    "Item",
    "TextItem",
    "MessageItem",
    "BytesItem",
    "ImageItem",
    "AudioItem",
    "SpeechSegmentItem",
    "SpeechResultItem",
    "SpeechWord",
    "ToolCallItem",
    "ToolResultItem",
    "TensorItem",
    "ItemType",
    "TextItemType",
    "SpeechSegmentKind",
    "MessageRole",
    "TensorDataType",
    "FinishReason",
    "TokenUsage",
    "SearchOptions",
    "RequestOptions",
    "ToolChoice",
    "Request",
    "Response",
    "Session",
    "ChatSession",
    "AudioSession",
    "EmbeddingsSession",
]


def test_public_symbols_importable():
    mod = importlib.import_module("foundry_local_sdk")
    missing = [name for name in PUBLIC_NAMES if not hasattr(mod, name)]
    assert not missing, f"Missing public symbols: {missing}"


def test_all_matches_documented_surface():
    mod = importlib.import_module("foundry_local_sdk")
    assert set(mod.__all__) >= set(PUBLIC_NAMES)


def test_model_is_alias_for_imodel():
    from foundry_local_sdk import IModel, Model
    assert Model is IModel


@pytest.mark.parametrize(
    "submodule",
    [
        "foundry_local_sdk.configuration",
        "foundry_local_sdk.catalog",
        "foundry_local_sdk.imodel",
        "foundry_local_sdk.items",
        "foundry_local_sdk.request",
        "foundry_local_sdk.response",
        "foundry_local_sdk.session",
        "foundry_local_sdk.session_types",
        "foundry_local_sdk.foundry_local_manager",
    ],
)
def test_submodule_importable(submodule):
    importlib.import_module(submodule)


class TestOpenAiImportGuard:
    """The openai subpackage must check for the openai dependency exactly
    once at package-import time — not in each individual client file."""

    def test_openai_clients_importable_when_openai_installed(self):
        # In the dev environment openai is installed; the import must succeed
        # and produce the three public clients.
        pytest.importorskip("openai")
        from foundry_local_sdk.openai.chat_client import ChatClient
        from foundry_local_sdk.openai.audio_client import AudioClient
        from foundry_local_sdk.openai.embedding_client import EmbeddingClient
        assert ChatClient is not None
        assert AudioClient is not None
        assert EmbeddingClient is not None

    def test_individual_clients_do_not_have_their_own_guard(self):
        # If a client file still has its own try/except ImportError block,
        # the consolidation regressed. Read the source files directly so this
        # test runs even when the openai package is not installed.
        from pathlib import Path

        pkg_dir = Path(__file__).resolve().parents[2] / "src" / "foundry_local_sdk" / "openai"
        for filename in ("chat_client.py", "embedding_client.py", "audio_client.py"):
            src = (pkg_dir / filename).read_text(encoding="utf-8")
            assert "except ImportError" not in src, (
                f"{filename} still has its own ImportError guard — "
                f"the package-level guard in foundry_local_sdk.openai.__init__ "
                f"should be the single source of truth."
            )
