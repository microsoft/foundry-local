# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
from __future__ import annotations

import logging
from threading import Event
from typing import Callable, List, Optional

from ..imodel import IModel
from ..openai.chat_client import ChatClient
from ..openai.audio_client import AudioClient
from ..openai.embedding_client import EmbeddingClient
from .model_variant import ModelVariant
from ..exception import FoundryLocalException
from .core_interop import CoreInterop
from .model_data_types import ModelInfo

logger = logging.getLogger(__name__)


class Model(IModel):
    """A model identified by an alias that groups one or more variants.

    Operations are delegated to the currently selected variant.
    """

    def __init__(self, model_variant: ModelVariant, core_interop: CoreInterop):
        self._alias = model_variant.alias
        self._variants: List[ModelVariant] = [model_variant]
        # Variants are sorted by Core, so the first one added is the default
        self._selected_variant = model_variant
        self._core_interop = core_interop

    def _add_variant(self, variant: ModelVariant) -> None:
        if variant.alias != self._alias:
            raise FoundryLocalException(
                f"Variant alias {variant.alias} does not match model alias {self._alias}"
            )

        self._variants.append(variant)

        # Prefer the highest priority locally cached variant
        if variant.info.cached and not self._selected_variant.info.cached:
            self._selected_variant = variant

    def _refresh_variants(self, variants: List[ModelVariant]) -> None:
        """Replace the variant list in place while preserving wrapper identity.

        Called by ``Catalog._update_models`` during incremental refresh so a
        user's held ``Model`` reference keeps pointing at the same object
        across refreshes. Because ``Catalog._update_models`` reuses the same
        ``ModelVariant`` wrappers for ids that survive a refresh, any
        explicit ``select_variant()`` choice that survives the refresh is
        preserved without any extra work here.

        ``variants`` must be a non-empty list of ModelVariants all sharing
        ``self._alias``.

        TODO: tighten the held-reference contract for the case where the
        previously selected variant is removed by a refresh; today
        ``_selected_variant`` keeps pointing at the dropped wrapper and
        callers must explicitly re-select.
        """
        if not variants:
            raise FoundryLocalException(
                f"Cannot refresh model {self._alias} with an empty variant list"
            )

        self._variants = list(variants)

    def select_variant(self, variant: IModel) -> None:
        """
        Select a specific model variant to use for IModel operations.
        An IModel from ``variants`` can also be used directly.

        :param variant: IModel to select. Must be one of the variants in ``variants``.
        :raises FoundryLocalException: If variant is not valid for this model
        """
        matching = next((v for v in self._variants if v.id == variant.id), None)
        if matching is None:
            raise FoundryLocalException(
                "Input variant was not found in Variants."
            )

        self._selected_variant = matching

    @property
    def variants(self) -> List[IModel]:
        """List of all variants for this model."""
        return list(self._variants)  # Return a copy to prevent external modification

    @property
    def id(self) -> str:
        """Model Id of the currently selected variant."""
        return self._selected_variant.id

    @property
    def alias(self) -> str:
        """Alias of this model."""
        return self._alias

    @property
    def info(self) -> ModelInfo:
        """ModelInfo of the currently selected variant."""
        return self._selected_variant.info

    @property
    def context_length(self) -> Optional[int]:
        """Maximum context length (in tokens) of the currently selected variant."""
        return self._selected_variant.context_length

    @property
    def input_modalities(self) -> Optional[str]:
        """Comma-separated input modalities of the currently selected variant."""
        return self._selected_variant.input_modalities

    @property
    def output_modalities(self) -> Optional[str]:
        """Comma-separated output modalities of the currently selected variant."""
        return self._selected_variant.output_modalities

    @property
    def capabilities(self) -> Optional[str]:
        """Comma-separated capability tags of the currently selected variant."""
        return self._selected_variant.capabilities

    @property
    def supports_tool_calling(self) -> Optional[bool]:
        """Whether the currently selected variant supports tool/function calling."""
        return self._selected_variant.supports_tool_calling

    @property
    def is_cached(self) -> bool:
        """Is the currently selected variant cached locally?"""
        return self._selected_variant.is_cached

    @property
    def is_loaded(self) -> bool:
        """Is the currently selected variant loaded in memory?"""
        return self._selected_variant.is_loaded

    def download(self, progress_callback: Optional[Callable[[float], None]] = None,
                 cancel_event: Optional[Event] = None) -> None:
        """Download the currently selected variant."""
        self._selected_variant.download(progress_callback, cancel_event)

    def get_path(self) -> str:
        """Get the path to the currently selected variant."""
        return self._selected_variant.get_path()

    def load(self) -> None:
        """Load the currently selected variant into memory."""
        self._selected_variant.load()

    def unload(self) -> None:
        """Unload the currently selected variant from memory."""
        self._selected_variant.unload()

    def remove_from_cache(self) -> None:
        """Remove the currently selected variant from the local cache."""
        self._selected_variant.remove_from_cache()

    def get_chat_client(self) -> ChatClient:
        """Get a chat client for the currently selected variant."""
        return self._selected_variant.get_chat_client()
    
    def get_audio_client(self) -> AudioClient:
        """Get an audio client for the currently selected variant."""
        return self._selected_variant.get_audio_client()

    def get_embedding_client(self) -> EmbeddingClient:
        """Get an embedding client for the currently selected variant."""
        return self._selected_variant.get_embedding_client()
