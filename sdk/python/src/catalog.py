# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------

from __future__ import annotations

import datetime
import logging
import threading
from typing import List, Optional
from pydantic import TypeAdapter

from .imodel import IModel
from .detail.model import Model
from .detail.model_variant import ModelVariant

from .detail.core_interop import CoreInterop, get_cached_model_ids
from .detail.model_data_types import ModelInfo
from .detail.model_load_manager import ModelLoadManager
from .exception import FoundryLocalException

logger = logging.getLogger(__name__)

class Catalog():
    """Model catalog for discovering and querying available models.

    Provides methods to list models, look up by alias or ID, and query
    cached or loaded models. The model list is refreshed every 6 hours.
    """

    def __init__(self, model_load_manager: ModelLoadManager, core_interop: CoreInterop):
        """Initialize the Catalog.

        Args:
            model_load_manager: Manager for loading/unloading models.
            core_interop: Native interop layer for Foundry Local Core.
        """
        self._core_interop = core_interop
        self._model_load_manager = model_load_manager
        self._lock = threading.Lock()

        self._models: List[ModelInfo] = []
        self._model_alias_to_model = {}
        self._model_id_to_model_variant = {}
        self._last_fetch = datetime.datetime.min

        response = core_interop.execute_command("get_catalog_name")
        if response.error is not None:
            raise FoundryLocalException(f"Failed to get catalog name: {response.error}")

        self.name = response.data

    def _update_models(self, force: bool = False):
        with self._lock:
            # refresh every 6 hours, or immediately when forced (e.g. self-heal
            # after a get_cached_models / get_model cache miss caused by a
            # manually-added (BYOM) model dropped into the cache directory).
            if not force and (datetime.datetime.now() - self._last_fetch) < datetime.timedelta(hours=6):
                return

            response = self._core_interop.execute_command("get_model_list")
            if response.error is not None:
                raise FoundryLocalException(f"Failed to get model list: {response.error}")

            model_list_json = response.data

            adapter = TypeAdapter(list[ModelInfo])
            models: List[ModelInfo] = adapter.validate_json(model_list_json)

            # Incremental refresh: preserve wrapper identity for ids/aliases
            # that survive the refresh so externally-held ``IModel`` references
            # keep working with up-to-date metadata and (for ``Model``) keep
            # any explicit ``select_variant()`` choice. New ids get fresh
            # wrappers; removed ids get evicted.

            fresh_ids: set[str] = set()
            fresh_alias_groups: dict[str, List[ModelInfo]] = {}
            for model_info in models:
                fresh_ids.add(model_info.id)
                fresh_alias_groups.setdefault(model_info.alias, []).append(model_info)

            for stale_id in [mid for mid in self._model_id_to_model_variant if mid not in fresh_ids]:
                del self._model_id_to_model_variant[stale_id]
            for stale_alias in [a for a in self._model_alias_to_model if a not in fresh_alias_groups]:
                del self._model_alias_to_model[stale_alias]

            for model_info in models:
                existing_variant = self._model_id_to_model_variant.get(model_info.id)
                if existing_variant is not None:
                    existing_variant._refresh_info(model_info)
                else:
                    self._model_id_to_model_variant[model_info.id] = ModelVariant(
                        model_info, self._model_load_manager, self._core_interop
                    )

            for alias, alias_infos in fresh_alias_groups.items():
                alias_variants = [self._model_id_to_model_variant[mi.id] for mi in alias_infos]
                existing_model = self._model_alias_to_model.get(alias)
                if existing_model is None:
                    new_model = Model(alias_variants[0], self._core_interop)
                    for variant in alias_variants[1:]:
                        new_model._add_variant(variant)
                    self._model_alias_to_model[alias] = new_model
                else:
                    existing_model._refresh_variants(alias_variants)

            self._models = models
            self._last_fetch = datetime.datetime.now()

    def _invalidate_cache(self):
        with self._lock:
            self._last_fetch = datetime.datetime.min

    def list_models(self) -> List[IModel]:
        """
        List the available models in the catalog.
        :return: List of IModel instances.
        """
        self._update_models()
        return list(self._model_alias_to_model.values())

    def get_model(self, model_alias: str) -> Optional[IModel]:
        """
        Lookup a model by its alias.
        :param model_alias: Model alias.
        :return: IModel if found.
        """
        if not model_alias or not model_alias.strip():
            return None

        self._update_models()
        model = self._model_alias_to_model.get(model_alias)
        if model is not None:
            return model

        # Self-heal: the alias may belong to a BYOM model added to the cache
        # directory after our last catalog refresh.
        self._update_models(force=True)
        return self._model_alias_to_model.get(model_alias)

    def get_model_variant(self, model_id: str) -> Optional[IModel]:
        """
        Lookup a model variant by its unique model id.
        NOTE: This will return an IModel with a single variant. Use get_model to get an IModel with all available
        variants.
        :param model_id: Model id.
        :return: IModel if found.
        """
        if not model_id or not model_id.strip():
            return None

        self._update_models()
        variant = self._model_id_to_model_variant.get(model_id)
        if variant is not None:
            return variant

        # Self-heal: the id may belong to a BYOM model added to the cache
        # directory after our last catalog refresh.
        self._update_models(force=True)
        return self._model_id_to_model_variant.get(model_id)

    def get_latest_version(self, model_or_model_variant: IModel) -> IModel:
        """
        Resolve the latest catalog version for the provided model or variant.

        :param model_or_model_variant: IModel to resolve.
        :return: Latest catalog version for the same model name.
        :raises FoundryLocalException: If the alias or name cannot be resolved.
        """
        self._update_models()

        model = self._model_alias_to_model.get(model_or_model_variant.alias)
        if model is None:
            raise FoundryLocalException(
                f"Model with alias '{model_or_model_variant.alias}' not found in catalog."
            )

        latest = next(
            (variant for variant in model.variants if variant.info.name == model_or_model_variant.info.name),
            None,
        )
        if latest is None:
            raise FoundryLocalException(
                f"Internal error. Mismatch between model (alias:{model.alias}) and "
                f"model variant (alias:{model_or_model_variant.alias})."
            )

        return model_or_model_variant if latest.id == model_or_model_variant.id else latest

    def get_cached_models(self) -> List[IModel]:
        """
        Get a list of currently downloaded models from the model cache.
        :return: List of IModel instances.
        """
        self._update_models()

        cached_model_ids = get_cached_model_ids(self._core_interop)
        return self._resolve_model_ids(cached_model_ids)

    def get_loaded_models(self) -> List[IModel]:
        """
        Get a list of the currently loaded models.
        :return: List of IModel instances.
        """
        self._update_models()

        loaded_model_ids = self._model_load_manager.list_loaded()
        return self._resolve_model_ids(loaded_model_ids)

    def _resolve_model_ids(self, model_ids: List[str]) -> List[IModel]:
        """Resolve a list of model ids against the in-memory catalog,
        self-healing once if any id is unknown (e.g. a manually-added BYOM
        model the SDK has not yet seen). Preserves the input order of
        ``model_ids`` (minus unknowns).
        """
        if any(model_id not in self._model_id_to_model_variant for model_id in model_ids):
            self._update_models(force=True)

        resolved: List[IModel] = []
        for model_id in model_ids:
            variant = self._model_id_to_model_variant.get(model_id)
            if variant is not None:
                resolved.append(variant)
        return resolved