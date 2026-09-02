# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
from __future__ import annotations

import os
from enum import IntEnum

from foundry_local_sdk.exception import FoundryLocalException
from foundry_local_sdk.imodel import IModel, _ModelImpl
from foundry_local_sdk.model_info import ModelInfo


class CatalogType(IntEnum):
    """Catalogs exposed by :class:`FoundryLocalManager`."""

    PUBLIC = 0
    LOCAL = 1


def _consume_model_list(ml, api, ffi, parent: object | None = None) -> list[IModel]:
    """Drain a native flModelList* into _ModelImpl wrappers, then release it.

    ``parent`` is the owning ``Catalog`` (or other object) whose lifetime must
    outlive the returned models — each ``_ModelImpl`` keeps a strong reference
    to it to prevent the underlying native pointer from being released early.
    """
    try:
        count = api.root.ModelList_Size(ml)
        return [_ModelImpl(api.root.ModelList_GetAt(ml, i), parent=parent) for i in range(count)]
    finally:
        api.root.ModelList_Release(ml)


class Catalog:
    """Model catalog for discovering and querying available models.

    Flat pass-through to the native ``flCatalogApi`` vtable.
    No grouping, caching, or alias merging — all that lives in the native layer.

    The ``Catalog`` does NOT own the native ``flCatalog*`` pointer — the
    ``FoundryLocalManager`` does.
    """

    def __init__(
        self,
        native_catalog_ptr: object,
        *,
        catalog_type: CatalogType = CatalogType.PUBLIC,
        parent: object | None = None,
    ) -> None:
        from foundry_local_sdk._native.api import api, ffi

        self._ptr = native_catalog_ptr
        self._catalog_type = catalog_type
        # Keep the owning object (typically the FoundryLocalManager) alive while this
        # Catalog exists. The native flCatalog* is owned by the manager; without this
        # reference, GC could release the manager first and dangle our pointer.
        self._parent = parent

        name_out = ffi.new("const char**")
        api.check_status(api.catalog.GetName(self._ptr, name_out))
        self.name: str = ffi.string(name_out[0]).decode("utf-8") if name_out[0] != ffi.NULL else ""

    @property
    def catalog_type(self) -> CatalogType:
        """Whether this is the public or local catalog."""
        return self._catalog_type

    # ------------------------------------------------------------------
    # Public query methods
    # ------------------------------------------------------------------

    def list_models(self) -> list[IModel]:
        """List the available models in the catalog.

        Returns:
            List of ``IModel`` instances, one per model alias.
        """
        from foundry_local_sdk._native.api import api, ffi

        ml_out = ffi.new("flModelList**")
        api.check_status(api.catalog.GetModels(self._ptr, ml_out))
        return _consume_model_list(ml_out[0], api, ffi, parent=self)

    def register_model(
        self,
        model_path: str | os.PathLike[str],
        model_id: str,
        metadata: ModelInfo,
    ) -> IModel:
        """Register existing model assets in the local catalog.

        The native catalog copies ``metadata`` and does not take ownership of
        the model directory. ``model_id`` must use ``<name>:<version>`` format,
        and ``model_path`` must contain ``genai_config.json``.

        Args:
            model_path: Existing model directory.
            model_id: Unique canonical model ID.
            metadata: Mutable registration metadata. At minimum, set ``task``.

        Returns:
            A borrowed model wrapper kept valid by this catalog's manager.
        """
        if self.catalog_type is not CatalogType.LOCAL:
            raise FoundryLocalException("Models can only be registered in the local catalog.")
        if not isinstance(metadata, ModelInfo):
            raise TypeError("metadata must be a ModelInfo instance")

        from foundry_local_sdk._native.api import api, ffi

        model_path_bytes = os.fspath(model_path).encode("utf-8")
        model_id_bytes = model_id.encode("utf-8")
        out = ffi.new("flModel**")
        api.check_status(
            api.catalog.RegisterModel(
                self._ptr,
                model_path_bytes,
                model_id_bytes,
                metadata._native_ptr,
                out,
            )
        )
        if out[0] == ffi.NULL:
            raise FoundryLocalException("RegisterModel returned no model.")
        return _ModelImpl(out[0], parent=self)

    def unregister_model(self, alias_or_model_id: str) -> None:
        """Unregister a local model without deleting its assets.

        Existing model wrappers remain valid for metadata and cleanup queries,
        but future catalog queries no longer return the registration.
        """
        if self.catalog_type is not CatalogType.LOCAL:
            raise FoundryLocalException("Models can only be unregistered from the local catalog.")

        from foundry_local_sdk._native.api import api

        identifier_bytes = alias_or_model_id.encode("utf-8")
        api.check_status(api.catalog.UnregisterModel(self._ptr, identifier_bytes))

    def get_model(self, model_alias: str) -> IModel | None:
        """Lookup a model by its alias.

        Args:
            model_alias: Model alias.

        Returns:
            ``IModel`` if found, ``None`` otherwise.
        """
        from foundry_local_sdk._native.api import api, ffi

        out = ffi.new("flModel**")
        api.check_status(api.catalog.GetModel(self._ptr, model_alias.encode("utf-8"), out))
        if out[0] == ffi.NULL:
            return None
        return _ModelImpl(out[0], parent=self)

    def get_model_variant(self, model_id: str) -> IModel | None:
        """Lookup a specific model variant by its unique model id.

        NOTE: Returns an ``IModel`` representing the single requested variant.
        Use ``get_model`` to obtain an ``IModel`` exposing all available
        variants for the same alias.

        Args:
            model_id: Model id.

        Returns:
            ``IModel`` if found, ``None`` otherwise.
        """
        from foundry_local_sdk._native.api import api, ffi

        out = ffi.new("flModel**")
        api.check_status(api.catalog.GetModelVariant(self._ptr, model_id.encode("utf-8"), out))
        if out[0] == ffi.NULL:
            return None
        return _ModelImpl(out[0], parent=self)

    def get_latest_version(self, model_or_model_variant: IModel) -> IModel:
        """Resolve the latest catalog version for the provided model or variant.

        Args:
            model_or_model_variant: ``IModel`` to resolve.

        Returns:
            Latest catalog version for the same model name.
        """
        from foundry_local_sdk._native.api import api, ffi

        if not isinstance(model_or_model_variant, _ModelImpl):
            raise FoundryLocalException(
                "model_or_model_variant must be an IModel returned from this Catalog."
            )

        out = ffi.new("flModel**")
        api.check_status(
            api.catalog.GetLatestVersion(self._ptr, model_or_model_variant._ptr, out)
        )
        if out[0] == ffi.NULL:
            raise FoundryLocalException(
                "get_latest_version returned no model. The IModel argument was not produced by this catalog."
            )
        return _ModelImpl(out[0], parent=self)

    def get_cached_models(self) -> list[IModel]:
        """Get a list of currently downloaded models from the model cache.

        Returns:
            One ``IModel`` instance per cached model variant.
        """
        from foundry_local_sdk._native.api import api, ffi

        ml_out = ffi.new("flModelList**")
        api.check_status(api.catalog.GetCachedModels(self._ptr, ml_out))
        return _consume_model_list(ml_out[0], api, ffi, parent=self)

    def get_loaded_models(self) -> list[IModel]:
        """Get a list of currently loaded models.

        Returns:
            List of ``IModel`` instances (leaf variants loaded in memory).
        """
        from foundry_local_sdk._native.api import api, ffi

        ml_out = ffi.new("flModelList**")
        api.check_status(api.catalog.GetLoadedModels(self._ptr, ml_out))
        return _consume_model_list(ml_out[0], api, ffi, parent=self)

    def get_model_versions(
        self,
        model_alias: str,
        model_name: str | None = None,
        max_versions: int = 50,
    ) -> list[IModel]:
        """Get all versions of a model alias.

        Args:
            model_alias: Model alias. Must be non-empty.
            model_name: Optional variant name; ``None`` or an empty string returns every
                variant for the alias.
            max_versions: Select the latest ``X`` versions per variant name. Defaults to
                50, matching the web service contract. Pass ``0`` or a negative value for
                no per-variant cap.

        Returns:
            One ``IModel`` instance per matching model variant.
        """
        from foundry_local_sdk._native.api import api, ffi

        alias_bytes = model_alias.encode("utf-8")
        model_name_ptr = ffi.NULL if model_name is None else model_name.encode("utf-8")

        ml_out = ffi.new("flModelList**")
        api.check_status(
            api.catalog.GetModelVersions(
                self._ptr,
                alias_bytes,
                model_name_ptr,
                max_versions,
                ml_out,
            )
        )
        return _consume_model_list(ml_out[0], api, ffi, parent=self)
