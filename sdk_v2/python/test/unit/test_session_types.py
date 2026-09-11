# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# --------------------------------------------------------------------------
"""Unit tests for FinishReason, TokenUsage, RequestOptions, and ToolChoice."""
from __future__ import annotations

import dataclasses

import pytest

from foundry_local_sdk import (
    FinishReason,
    RequestOptions,
    SearchOptions,
    TokenUsage,
    ToolChoice,
)


class TestFinishReason:
    def test_values_match_native_c_enum(self):
        # Mirrors flFinishReason in foundry_local_c.h.
        assert FinishReason.NONE == 0
        assert FinishReason.ERROR == 1
        assert FinishReason.STOP == 2
        assert FinishReason.LENGTH == 3
        assert FinishReason.TOOL_CALLS == 4

    def test_int_construction_preserves_identity(self):
        assert FinishReason(2) is FinishReason.STOP

    def test_unknown_value_raises(self):
        with pytest.raises(ValueError):
            FinishReason(999)


class TestTokenUsage:
    def test_is_frozen_dataclass(self):
        u = TokenUsage(prompt_tokens=10, completion_tokens=20, total_tokens=30)
        with pytest.raises(dataclasses.FrozenInstanceError):
            u.prompt_tokens = 0  # type: ignore[misc]

    def test_field_round_trip(self):
        u = TokenUsage(prompt_tokens=1, completion_tokens=2, total_tokens=3)
        assert (u.prompt_tokens, u.completion_tokens, u.total_tokens) == (1, 2, 3)

    def test_equality_by_value(self):
        a = TokenUsage(1, 2, 3)
        b = TokenUsage(1, 2, 3)
        c = TokenUsage(1, 2, 4)
        assert a == b
        assert a != c


class TestToolChoice:
    def test_wire_values(self):
        # Must match the FOUNDRY_LOCAL_TOOL_CHOICE_* C++ enum's serialised form.
        assert ToolChoice.AUTO.value == "auto"
        assert ToolChoice.NONE.value == "none"
        assert ToolChoice.REQUIRED.value == "required"

    def test_str_enum_compat(self):
        # str subclass: usable directly as a dict key / equal to plain string.
        assert ToolChoice.AUTO == "auto"


class TestRequestOptions:
    def test_empty_options_produce_empty_dict(self):
        assert RequestOptions().to_native_options() == {}

    def test_typed_search_fields_emit_native_keys(self):
        opts = RequestOptions(
            search=SearchOptions(
                temperature=0.7,
                top_p=0.9,
                top_k=40,
                max_output_tokens=128,
                frequency_penalty=0.0,
                presence_penalty=0.0,
                seed=42,
                early_stopping=True,
                do_sample=False,
            )
        )
        native = opts.to_native_options()
        assert native["temperature"] == "0.7"
        assert native["top_p"] == "0.9"
        assert native["top_k"] == "40"
        assert native["max_output_tokens"] == "128"
        assert native["frequency_penalty"] == "0.0"
        assert native["presence_penalty"] == "0.0"
        assert native["seed"] == "42"
        # Bools must be lowercase to match the C++ "true"/"false" literal.
        assert native["early_stopping"] == "true"
        assert native["do_sample"] == "false"

    def test_tool_choice_emits_value_string(self):
        native = RequestOptions(tool_choice=ToolChoice.REQUIRED).to_native_options()
        assert native["tool_choice"] == "required"

    def test_additional_options_passthrough(self):
        native = RequestOptions(additional_options={"custom_key": "custom_value"}).to_native_options()
        assert native["custom_key"] == "custom_value"

    def test_typed_fields_win_over_additional_options(self):
        # Precedence: additional_options first, then typed search fields, then tool_choice.
        opts = RequestOptions(
            search=SearchOptions(temperature=0.0),
            tool_choice=ToolChoice.AUTO,
            additional_options={
                "temperature": "1.5",
                "tool_choice": "none",
                "untyped": "passthrough",
            },
        )
        native = opts.to_native_options()
        assert native["temperature"] == "0.0"
        assert native["tool_choice"] == "auto"
        assert native["untyped"] == "passthrough"

    def test_int_temperature_renders_as_integer_string(self):
        # str(0) -> "0" (not "0.0"). The native layer parses both.
        native = RequestOptions(search=SearchOptions(temperature=0)).to_native_options()
        assert native["temperature"] == "0"
