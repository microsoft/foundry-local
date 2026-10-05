# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum, IntEnum


class FinishReason(IntEnum):
    NONE = 0
    ERROR = 1
    STOP = 2
    LENGTH = 3
    TOOL_CALLS = 4


@dataclass(frozen=True)
class TokenUsage:
    prompt_tokens: int
    completion_tokens: int
    total_tokens: int


@dataclass(frozen=True)
class RequestPreflightResult:
    """Exact structural token budget. ``fits`` does not reserve cache or guarantee live Engine admission.

    ``context_limit_tokens`` is the Engine request capacity or Generator model context.
    """

    prompt_tokens: int
    output_reserve_tokens: int
    required_tokens: int
    context_limit_tokens: int
    fits: bool
    deficit_tokens: int


class _SessionParam:
    """Internal — well-known parameter key strings for the native KVP wire format. These mirror the
    ``FOUNDRY_LOCAL_PARAM_*`` macros in ``foundry_local_c.h``. Use the typed ``RequestOptions`` / ``SearchOptions`` API
    for these knobs; ``additional_options`` is the escape hatch for params not yet typed (and therefore not represented
    here).
    """

    Temperature = "temperature"
    TopP = "top_p"
    TopK = "top_k"
    MaxOutputTokens = "max_output_tokens"
    FrequencyPenalty = "frequency_penalty"
    PresencePenalty = "presence_penalty"
    Seed = "seed"
    EarlyStopping = "early_stopping"
    DoSample = "do_sample"
    ToolChoice = "tool_choice"


class ToolChoice(str, Enum):
    """Tool-choice mode for tool-enabled requests.

    Wire values match the ``FOUNDRY_LOCAL_TOOL_CHOICE_*`` enum on the C++ side: the native KVP receives the lowercase
    string.
    """

    AUTO = "auto"
    NONE = "none"
    REQUIRED = "required"


@dataclass
class SearchOptions:
    """Pure sampling / decoder knobs. Each field is optional — only non-``None`` values are forwarded to the C ABI."""

    temperature: float | None = None
    top_p: float | None = None
    top_k: int | None = None
    max_output_tokens: int | None = None
    frequency_penalty: float | None = None
    presence_penalty: float | None = None
    seed: int | None = None
    early_stopping: bool | None = None
    do_sample: bool | None = None


@dataclass
class RequestOptions:
    """Options to apply to all requests on a session (when passed to ``Session.set_options``) or to override session
    options for a single request (when passed to ``Request.set_options``)."""

    search: SearchOptions = field(default_factory=SearchOptions)
    tool_choice: ToolChoice | None = None
    # Passthrough for params not yet typed. Typed fields win on key collision (see ``to_native_options``).
    additional_options: dict[str, str] = field(default_factory=dict)

    def to_native_options(self) -> dict[str, str]:
        """Flatten this RequestOptions into the ``dict[str, str]`` the native KVP layer consumes.

        Precedence (later writes win):
          1. ``additional_options`` (raw passthrough).
          2. Typed ``SearchOptions`` fields.
          3. ``tool_choice``.

        ``str()`` on Python ``float``/``int`` uses the ``.`` decimal separator regardless of locale, matching what the
        C++ side produces with ``std::to_string``. Bools are written as lowercase ``"true"``/``"false"`` to match the
        C++ ``"true" : "false"`` literal, not Python's capitalised ``str(True)``.
        """
        out: dict[str, str] = {}

        # 1. Seed with additional_options so typed fields win on collision.
        for key, value in self.additional_options.items():
            out[key] = str(value)

        # 2. Layer typed SearchOptions fields.
        s = self.search
        if s.temperature is not None:
            out[_SessionParam.Temperature] = str(s.temperature)
        if s.top_p is not None:
            out[_SessionParam.TopP] = str(s.top_p)
        if s.top_k is not None:
            out[_SessionParam.TopK] = str(s.top_k)
        if s.max_output_tokens is not None:
            out[_SessionParam.MaxOutputTokens] = str(s.max_output_tokens)
        if s.frequency_penalty is not None:
            out[_SessionParam.FrequencyPenalty] = str(s.frequency_penalty)
        if s.presence_penalty is not None:
            out[_SessionParam.PresencePenalty] = str(s.presence_penalty)
        if s.seed is not None:
            out[_SessionParam.Seed] = str(s.seed)
        if s.early_stopping is not None:
            out[_SessionParam.EarlyStopping] = "true" if s.early_stopping else "false"
        if s.do_sample is not None:
            out[_SessionParam.DoSample] = "true" if s.do_sample else "false"

        # 3. Layer tool_choice.
        if self.tool_choice is not None:
            out[_SessionParam.ToolChoice] = self.tool_choice.value

        return out
