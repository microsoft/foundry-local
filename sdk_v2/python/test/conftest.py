# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Shared fixtures for the Foundry Local Python SDK test suite.

CI policy mirrors the C++ integration tests (see
``sdk_v2/cpp/test/sdk_api/shared_test_env.h``):

- ``IS_CI`` is true when ``TF_BUILD=true`` (Azure DevOps) or
  ``GITHUB_ACTIONS=true`` (GitHub Actions), case-insensitive.
- Tests that require a model **never download** in CI. They use a model
  only when it is already present in the local model cache. When no
  suitable model is cached, the test is skipped with a clear reason.
- Locally the same rule applies: pre-cache models with the Foundry
  Local CLI before running model-dependent tests.

The ``Manager`` is a singleton — it is created exactly once per test
process by the session-scoped ``manager`` fixture.
"""

from __future__ import annotations

import os

import pytest


# ---------------------------------------------------------------------------
# CI detection
# ---------------------------------------------------------------------------

def is_running_in_ci() -> bool:
    azure_devops = os.environ.get("TF_BUILD", "false").lower() == "true"
    github_actions = os.environ.get("GITHUB_ACTIONS", "false").lower() == "true"
    return azure_devops or github_actions


IS_CI: bool = is_running_in_ci()

# Required model cache path for sdk_v2 Python tests.
FOUNDRY_TEST_DATA_DIR: str | None = os.environ.get("FOUNDRY_TEST_DATA_DIR") or None


# ---------------------------------------------------------------------------
# Session-scoped manager — singleton, created exactly once per test process
# ---------------------------------------------------------------------------

@pytest.fixture(scope="session")
def manager():
    """Initialize the FoundryLocalManager singleton for the test session.

    A working manager is the most basic prerequisite for the entire integration
    suite — without it nothing the SDK does is meaningful. Any failure here
    (package not importable, native library can't be dlopened, initialize
    raises) is therefore a hard error, not a skip. Converting it to
    ``pytest.skip`` would silently turn a broken build into a green CI run by
    cascading the skip through every test that depends on this fixture.
    """
    from foundry_local_sdk import Configuration, FoundryLocalManager, LogLevel

    # Track whether this fixture is responsible for closing the singleton.
    # If another fixture already initialised it, we are just borrowing it
    # and must not close it on teardown.
    created_here = False

    if not FOUNDRY_TEST_DATA_DIR:
        pytest.skip("FOUNDRY_TEST_DATA_DIR is required for sdk_v2 Python tests.")

    if not os.path.isdir(FOUNDRY_TEST_DATA_DIR):
        pytest.skip(f"FOUNDRY_TEST_DATA_DIR does not exist: {FOUNDRY_TEST_DATA_DIR}")

    if FoundryLocalManager.instance is None:
        config_kwargs = {
            "app_name": "FoundryLocalPythonTests",
            "log_level": LogLevel.WARNING,
            "model_cache_dir": FOUNDRY_TEST_DATA_DIR,
        }

        config = Configuration(**config_kwargs)
        FoundryLocalManager.initialize(config)
        created_here = True

    mgr = FoundryLocalManager.instance
    try:
        yield mgr
    finally:
        # Only close if we created the singleton AND it has not been replaced
        # or torn down by another test (the zz_* tests intentionally call
        # shutdown()/close() and run last alphabetically, leaving the
        # singleton field as None or pointing at a different instance).
        if created_here and FoundryLocalManager.instance is mgr:
            try:
                mgr.close()
            except Exception as e:
                # Teardown must not break unrelated tests, but a silent swallow
                # hides real native shutdown bugs. Surface as a warning so it
                # appears in pytest's warnings summary without failing the run.
                import warnings
                warnings.warn(
                    f"FoundryLocalManager teardown raised: {e!r}",
                    RuntimeWarning,
                    stacklevel=1,
                )


# ---------------------------------------------------------------------------
# Model discovery helpers
# ---------------------------------------------------------------------------

def _find_smallest_cached_model_for_task(manager, task: str, *, name_substr: str | None = None):
    """Return the smallest cached model variant whose task matches.

    Walks each alias and its variants — selecting a CPU variant when the default selection has the wrong task
    — to mirror the C++ helper ``FindSmallestModelByTask``. When ``name_substr`` is supplied, additionally
    requires the model id or alias to contain that substring (case-insensitive); used to constrain ASR
    selection to whisper-family models since that is the only ASR runtime the native side currently implements.
    """
    from foundry_local_sdk.exception import FoundryLocalException

    needle = name_substr.lower() if name_substr else None

    def _name_matches(m) -> bool:
        if needle is None:
            return True
        candidates = (getattr(m, "alias", "") or "", getattr(m.info, "id", "") or "")
        return any(needle in c.lower() for c in candidates)

    best = None
    best_size = None
    for model in manager.catalog.list_models():
        info = model.info
        if info.task != task:
            # Try variants for a matching task. Only swallow SDK errors —
            # let unrelated Python errors surface.
            try:
                variants = model.variants
            except FoundryLocalException:
                variants = []
            matched = None
            for v in variants:
                if v.info.task == task:
                    matched = v
                    break
            if matched is None:
                continue
            try:
                model.select_variant(matched)
            except FoundryLocalException:
                continue
            info = model.info
        if not model.is_cached:
            continue
        if not _name_matches(model):
            continue
        size = info.file_size_mb or 0
        if size <= 0:
            continue
        if best_size is None or size < best_size:
            best = model
            best_size = size
    return best


def _model_fixture_or_skip(manager, task: str, role: str, *, load: bool, name_substr: str | None = None):
    from foundry_local_sdk.exception import FoundryLocalException

    model = _find_smallest_cached_model_for_task(manager, task, name_substr=name_substr)
    if model is None:
        extra = f" with name containing {name_substr!r}" if name_substr else ""
        reason = (
            f"No cached {role} model (task={task!r}){extra} available. "
            f"In CI: pre-stage one in FOUNDRY_TEST_DATA_DIR. "
            f"Locally: run 'foundry model download <alias>' first."
        )
        pytest.skip(reason)
    if load:
        try:
            model.load()
        except FoundryLocalException as e:
            pytest.skip(f"Could not load {role} model {model.alias!r}: {e}")
    return model


# Pinned chat model for deterministic streaming/content assertions. Matches the
# JS/C#/C++ integration suites (qwen2.5-0.5b-instruct, generic-cpu variant 4) so
# cross-language tests exercise the same weights. Auto-selecting "smallest cached
# chat model" can pick a reasoning/thinking model whose output is unsuitable for
# substring-based content assertions.
_PINNED_CHAT_MODEL_ID = "qwen2.5-0.5b-instruct-generic-cpu:4"


@pytest.fixture(scope="session")
def chat_model(manager):
    """Pinned cached chat-completion model, loaded. Falls back to smallest cached
    chat model when the pinned variant is not present. Skips if none cached.
    """
    from foundry_local_sdk.exception import FoundryLocalException

    pinned = manager.catalog.get_model_variant(_PINNED_CHAT_MODEL_ID)
    if pinned is not None and pinned.is_cached:
        try:
            pinned.load()
            return pinned
        except FoundryLocalException as e:
            pytest.skip(f"Could not load pinned chat model {_PINNED_CHAT_MODEL_ID!r}: {e}")

    return _model_fixture_or_skip(manager, "chat-completion", "chat", load=True)


@pytest.fixture(scope="session")
def embedding_model(manager):
    """Smallest cached embeddings model, loaded. Skips if none cached."""
    return _model_fixture_or_skip(manager, "embeddings", "embedding", load=True)


@pytest.fixture(scope="session")
def audio_model(manager):
    """Smallest cached ASR model, loaded. Skips if none cached.

    Used by tests that exercise the streaming live-audio path, which works with any
    ``automatic-speech-recognition`` model the native side can load (e.g. nemotron-style). Tests that hit the
    one-shot file transcription path should depend on :func:`whisper_audio_model` instead — today only the
    whisper decoder implements that path.
    """
    return _model_fixture_or_skip(manager, "automatic-speech-recognition", "audio", load=True)


@pytest.fixture(scope="session")
def whisper_audio_model(manager):
    """Smallest cached whisper-family ASR model, loaded. Skips if none cached.

    The one-shot ``AudioClient.transcribe`` path goes through ``onnx_audio_generator``, which only implements
    whisper-style decoders today — a non-whisper ASR model loads but fails at inference time with
    ``model does not support audio processing``. Constrain selection to models whose id/alias contains
    ``whisper``.
    """
    return _model_fixture_or_skip(
        manager, "automatic-speech-recognition", "whisper-audio", load=True, name_substr="whisper"
    )


@pytest.fixture(scope="session")
def streaming_audio_model(manager):
    """Smallest cached nemotron-family ASR model, loaded. Skips if none cached.

    The live-streaming PCM path (``AudioItem`` format-descriptor + ``ItemQueue``) requires a streaming-capable
    decoder. Whisper-style models load but fail when fed unbounded streamed PCM, so constrain selection to
    models whose id/alias contains ``nemotron``.
    """
    return _model_fixture_or_skip(
        manager, "automatic-speech-recognition", "streaming-audio", load=True, name_substr="nemotron"
    )


# ---------------------------------------------------------------------------
# Native-layer availability — used to skip integration tests if the .pyd
# was not built or is incompatible with the current Python.
# ---------------------------------------------------------------------------

@pytest.fixture(scope="session")
def native_api():
    try:
        from foundry_local_sdk._native.api import api, ffi  # noqa: F401
    except Exception as e:
        pytest.skip(f"native cffi extension not loadable: {e}")
    from foundry_local_sdk._native.api import api, ffi
    return api, ffi
