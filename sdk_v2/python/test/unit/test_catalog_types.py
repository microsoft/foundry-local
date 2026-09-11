# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Unit tests for the public BYOM catalog selection API."""
from __future__ import annotations

import inspect

from foundry_local_sdk import CatalogType, FoundryLocalManager


def test_catalog_type_values_match_native_c_enum() -> None:
    assert CatalogType.PUBLIC == 0
    assert CatalogType.LOCAL == 1


def test_get_catalog_defaults_to_public_catalog() -> None:
    default = inspect.signature(FoundryLocalManager.get_catalog).parameters["catalog_type"].default
    assert default is CatalogType.PUBLIC