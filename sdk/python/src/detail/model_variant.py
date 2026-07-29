# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
from __future__ import annotations

import logging
from threading import Event
from typing import Callable, List, Optional

from ..imodel import IModel
from ..exception import FoundryLocalException

from .core_interop import CoreInterop, InteropRequest
from .model_data_types import ModelInfo
from .core_interop import get_cached_model_ids
from .model_load_manager import ModelLoadManager
from ..openai.audio_client import AudioClient
from ..openai.chat_client import ChatClient
from ..openai.embedding_client import EmbeddingClient

logger = logging.getLogger(__name__)


class ModelVariant(IModel):
    """A specific variant of a model (e.g. a particular device type, version, or quantization).

    Implements ``IModel`` and provides download, cache, load/unload, and
    client-creation operations for a single model variant.
    """

    def __init__(self, model_info: ModelInfo, model_load_manager: ModelLoadManager, core_interop: CoreInterop):
        """Initialize a ModelVariant.

        Args:
            model_info: Catalog metadata for this variant.
            model_load_manager: Manager for loading/unloading models.
            core_interop: Native interop layer for Foundry Local Core.
        """
        self._model_info = model_info
        self._model_load_manager = model_load_manager
        self._core_interop = core_interop

        self._id = model_info.id
        self._alias = model_info.alias

    def _refresh_info(self, model_info: ModelInfo) -> None:
        """Update the cached ``ModelInfo`` snapshot in place.

        Called by ``Catalog._update_models`` when refreshing the catalog so
        wrapper identity is preserved across refreshes while still surfacing
        fresh metadata (notably ``cached``) on held references.

        ``id`` and ``alias`` are immutable for a given variant; callers must
        only invoke this with a ``model_info`` whose id matches ``self.id``.
        Pointer reassignment is atomic under the GIL, so concurrent readers
        observe either the old or new snapshot, never a torn intermediate.
        """
        self._model_info = model_info

    @property
    def id(self) -> str:
        """Unique model variant ID (e.g. ``name:version``)."""
        return self._id

    @property
    def alias(self) -> str:
        """Model alias shared across variants."""
        return self._alias

    @property
    def info(self) -> ModelInfo:
        """Full catalog metadata for this variant."""
        return self._model_info

    @property
    def context_length(self) -> Optional[int]:
        """Maximum context length (in tokens) supported by this variant, or ``None`` if unknown."""
        return self._model_info.context_length

    @property
    def variants(self) -> List[IModel]:
        """A ModelVariant is a single variant, so variants returns itself."""
        return [self]

    def select_variant(self, variant: IModel) -> None:
        """SelectVariant is not supported on a ModelVariant.

        Call ``Catalog.get_model()`` to get an IModel with all variants available.

        :raises FoundryLocalException: Always.
        """
        raise FoundryLocalException(
            f"select_variant is not supported on a ModelVariant. "
            f'Call Catalog.get_model("{self._alias}") to get an IModel with all variants available.'
        )

    @property
    def input_modalities(self) -> Optional[str]:
        """Comma-separated input modalities (e.g. ``"text,image"``), or ``None`` if unknown."""
        return self._model_info.input_modalities

    @property
    def output_modalities(self) -> Optional[str]:
        """Comma-separated output modalities (e.g. ``"text"``), or ``None`` if unknown."""
        return self._model_info.output_modalities

    @property
    def capabilities(self) -> Optional[str]:
        """Comma-separated capability tags (e.g. ``"chat,completion"``), or ``None`` if unknown."""
        return self._model_info.capabilities

    @property
    def supports_tool_calling(self) -> Optional[bool]:
        """Whether this variant supports tool/function calling, or ``None`` if unknown."""
        return self._model_info.supports_tool_calling

    @property
    def is_cached(self) -> bool:
        """``True`` if this variant is present in the local model cache."""
        cached_model_ids = get_cached_model_ids(self._core_interop)
        return self.id in cached_model_ids

    @property
    def is_loaded(self) -> bool:
        """``True`` if this variant is currently loaded into memory."""
        loaded_model_ids = self._model_load_manager.list_loaded()
        return self.id in loaded_model_ids

    def download(self, progress_callback: Callable[[float], None] = None,
                 cancel_event: Optional[Event] = None):
        """Download this variant to the local cache.

        Args:
            progress_callback: Optional callback receiving download progress as a
                percentage (0.0 to 100.0).
            cancel_event: Optional ``threading.Event``. When set, the download will be
                cancelled at the next progress update and ``FoundryLocalException`` is raised.
        """
        self._download_impl(progress_callback, cancel_event)

    def _download_impl(self, progress_callback: Callable[[float], None] = None,
                       cancel_event: Optional[Event] = None) -> None:
        request = InteropRequest(params={"Model": self.id})
        if progress_callback is None and cancel_event is None:
            response = self._core_interop.execute_command("download_model", request)
        else:
            # Use the callback path when either progress or cancellation is needed.
            # Ignore invalid progress chunks so cancellation-only downloads
            # still tolerate any non-progress output from the native layer.
            def _on_chunk(chunk: str) -> None:
                if progress_callback is None:
                    return

                try:
                    progress_callback(float(chunk))
                except ValueError:
                    pass

            response = self._core_interop.execute_command_with_callback(
                "download_model", request,
                _on_chunk,
                cancel_event,
            )

        logger.info("Download response: %s", response)
        if response.error is not None:
            raise FoundryLocalException(f"Failed to download model: {response.error}")

    def get_path(self) -> str:
        """Get the local file-system path to this variant if cached.

        Returns:
            Path to the model directory.

        Raises:
            FoundryLocalException: If the model path cannot be retrieved.
        """
        request = InteropRequest(params={"Model": self.id})
        response = self._core_interop.execute_command("get_model_path", request)
        if response.error is not None:
            raise FoundryLocalException(f"Failed to get model path: {response.error}")

        return response.data

    def load(self) -> None:
        """Load this variant into memory for inference."""
        self._model_load_manager.load(self.id)

    def remove_from_cache(self) -> None:
        """Remove this variant from the local model cache."""
        request = InteropRequest(params={"Model": self.id})
        response = self._core_interop.execute_command("remove_cached_model", request)
        if response.error is not None:
            raise FoundryLocalException(f"Failed to remove model from cache: {response.error}")


    def unload(self) -> None:
        """Unload this variant from memory."""
        self._model_load_manager.unload(self.id)

    def get_chat_client(self) -> ChatClient:
        """Create an OpenAI-compatible ``ChatClient`` for this variant."""
        return ChatClient(self.id, self._core_interop)

    def get_audio_client(self) -> AudioClient:
        """Create an OpenAI-compatible ``AudioClient`` for this variant."""
        return AudioClient(self.id, self._core_interop)

    def get_embedding_client(self) -> EmbeddingClient:
        """Create an OpenAI-compatible ``EmbeddingClient`` for this variant."""
        return EmbeddingClient(self.id, self._core_interop)
