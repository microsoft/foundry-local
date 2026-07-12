# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""OpenAI-compatible embedding client backed by the Foundry Local native layer."""
from __future__ import annotations

import json
import warnings
from typing import TYPE_CHECKING

from openai.types import CreateEmbeddingResponse
from typing_extensions import deprecated

if TYPE_CHECKING:
    from foundry_local_sdk.imodel import IModel


@deprecated(
    "The OpenAI direct client is deprecated and will be removed at the end of 2026; use EmbeddingsSession. "
    "OpenAI types remain supported for the web-server path."
)
class EmbeddingClient:
    """OpenAI-compatible embedding client backed by Foundry Local Core.

    .. deprecated::
        The OpenAI direct client is deprecated and will be removed at the end of 2026; use
        :class:`foundry_local_sdk.session.EmbeddingsSession`. OpenAI types
        remain supported for the web-server path.

    Each call creates a fresh native session (stateless — no session history).

    Attributes:
        model_id: The ID of the loaded embedding model variant.
    """

    def __init__(self, model_id: str, model: IModel) -> None:
        warnings.warn(
            "The OpenAI direct client is deprecated and will be removed at the end of 2026; use "
            "EmbeddingsSession. OpenAI types remain supported for the web-server path.",
            DeprecationWarning,
            stacklevel=2,
        )
        self.model_id = model_id
        # Hold the IModel reference so the underlying native model pointer
        # cannot be released out from under us.
        self._model = model

    @staticmethod
    def _validate_input(input_text: str) -> None:
        """Validate that the input is a non-empty string."""
        if not isinstance(input_text, str) or not input_text.strip():
            raise ValueError("Input must be a non-empty string.")

    def _build_request_json(self, input_value: str | list[str]) -> str:
        """Build the JSON payload for an embeddings request."""
        return json.dumps({"model": self.model_id, "input": input_value})

    def _run_native_request(self, request_json: str) -> str:
        """Create a fresh EmbeddingsSession, process the request, return the response JSON string."""
        from foundry_local_sdk.items import TextItem, TextItemType
        from foundry_local_sdk.request import Request
        from foundry_local_sdk.session import EmbeddingsSession

        with (
            EmbeddingsSession(self._model) as session,
            Request() as request,
        ):
            request.add_item(TextItem(request_json, TextItemType.OPENAI_JSON))
            with session.process_request(request) as response:
                # Copy the text out of the (response-owned) item before the response is released.
                return response.get_item(0).text

    def _parse_response(self, response_json: str) -> CreateEmbeddingResponse:
        """Parse the response JSON and apply fields required by the OpenAI type."""
        data = json.loads(response_json)

        # The server may omit "object" on embedding items and "usage" on the response;
        # add defaults so CreateEmbeddingResponse.model_validate doesn't reject them.
        for item in data.get("data", []):
            if "object" not in item:
                item["object"] = "embedding"

        if "usage" not in data:
            data["usage"] = {"prompt_tokens": 0, "total_tokens": 0}

        return CreateEmbeddingResponse.model_validate(data)

    def generate_embedding(self, input_text: str) -> CreateEmbeddingResponse:
        """Generate embeddings for a single input text.

        Args:
            input_text: The text to generate embeddings for.

        Returns:
            A ``CreateEmbeddingResponse`` containing the embedding vector.

        Raises:
            ValueError: If *input_text* is not a non-empty string.
            FoundryLocalException: If the native embeddings call fails.
        """
        self._validate_input(input_text)

        request_json = self._build_request_json(input_text)
        response_json = self._run_native_request(request_json)
        return self._parse_response(response_json)

    def generate_embeddings(self, inputs: list[str]) -> CreateEmbeddingResponse:
        """Generate embeddings for multiple input texts in a single request.

        Args:
            inputs: The texts to generate embeddings for.

        Returns:
            A ``CreateEmbeddingResponse`` containing one embedding vector per input.

        Raises:
            ValueError: If *inputs* is empty or any element is empty.
            FoundryLocalException: If the native embeddings call fails.
        """
        if not inputs:
            raise ValueError("Inputs must be a non-empty list of strings.")
        for text in inputs:
            self._validate_input(text)

        request_json = self._build_request_json(inputs)
        response_json = self._run_native_request(request_json)
        return self._parse_response(response_json)
