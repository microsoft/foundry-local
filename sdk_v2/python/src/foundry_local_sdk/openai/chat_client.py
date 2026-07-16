# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""OpenAI-compatible chat completion client backed by the Foundry Local native layer."""
from __future__ import annotations

import json
import warnings
from typing import TYPE_CHECKING, Any, Generator

from openai.types.chat.chat_completion_message_param import ChatCompletionMessageParam
from openai.types.chat import ChatCompletion
from openai.types.chat.chat_completion_chunk import ChatCompletionChunk
from typing_extensions import deprecated

if TYPE_CHECKING:
    from foundry_local_sdk.imodel import IModel


class ChatClientSettings:
    """Settings for chat completion requests.

    Attributes match the OpenAI chat completion API parameters.
    Foundry-specific settings (``top_k``, ``random_seed``) are sent via metadata.
    """

    def __init__(
        self,
        frequency_penalty: float | None = None,
        max_tokens: int | None = None,
        n: int | None = None,
        temperature: float | None = None,
        presence_penalty: float | None = None,
        random_seed: int | None = None,
        top_k: int | None = None,
        top_p: float | None = None,
        response_format: dict[str, Any] | None = None,
        tool_choice: dict[str, Any] | None = None,
    ):
        self.frequency_penalty = frequency_penalty
        self.max_tokens = max_tokens
        self.n = n
        self.temperature = temperature
        self.presence_penalty = presence_penalty
        self.random_seed = random_seed
        self.top_k = top_k
        self.top_p = top_p
        self.response_format = response_format
        self.tool_choice = tool_choice

    def _serialize(self) -> dict[str, Any]:
        """Serialize settings into an OpenAI-compatible request dict."""
        self._validate_response_format(self.response_format)
        self._validate_tool_choice(self.tool_choice)

        result: dict[str, Any] = {
            k: v for k, v in {
                "frequency_penalty": self.frequency_penalty,
                "max_tokens": self.max_tokens,
                "n": self.n,
                "presence_penalty": self.presence_penalty,
                "temperature": self.temperature,
                "top_p": self.top_p,
                "response_format": self.response_format,
                "tool_choice": self.tool_choice,
            }.items() if v is not None
        }

        metadata: dict[str, str] = {}
        if self.top_k is not None:
            metadata["top_k"] = str(self.top_k)
        if self.random_seed is not None:
            metadata["random_seed"] = str(self.random_seed)

        if metadata:
            result["metadata"] = metadata

        return result

    def _validate_response_format(self, response_format: dict[str, Any] | None) -> None:
        if response_format is None:
            return
        valid_types = ["text", "json_object", "json_schema", "lark_grammar"]
        fmt_type = response_format.get("type")
        if fmt_type not in valid_types:
            raise ValueError(f"ResponseFormat type must be one of: {', '.join(valid_types)}")
        grammar_types = ["json_schema", "lark_grammar"]
        if fmt_type in grammar_types:
            if fmt_type == "json_schema" and (
                not isinstance(response_format.get("json_schema"), str)
                or not response_format["json_schema"].strip()
            ):
                raise ValueError('ResponseFormat with type "json_schema" must have a valid json_schema string.')
            if fmt_type == "lark_grammar" and (
                not isinstance(response_format.get("lark_grammar"), str)
                or not response_format["lark_grammar"].strip()
            ):
                raise ValueError('ResponseFormat with type "lark_grammar" must have a valid lark_grammar string.')
        elif response_format.get("json_schema") or response_format.get("lark_grammar"):
            raise ValueError(
                f'ResponseFormat with type "{fmt_type}" should not have json_schema or lark_grammar properties.'
            )

    def _validate_tool_choice(self, tool_choice: dict[str, Any] | None) -> None:
        if tool_choice is None:
            return
        valid_types = ["none", "auto", "required", "function"]
        choice_type = tool_choice.get("type")
        if choice_type not in valid_types:
            raise ValueError(f"ToolChoice type must be one of: {', '.join(valid_types)}")
        if choice_type == "function" and (
            not isinstance(tool_choice.get("name"), str) or not tool_choice.get("name", "").strip()
        ):
            raise ValueError('ToolChoice with type "function" must have a valid name string.')
        elif choice_type != "function" and tool_choice.get("name"):
            raise ValueError(f'ToolChoice with type "{choice_type}" should not have a name property.')


@deprecated(
    "The OpenAI direct client is deprecated and will be removed at the end of 2026; use ChatSession. "
    "OpenAI types remain supported for the web-server path."
)
class ChatClient:
    """OpenAI-compatible chat completions client backed by Foundry Local Core.

    .. deprecated::
        The OpenAI direct client is deprecated and will be removed at the end of 2026; use
        :class:`foundry_local_sdk.session.ChatSession`. OpenAI types remain
        supported for the web-server path.

    Each call creates a fresh native session (stateless — no turn history).
    Supports non-streaming and streaming completions with optional tool calling.

    Attributes:
        model_id: The ID of the loaded model variant.
        settings: Tunable ``ChatClientSettings`` (temperature, max tokens, etc.).
    """

    def __init__(self, model_id: str, model: IModel) -> None:
        warnings.warn(
            "The OpenAI direct client is deprecated and will be removed at the end of 2026; use "
            "ChatSession. OpenAI types remain supported for the web-server path.",
            DeprecationWarning,
            stacklevel=2,
        )
        self.model_id = model_id
        # Hold the IModel reference so the underlying native model pointer
        # cannot be released out from under us.
        self._model = model
        self.settings = ChatClientSettings()

    def _validate_messages(self, messages: list[ChatCompletionMessageParam]) -> None:
        """Validate the messages list before sending to the native layer."""
        if not messages:
            raise ValueError("messages must be a non-empty list.")
        for i, msg in enumerate(messages):
            if not isinstance(msg, dict):
                raise ValueError(f"messages[{i}] must be a dict, got {type(msg).__name__}.")
            if "role" not in msg:
                raise ValueError(f"messages[{i}] is missing required key 'role'.")
            # Note: `content` is intentionally not required here. OpenAI allows assistant
            # messages that carry only `tool_calls` (no `content`), and tool/function
            # messages have their own shape. The native layer enforces per-role rules.

    def _validate_tools(self, tools: list[dict[str, Any]] | None) -> None:
        """Validate the tools list before sending to the native layer."""
        if not tools:
            return
        if not isinstance(tools, list):
            raise ValueError("tools must be a list if provided.")
        for i, tool in enumerate(tools):
            if not isinstance(tool, dict) or not tool:
                raise ValueError(
                    f"tools[{i}] must be a non-null object with a valid 'type' and 'function' definition."
                )
            if not isinstance(tool.get("type"), str) or not tool["type"].strip():
                raise ValueError(f"tools[{i}] must have a 'type' property that is a non-empty string.")
            fn = tool.get("function")
            if not isinstance(fn, dict):
                raise ValueError(f"tools[{i}] must have a 'function' property that is a non-empty object.")
            if not isinstance(fn.get("name"), str) or not fn["name"].strip():
                raise ValueError(
                    f"tools[{i}]'s function must have a 'name' property that is a non-empty string."
                )

    def _build_request_json(
        self,
        messages: list[ChatCompletionMessageParam],
        streaming: bool,
        tools: list[dict[str, Any]] | None = None,
    ) -> str:
        """Build the OpenAI-format JSON request string."""
        request_dict: dict[str, Any] = {
            "model": self.model_id,
            "messages": messages,
            **({"tools": tools} if tools else {}),
            **({"stream": True} if streaming else {}),
            **self.settings._serialize(),
        }
        return json.dumps(request_dict)

    def _run_native_request(self, request_json: str) -> str:
        """Create a fresh ChatSession, process the request, return the response JSON string."""
        from foundry_local_sdk.items import TextItem, TextItemType
        from foundry_local_sdk.request import Request
        from foundry_local_sdk.session import ChatSession

        with (
            ChatSession(self._model) as session,
            Request() as request,
        ):
            request.add_item(TextItem(request_json, TextItemType.OPENAI_JSON))
            with session.process_request(request) as response:
                # Copy the text out of the (response-owned) item before the response is released.
                return response.get_item(0).text

    def complete_chat(
        self,
        messages: list[ChatCompletionMessageParam],
        tools: list[dict[str, Any]] | None = None,
    ) -> ChatCompletion:
        """Perform a non-streaming chat completion.

        Args:
            messages: Conversation history as a list of OpenAI message dicts.
            tools: Optional list of tool definitions for function calling.

        Returns:
            A ``ChatCompletion`` response.

        Raises:
            ValueError: If messages is None, empty, or contains malformed entries.
            FoundryLocalException: If the native call returns an error.
        """
        self._validate_messages(messages)
        self._validate_tools(tools)

        request_json = self._build_request_json(messages, streaming=False, tools=tools)
        response_json = self._run_native_request(request_json)
        return ChatCompletion.model_validate_json(response_json)

    def complete_streaming_chat(
        self,
        messages: list[ChatCompletionMessageParam],
        tools: list[dict[str, Any]] | None = None,
    ) -> Generator[ChatCompletionChunk, None, None]:
        """Perform a streaming chat completion, yielding chunks as they arrive.

        Consume with a standard ``for`` loop::

            for chunk in client.complete_streaming_chat(messages):
                delta = chunk.choices[0].delta.content
                if delta:
                    print(delta, end="", flush=True)

        Args:
            messages: Conversation history as a list of OpenAI message dicts.
            tools: Optional list of tool definitions for function calling.

        Returns:
            A generator of ``ChatCompletionChunk`` objects.

        Raises:
            ValueError: If messages or tools are malformed.
            FoundryLocalException: If the native layer returns an error.
        """
        self._validate_messages(messages)
        self._validate_tools(tools)

        request_json = self._build_request_json(messages, streaming=True, tools=tools)

        from foundry_local_sdk.items import TextItem, TextItemType
        from foundry_local_sdk.request import Request
        from foundry_local_sdk.session import ChatSession

        with (
            ChatSession(self._model) as session,
            Request() as request,
        ):
            session.set_streaming(True)
            request.add_item(TextItem(request_json, TextItemType.OPENAI_JSON))
            for item in session.process_streaming_request(request):
                # Each item is a TextItem(OPENAI_JSON) — parse and normalize.
                raw = json.loads(item.text)

                # Foundry Local streams tool calls under "message" instead of the
                # standard "delta".  Normalize to "delta" so ChatCompletionChunk parses.
                for choice in raw.get("choices", []):
                    if "message" in choice and "delta" not in choice:
                        msg_obj = choice.pop("message")
                        # ChoiceDeltaToolCall requires "index"; add if absent.
                        for i, tc in enumerate(msg_obj.get("tool_calls", [])):
                            tc.setdefault("index", i)
                        choice["delta"] = msg_obj

                yield ChatCompletionChunk.model_validate(raw)
