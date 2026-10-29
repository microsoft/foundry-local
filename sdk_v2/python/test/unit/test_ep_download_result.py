# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Deterministic tests for execution-provider download result reporting."""
from __future__ import annotations

from foundry_local_sdk.ep_types import EpInfo
from foundry_local_sdk.foundry_local_manager import _build_ep_download_result

CUDA = "CUDAExecutionProvider"
WEBGPU = "WebGpuExecutionProvider"


def test_explicit_known_failure_is_reported():
    result = _build_ep_download_result(
        [EpInfo(CUDA, False)],
        [EpInfo(CUDA, False)],
        [CUDA],
    )

    assert result.success is False
    assert result.status == "Some EPs failed"
    assert result.failed_eps == [CUDA]
    assert result.registered_eps == []


def test_all_providers_partial_failure_is_reported():
    result = _build_ep_download_result(
        [EpInfo(CUDA, False), EpInfo(WEBGPU, False)],
        [EpInfo(CUDA, True), EpInfo(WEBGPU, False)],
        None,
    )

    assert result.success is False
    assert result.status == "Some EPs failed"
    assert result.registered_eps == [CUDA]
    assert result.failed_eps == [WEBGPU]


def test_explicit_unknown_provider_is_reported():
    result = _build_ep_download_result([], [], ["UnknownExecutionProvider"])

    assert result.success is False
    assert result.status == "Some EPs failed"
    assert result.failed_eps == ["UnknownExecutionProvider"]


def test_already_registered_provider_succeeds_without_registration_delta():
    result = _build_ep_download_result(
        [EpInfo(CUDA, True)],
        [EpInfo(CUDA, True)],
        [CUDA],
    )

    assert result.success is True
    assert result.status == "All EPs registered"
    assert result.registered_eps == []
    assert result.failed_eps == []


def test_newly_registered_provider_is_reported():
    result = _build_ep_download_result(
        [EpInfo(CUDA, False)],
        [EpInfo(CUDA, True)],
        [CUDA],
    )

    assert result.success is True
    assert result.status == "All EPs registered"
    assert result.registered_eps == [CUDA]
    assert result.failed_eps == []
