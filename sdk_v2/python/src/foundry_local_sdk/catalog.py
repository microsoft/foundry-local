# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
from __future__ import annotations

from foundry_local_sdk.exception import FoundryLocalException
from foundry_local_sdk.imodel import IModel, _ModelImpl


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

    def __init__(self, native_catalog_ptr: object, *, parent: object | None = None) -> None:
        from foundry_local_sdk._native.api import api, ffi

        self._ptr = native_catalog_ptr
        # Keep the owning object (typically the FoundryLocalManager) alive while this
        # Catalog exists. The native flCatalog* is owned by the manager; without this
        # reference, GC could release the manager first and dangle our pointer.
        self._parent = parent

        name_out = ffi.new("const char**")
        api.check_status(api.catalog.GetName(self._ptr, name_out))
        self.name: str = ffi.string(name_out[0]).decode("utf-8") if name_out[0] != ffi.NULL else ""

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
