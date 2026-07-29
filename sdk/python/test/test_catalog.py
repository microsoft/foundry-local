# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Tests for Catalog – mirrors catalog.test.ts."""

from __future__ import annotations

import json

from foundry_local_sdk.catalog import Catalog
from foundry_local_sdk.detail.core_interop import Response

from .conftest import TEST_MODEL_ALIAS


class TestCatalog:
    """Catalog Tests."""

    def test_should_initialize_with_catalog_name(self, catalog):
        """Catalog should expose a non-empty name string."""
        assert isinstance(catalog.name, str)
        assert len(catalog.name) > 0

    def test_should_list_models(self, catalog):
        """list_models() should return a non-empty list containing the test model."""
        models = catalog.list_models()
        assert isinstance(models, list)
        assert len(models) > 0

        # Verify test model is present
        aliases = {m.alias for m in models}
        assert TEST_MODEL_ALIAS in aliases

    def test_should_get_model_by_alias(self, catalog):
        """get_model() should return a Model whose alias matches."""
        model = catalog.get_model(TEST_MODEL_ALIAS)
        assert model is not None
        assert model.alias == TEST_MODEL_ALIAS

    def test_should_return_none_for_empty_alias(self, catalog):
        """get_model('') should return None (unknown alias)."""
        result = catalog.get_model("")
        assert result is None

    def test_should_return_none_for_unknown_alias(self, catalog):
        """get_model() with a random alias should return None."""
        result = catalog.get_model("definitely-not-a-real-model-alias-12345")
        assert result is None

    def test_should_get_cached_models(self, catalog):
        """get_cached_models() should return a list with at least the test model."""
        cached = catalog.get_cached_models()
        assert isinstance(cached, list)
        assert len(cached) > 0

        # At least the test model should be cached
        aliases = {m.alias for m in cached}
        assert TEST_MODEL_ALIAS in aliases

    def test_should_get_model_variant_by_id(self, catalog):
        """get_model_variant() with a valid ID should return the variant."""
        cached = catalog.get_cached_models()
        assert len(cached) > 0
        variant = cached[0]

        result = catalog.get_model_variant(variant.id)
        assert result is not None
        assert result.id == variant.id

    def test_should_return_none_for_empty_variant_id(self, catalog):
        """get_model_variant('') should return None."""
        result = catalog.get_model_variant("")
        assert result is None

    def test_should_return_none_for_unknown_variant_id(self, catalog):
        """get_model_variant() with a random ID should return None."""
        result = catalog.get_model_variant("definitely-not-a-real-model-id-12345")
        assert result is None

    def test_should_resolve_latest_version_for_model_and_variant_inputs(self):
        """get_latest_version() should resolve latest variant and preserve Model input when already latest."""

        test_model_infos = [
            {
                "id": "test-model:3",
                "name": "test-model",
                "version": 3,
                "alias": "test-alias",
                "displayName": "Test Model",
                "providerType": "test",
                "uri": "test://model/3",
                "modelType": "ONNX",
                "runtime": {"deviceType": "CPU", "executionProvider": "CPUExecutionProvider"},
                "cached": False,
                "createdAt": 1700000003,
            },
            {
                "id": "test-model:2",
                "name": "test-model",
                "version": 2,
                "alias": "test-alias",
                "displayName": "Test Model",
                "providerType": "test",
                "uri": "test://model/2",
                "modelType": "ONNX",
                "runtime": {"deviceType": "CPU", "executionProvider": "CPUExecutionProvider"},
                "cached": False,
                "createdAt": 1700000002,
            },
            {
                "id": "test-model:1",
                "name": "test-model",
                "version": 1,
                "alias": "test-alias",
                "displayName": "Test Model",
                "providerType": "test",
                "uri": "test://model/1",
                "modelType": "ONNX",
                "runtime": {"deviceType": "CPU", "executionProvider": "CPUExecutionProvider"},
                "cached": False,
                "createdAt": 1700000001,
            },
        ]

        class _MockCoreInterop:
            def execute_command(self, command_name, command_input=None):
                if command_name == "get_catalog_name":
                    return Response(data="TestCatalog", error=None)
                if command_name == "get_model_list":
                    return Response(data=json.dumps(test_model_infos), error=None)
                if command_name == "get_cached_models":
                    return Response(data="[]", error=None)
                return Response(data=None, error=f"Unexpected command: {command_name}")

        class _MockModelLoadManager:
            def list_loaded(self):
                return []

        catalog = Catalog(_MockModelLoadManager(), _MockCoreInterop())

        model = catalog.get_model("test-alias")
        assert model is not None

        variants = model.variants
        assert len(variants) == 3

        latest_variant = variants[0]
        middle_variant = variants[1]
        oldest_variant = variants[2]

        assert latest_variant.id == "test-model:3"
        assert middle_variant.id == "test-model:2"
        assert oldest_variant.id == "test-model:1"

        result1 = catalog.get_latest_version(latest_variant)
        assert result1.id == "test-model:3"

        result2 = catalog.get_latest_version(middle_variant)
        assert result2.id == "test-model:3"

        result3 = catalog.get_latest_version(oldest_variant)
        assert result3.id == "test-model:3"

        model.select_variant(latest_variant)
        result4 = catalog.get_latest_version(model)
        assert result4 is model

    def test_should_self_heal_get_model_on_cache_miss(self):
        """get_model() must trigger a forced refresh when the alias is unknown
        in the cached map, surfacing BYOM models added to the cache directory
        after the SDK was warmed up.
        """
        byom_model_infos = [
            {
                "id": "byom-self-heal:1",
                "name": "byom-self-heal",
                "version": 1,
                "alias": "byom-self-heal",
                "displayName": "BYOM Self Heal",
                "providerType": "local",
                "uri": "local://byom-self-heal/1",
                "modelType": "ONNX",
                "runtime": {"deviceType": "CPU", "executionProvider": "CPUExecutionProvider"},
                "cached": True,
                "createdAt": 1700000001,
            }
        ]

        state = {"model_list_calls": 0}

        class _MockCoreInterop:
            def execute_command(self, command_name, command_input=None):
                if command_name == "get_catalog_name":
                    return Response(data="TestCatalog", error=None)
                if command_name == "get_model_list":
                    state["model_list_calls"] += 1
                    # Warm path returns no models; the forced self-heal refresh
                    # returns the BYOM.
                    models = byom_model_infos if state["model_list_calls"] > 1 else []
                    return Response(data=json.dumps(models), error=None)
                return Response(data=None, error=f"Unexpected command: {command_name}")

        class _MockModelLoadManager:
            def list_loaded(self):
                return []

        catalog = Catalog(_MockModelLoadManager(), _MockCoreInterop())

        model = catalog.get_model("byom-self-heal")
        assert model is not None
        assert model.alias == "byom-self-heal"

        variant = catalog.get_model_variant("byom-self-heal:1")
        assert variant is not None
        assert variant.id == "byom-self-heal:1"

        # First lookup: warm + forced refresh (2 calls).
        # Second lookup hits the now-populated cache map directly; the inner
        # TTL-gated _update_models() short-circuits, so no extra fetches.
        assert state["model_list_calls"] == 2

    def test_should_self_heal_get_cached_models_on_unknown_id(self):
        """get_cached_models() must trigger a forced refresh when core returns
        an id that the cached alias/variant maps do not yet know about.
        """
        byom_model_infos = [
            {
                "id": "byom-cached:1",
                "name": "byom-cached",
                "version": 1,
                "alias": "byom-cached",
                "displayName": "BYOM Cached",
                "providerType": "local",
                "uri": "local://byom-cached/1",
                "modelType": "ONNX",
                "runtime": {"deviceType": "CPU", "executionProvider": "CPUExecutionProvider"},
                "cached": True,
                "createdAt": 1700000001,
            }
        ]

        state = {"model_list_calls": 0}

        class _MockCoreInterop:
            def execute_command(self, command_name, command_input=None):
                if command_name == "get_catalog_name":
                    return Response(data="TestCatalog", error=None)
                if command_name == "get_model_list":
                    state["model_list_calls"] += 1
                    models = byom_model_infos if state["model_list_calls"] > 1 else []
                    return Response(data=json.dumps(models), error=None)
                if command_name == "get_cached_models":
                    # Core always reports the BYOM as cached; the SDK has to
                    # self-heal to learn it exists.
                    return Response(data='["byom-cached:1"]', error=None)
                return Response(data=None, error=f"Unexpected command: {command_name}")

        class _MockModelLoadManager:
            def list_loaded(self):
                return []

        catalog = Catalog(_MockModelLoadManager(), _MockCoreInterop())

        cached = catalog.get_cached_models()
        assert len(cached) == 1
        assert cached[0].id == "byom-cached:1"
        assert state["model_list_calls"] == 2

    def test_should_not_refresh_catalog_on_empty_or_whitespace_input(self):
        """get_model() / get_model_variant() must short-circuit on
        empty / whitespace / None input without triggering the (expensive)
        forced catalog refresh that powers the self-heal path.
        """
        state = {"model_list_calls": 0}

        class _MockCoreInterop:
            def execute_command(self, command_name, command_input=None):
                if command_name == "get_catalog_name":
                    return Response(data="TestCatalog", error=None)
                if command_name == "get_model_list":
                    state["model_list_calls"] += 1
                    return Response(data="[]", error=None)
                return Response(data=None, error=f"Unexpected command: {command_name}")

        class _MockModelLoadManager:
            def list_loaded(self):
                return []

        catalog = Catalog(_MockModelLoadManager(), _MockCoreInterop())

        # Warm the catalog so the inner TTL-gated _update_models() would not
        # itself fetch for a valid input either — any increment of
        # model_list_calls below would prove an unwanted forced refresh.
        catalog.list_models()
        assert state["model_list_calls"] == 1

        for invalid in ("", "   ", None):
            assert catalog.get_model(invalid) is None
            assert catalog.get_model_variant(invalid) is None

        assert state["model_list_calls"] == 1


class TestIncrementalRefresh:
    """Catalog._update_models incremental-refresh behavior.

    The refresh path is shared by all Catalog methods. These tests pin down
    the contract that externally-held IModel references and per-Model variant
    selection survive across forced refreshes when the underlying model id is
    still present in the fresh catalog. They guard against regressing back to
    the clear-and-rebuild pattern that churned wrapper identity on every
    refresh.
    """

    @staticmethod
    def _model_info(model_id: str, alias: str, cached: bool, context_length: int = 1024) -> dict:
        return {
            "id": model_id,
            "name": alias,
            "version": int(model_id.split(":")[-1]),
            "alias": alias,
            "displayName": alias,
            "providerType": "local",
            "uri": f"local://{alias}/{model_id.split(':')[-1]}",
            "modelType": "ONNX",
            "runtime": {"deviceType": "CPU", "executionProvider": "CPUExecutionProvider"},
            "cached": cached,
            "createdAt": 1700000001,
            "contextLength": context_length,
        }

    def _make_catalog(self, responses: list[list[dict]]):
        """Build a Catalog whose ``get_model_list`` IPC returns ``responses[N]``
        on the (N+1)-th call. ``get_cached_models`` and ``list_loaded_models``
        return empty.
        """
        state = {"call": 0}

        class _MockCoreInterop:
            def execute_command(self, command_name, command_input=None):
                if command_name == "get_catalog_name":
                    return Response(data="TestCatalog", error=None)
                if command_name == "get_model_list":
                    idx = min(state["call"], len(responses) - 1)
                    state["call"] += 1
                    return Response(data=json.dumps(responses[idx]), error=None)
                if command_name == "get_cached_models":
                    return Response(data="[]", error=None)
                return Response(data=None, error=f"Unexpected command: {command_name}")

        class _MockModelLoadManager:
            def list_loaded(self):
                return []

        return Catalog(_MockModelLoadManager(), _MockCoreInterop()), state

    def test_should_preserve_identity_and_propagate_info_on_refresh(self):
        """A held IModel / ModelVariant reference must survive a forced refresh
        when the id is still present (identity preserved) AND surface fresh
        ModelInfo through the same reference (in-place info refresh, not
        replace-and-rebuild). Mutating info is the natural way to prove the
        post-refresh wrapper IS the pre-refresh wrapper, not a fresh one that
        compares equal.
        """
        first_state = [self._model_info("a:1", "alpha", cached=False, context_length=1024)]
        second_state = [self._model_info("a:1", "alpha", cached=True, context_length=2048)]
        catalog, _ = self._make_catalog([first_state, second_state])

        first_model = catalog.get_model("alpha")
        first_variant = catalog.get_model_variant("a:1")
        assert first_variant.info.cached is False
        assert first_variant.info.context_length == 1024

        catalog._update_models(force=True)

        second_model = catalog.get_model("alpha")
        second_variant = catalog.get_model_variant("a:1")
        assert first_model is second_model
        assert first_variant is second_variant
        # Same held reference now reflects the fresh ModelInfo.
        assert first_variant.info.cached is True
        assert first_variant.info.context_length == 2048

    def test_selection_survives_refresh(self):
        """When the selected variant's id is still present after a refresh,
        ``select_variant()`` survives so subsequent ops (``load``, ``unload``,
        ...) target the user's pick. Wrapper identity is preserved by
        ``Catalog._update_models`` reusing the same ``ModelVariant`` for
        surviving ids, so ``_refresh_variants`` does not need to touch
        ``_selected_variant``.
        """
        v1 = self._model_info("multi:1", "multi", cached=True)
        v2 = self._model_info("multi:2", "multi", cached=True)
        both = [v1, v2]
        catalog, _ = self._make_catalog([both, both])

        model = catalog.get_model("multi")
        variant_v2 = next(v for v in model.variants if v.id == "multi:2")
        model.select_variant(variant_v2)
        assert model.id == "multi:2"

        catalog._update_models(force=True)
        assert model.id == "multi:2"
        assert len(model.variants) == 2

    def test_should_apply_adds_and_removes_on_refresh(self):
        """Ids no longer present must be evicted from both
        ``_model_id_to_model_variant`` and ``_model_alias_to_model``; new ids
        must be inserted as fresh wrappers. Both directions are symmetric and
        share setup, so we cover them in one test against a single refresh
        that does both at once. We probe the internal maps directly to avoid
        triggering the public self-heal paths.
        """
        first_state = [
            self._model_info("a:1", "alpha", cached=True),
            self._model_info("b:1", "beta", cached=True),
        ]
        second_state = [
            self._model_info("a:1", "alpha", cached=True),
            self._model_info("byom:1", "byom-new", cached=True),
        ]
        catalog, _ = self._make_catalog([first_state, second_state])

        # Warm the catalog and confirm the pre-state via internal maps so we
        # don't trip the public self-heal paths.
        catalog.list_models()
        assert "alpha" in catalog._model_alias_to_model
        assert "beta" in catalog._model_alias_to_model
        assert "byom-new" not in catalog._model_alias_to_model
        assert "byom:1" not in catalog._model_id_to_model_variant

        catalog._update_models(force=True)

        assert "alpha" in catalog._model_alias_to_model
        assert "beta" not in catalog._model_alias_to_model
        assert "b:1" not in catalog._model_id_to_model_variant
        assert "byom-new" in catalog._model_alias_to_model
        assert "byom:1" in catalog._model_id_to_model_variant
